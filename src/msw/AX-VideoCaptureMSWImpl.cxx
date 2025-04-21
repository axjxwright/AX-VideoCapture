//
//  AX-VideoCaptureMSWImpl.cxx
//  AX-VideoCapture
//
//  Created by Andrew Wright (@axjxwright) on 21/04/25.
//  (c) 2025 AX Interactive (axinteractive.com.au)
//  
//

#include "AX-VideoCaptureMSWImpl.h"

#if (CINDER_VERSION < 903)
    // Cinder 0.9.2 and younger used GLLoad
#include "glload/wgl_all.h"
#else
    // Cinder 0.9.3 and (presumably) going forward uses GLAD
#include "glad/glad_wgl.h"
#endif

#include "cinder/app/App.h"
#include "cinder/DataSource.h"
#include "cinder/msw/CinderMsw.h"
#include "cinder/Log.h"
#include "cinder/audio/Device.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

#include <mfapi.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <mfidl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <cfgmgr32.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "OneCoreUAP.lib")

using namespace ci;

namespace
{
    inline static std::string HRToString ( HRESULT hresult )
    {
        LPSTR errorText = nullptr;

        // @NOTE(andrew): Win32 is bananas.
        FormatMessageA ( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, hresult, MAKELANGID ( LANG_NEUTRAL, SUBLANG_DEFAULT ), (LPSTR)&errorText, 0, nullptr );

        if ( errorText != nullptr )
        {
            std::string result = errorText;
            LocalFree ( errorText );
            errorText = nullptr;

            return result;
        }

        return "Unknown error";
    }

    #define AX_PRINT_HR(level, x, hr) std::printf ( "%s returned HR: (%x) => %s\n", x, hr, HRToString ( hr ).c_str ( ) );

    #define AssertSucceeded(x) { HRESULT hr = (x); if ( !SUCCEEDED(hr) ) { AX_PRINT_HR ( AX_ERROR, #x, hr ); assert ( false && #x ); } }
    #define CheckSucceeded(x) [&] { HRESULT hr = (x); if ( !SUCCEEDED(hr) ) { AX_PRINT_HR ( AX_WARN, #x, hr ); return false; } return true; }()
    #define ReturnIfFailed(x) { hr = (x); if ( !SUCCEEDED(hr) ) { return hr; } }
    #define BailIfFailed(x) { HRESULT hr = (x); if ( !SUCCEEDED(hr) ) { return; } }
    #define ReturnFalseIfFailed(x) { HRESULT hr = (x); if ( !SUCCEEDED(hr) ) { AX_PRINT_HR ( AX_WARN, #x, hr ); return false; } }

    using SharedTexture = AX::Video::SharedTexture;
    using SharedTextureRef = AX::Video::SharedTextureRef;
    using InteropContextRef = std::unique_ptr<class InteropContext>;

    // @note(andrew): Have a single D3D + interop context for all capture sessions
    class InteropContext : public ci::Noncopyable
    {
    public:

        static void                     StaticInitialize ( const AX::Video::Capture::Format& format );
        static InteropContext&          Get ( );

        ~InteropContext ( );

        inline ID3D11Device*            Device ( ) const { return _device.Get ( ); }
        inline ID3D11DeviceContext*     DeviceContext ( ) const { return _deviceContext.Get ( ); }
        inline HANDLE                   Handle ( ) const { return _interopHandle; }
        inline IMFDXGIDeviceManager*    DXGIManager ( ) const { return _dxgiManager.Get ( ); }

        SharedTextureRef                CreateSharedTexture ( const ivec2& size );
        inline bool                     IsValid ( ) const { return _isValid; }

    protected:

        InteropContext ( const AX::Video::Capture::Format& format );

        ComPtr<ID3D11Device>            _device{ nullptr };
        ComPtr<ID3D11DeviceContext>     _deviceContext{ nullptr };
        ComPtr<IMFDXGIDeviceManager>    _dxgiManager{ nullptr };

        HANDLE                          _interopHandle{ nullptr };
        bool                            _isValid{ false };
    };
}

namespace AX::Video
{
    class SharedTexture
    {
    public:

        SharedTexture               ( const ivec2& size );
        ~SharedTexture              ( );

        bool                        Lock ( );
        bool                        Unlock ( );
        inline bool                 IsLocked ( ) const { return _isLocked; }

        inline bool                 IsValid ( ) const { return _isValid; }
        ID3D11Texture2D*            DXTextureHandle ( ) const { return _dxTexture.Get ( ); }
        const ci::gl::TextureRef&   GLTextureHandle ( ) const { return _glTexture; }

    protected:

        ci::gl::TextureRef          _glTexture;
        ComPtr<ID3D11Texture2D>     _dxTexture{ nullptr };
        HANDLE                      _shareHandle{ nullptr };
        bool                        _isValid{ false };
        bool                        _isLocked{ false };
    };

    SharedTexture::SharedTexture ( const ivec2& size )
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = size.x;
        desc.Height = size.y;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8X8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.Usage = D3D11_USAGE_DEFAULT;

        auto& context = InteropContext::Get ( );

        if ( SUCCEEDED ( context.Device ( )->CreateTexture2D ( &desc, nullptr, _dxTexture.GetAddressOf ( ) ) ) )
        {
            gl::Texture::Format fmt;
            fmt.internalFormat ( GL_RGBA ).loadTopDown ( );

            _glTexture = gl::Texture::create ( size.x, size.y, fmt );
            _shareHandle = wglDXRegisterObjectNV ( context.Handle ( ), _dxTexture.Get ( ), _glTexture->getId ( ), GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV );
            _isValid = _shareHandle != nullptr;
        }
    }

    bool SharedTexture::Lock ( )
    {
        assert ( !IsLocked ( ) );
        _isLocked = wglDXLockObjectsNV ( InteropContext::Get ( ).Handle ( ), 1, &_shareHandle );
        return _isLocked;
    }

    bool SharedTexture::Unlock ( )
    {
        assert ( IsLocked ( ) );
        if ( wglDXUnlockObjectsNV ( InteropContext::Get ( ).Handle ( ), 1, &_shareHandle ) )
        {
            _isLocked = false;
            return true;
        }

        return false;
    }

    SharedTexture::~SharedTexture ( )
    {
        if ( _shareHandle != nullptr )
        {
            if ( wglGetCurrentContext ( ) != nullptr ) // No GL Context, so we're likely in the process of shutting down
            {
                if ( IsLocked ( ) ) wglDXUnlockObjectsNV ( InteropContext::Get ( ).Handle ( ), 1, &_shareHandle );
                wglDXUnregisterObjectNV ( InteropContext::Get ( ).Handle ( ), _shareHandle );
                _shareHandle = nullptr;
            }
        }
    }

    struct ControlMSW : public AX::Video::Capture::Control
    {
        ControlMSW ( const std::string& name, const ComPtr<IKsControl>& control, int key, GUID set = PROPSETID_VIDCAP_VIDEOPROCAMP )
            : _control ( control )
            , _set ( set )
            , _key ( key )
        {
            _name = name;
        }

        void InitState ( )
        {
            typedef struct _KsControlMemberList
            {
                KSPROPERTY_DESCRIPTION desc;
                KSPROPERTY_MEMBERSHEADER hdr;
                KSPROPERTY_STEPPING_LONG step;
            } KsControlMemberList, * PKsControlMemberList;

            typedef struct _KsControlDefaultValue
            {
                KSPROPERTY_DESCRIPTION desc;
                KSPROPERTY_MEMBERSHEADER hdr;
                long lValue;
            } KsControlDefaultValue;

            HRESULT hr = S_OK;
            KsControlMemberList ksMemList = {};
            KsControlDefaultValue ksDefault = {};
            KSPROPERTY_CAMERACONTROL_S ksProp = {};
            ULONG capability = 0;
            ULONG cbReturned = 0;

            ksProp.Property.Set = _set;
            ksProp.Property.Id = _key;
            ksProp.Property.Flags = KSPROPERTY_TYPE_BASICSUPPORT;

            BailIfFailed ( _control->KsProperty ( (PKSPROPERTY)&ksProp, sizeof ( ksProp ), &ksMemList, sizeof ( ksMemList ), &cbReturned ) );

            ksProp.Property.Flags = KSPROPERTY_TYPE_DEFAULTVALUES;
            BailIfFailed ( _control->KsProperty ( (PKSPROPERTY)&ksProp, sizeof ( ksProp ), &ksDefault, sizeof ( ksDefault ), &cbReturned ) );

            _value = LoadValue ( );
            _isSupported = true;

            _min = ksMemList.step.Bounds.SignedMinimum;
            _max = ksMemList.step.Bounds.SignedMaximum;
            _step = ksMemList.step.SteppingDelta;
            _default = ksDefault.lValue;
        }

        void StoreValue ( int32_t value ) override
        {
            KSPROPERTY_CAMERACONTROL_S videoControl = {};
            videoControl.Property.Set = _set;
            videoControl.Property.Id = _key;
            videoControl.Property.Flags = KSPROPERTY_TYPE_SET;
            videoControl.Value = _value = value;
            ULONG retSize = 0;

            CheckSucceeded ( _control->KsProperty ( (PKSPROPERTY)&videoControl, sizeof ( videoControl ), &videoControl, sizeof ( videoControl ), &retSize ) );
        }

        int32_t LoadValue ( ) override
        {
            KSPROPERTY_CAMERACONTROL_S videoControl = {};
            videoControl.Property.Set = _set;
            videoControl.Property.Id = _key;
            videoControl.Property.Flags = KSPROPERTY_TYPE_GET;
            videoControl.Value = -1;
            ULONG retSize = 0;

            CheckSucceeded ( _control->KsProperty ( (PKSPROPERTY)&videoControl, sizeof ( videoControl ), &videoControl, sizeof ( videoControl ), &retSize ) );

            _value = videoControl.Value;
            return _value;
        }

    protected:
        ComPtr<IKsControl>  _control;
        GUID                _set{ PROPSETID_VIDCAP_VIDEOPROCAMP };
        int                 _key{ KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS };
    };

    void SharedTextureDeleter::operator ( ) ( SharedTexture* ptr ) const
    {
        std::default_delete<SharedTexture> deleter;
        deleter ( ptr );
    }

    class DXGIRenderPathFrameLease : public AX::Video::Capture::FrameLease
    {
    public:

        DXGIRenderPathFrameLease ( const SharedTextureRef& texture )
            : _texture ( texture.get ( ) )
        {
            if ( _texture ) _texture->Lock ( );
        }

        inline bool    IsValid ( ) const override { return ToTexture ( ) != nullptr; }
        gl::TextureRef ToTexture ( ) const override { return _texture ? _texture->GLTextureHandle ( ) : nullptr; };

        ~DXGIRenderPathFrameLease ( )
        {
            if ( _texture && _texture->IsLocked ( ) )
            {
                _texture->Unlock ( );
                _texture = nullptr;
            }
        }

    protected:

        SharedTexture* _texture{ nullptr };
    };

    static void DispatchDeviceChangeSignals ( )
    {
        auto previous = AX::Video::Capture::GetDevices ( );
        auto current = AX::Video::Capture::GetDevices ( true );

        for ( auto& p : previous )
        {
            if ( std::find ( current.begin ( ), current.end ( ), p ) == current.end ( ) )
            {
                AX::Video::Capture::OnDeviceRemoved ( ).emit ( p );
            }
        }

        for ( auto& c : current )
        {
            if ( std::find ( previous.begin ( ), previous.end ( ), c ) == previous.end ( ) )
            {
                AX::Video::Capture::OnDeviceAdded ( ).emit ( c );
            }
        }
    }
}

namespace
{
    DWORD CALLBACK OnDeviceNotify ( HCMNOTIFICATION hNotify, PVOID Context, CM_NOTIFY_ACTION Action, PCM_NOTIFY_EVENT_DATA EventData, DWORD EventDataSize )
    {
        auto& di = EventData->u.DeviceInstance;
        
        switch ( Action )
        {
            case CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL :
            {
                AX::Video::DispatchDeviceChangeSignals ( );
                break;
            }

            case CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL:
            {
                AX::Video::DispatchDeviceChangeSignals ( );
                break;
            }
        }
        
        return ERROR_SUCCESS;
    }
    
    static struct Notifier
    {
        HCMNOTIFICATION handle{ nullptr };
        
        Notifier ( )
        {
            CM_NOTIFY_FILTER filter{};
            filter.cbSize = sizeof ( filter );
            filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
            filter.Flags = 0;
            filter.u.DeviceInterface.ClassGuid = KSCATEGORY_CAPTURE;
            CM_Register_Notification ( &filter, this, OnDeviceNotify, &handle );
        }

        ~Notifier ( )
        {
            CM_Unregister_Notification ( handle );
        }
        
    } kNotifier;

    static std::atomic_int kNumMediaFoundationInstances = 0;
    static std::atomic_bool kIsMFInitialized = false;

    using MFCreateCaptureEngineFn = HRESULT ( * ) ( IMFCaptureEngine** );

    struct Lib
    {
        Lib ( const fs::path& path )
        {
            _handle = LoadLibraryA ( path.string ( ).c_str ( ) );
        }

        template <typename T>
        T GetFunction ( const std::string& name )
        {
            return (T)( GetProcAddress ( _handle, name.c_str ( ) ) );
        }

        ~Lib ( )
        {
            FreeLibrary ( _handle );
        }

        HMODULE _handle{ nullptr };
    };

    template <typename T>
    std::wstring GUIDToString ( const T& guid )
    {
        wchar_t* string = nullptr;
        std::wstring result = L"{Unknown}";
        if ( CheckSucceeded ( StringFromIID ( guid, &string ) ) )
        {
            result = string;
            CoTaskMemFree ( string );
        }
        return result;
    }

    template <typename T>
    struct ComArray : public ci::Noncopyable
    {
        ~ComArray ( )
        {
            for ( uint32_t i = 0; i < Count; i++ )
            {
                if ( Data[i] ) Data[i]->Release ( );
            }
            CoTaskMemFree ( Data );
            Data = nullptr;
        }

        T* operator[] ( int index ) { return Data[index]; };
        const T* operator[] ( int index ) const { return Data[index]; };

        uint32_t Count{ 0 };
        T** Data{ nullptr };
    };
   
    static InteropContext* kInteropContext{ nullptr };

    static void OnCaptureCreated ( )
    {
        if ( kNumMediaFoundationInstances++ == 0 )
        {
            kIsMFInitialized = SUCCEEDED ( MFStartup ( MF_VERSION ) );
        }
    }

    static void OnCaptureDestroyed ( )
    {
        if ( --kNumMediaFoundationInstances == 0 )
        {
            delete kInteropContext;
            kInteropContext = nullptr;
            MFShutdown ( );
            kIsMFInitialized = false;
        }
    }

    void InteropContext::StaticInitialize ( const AX::Video::Capture::Format& format )
    {
        if ( !kInteropContext )
        {
            kInteropContext = new InteropContext ( format );
        }
    }

    InteropContext& InteropContext::Get ( )
    {
        assert ( kInteropContext );
        return *kInteropContext;
    }

    InteropContext::InteropContext ( const AX::Video::Capture::Format& format )
        : _isValid ( false )
    {
        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

#ifndef NDEBUG
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        UINT resetToken{ 0 };
        BailIfFailed ( MFCreateDXGIDeviceManager ( &resetToken, _dxgiManager.GetAddressOf ( ) ) );
        BailIfFailed ( D3D11CreateDevice ( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr, 0, D3D11_SDK_VERSION, _device.GetAddressOf ( ), nullptr, _deviceContext.GetAddressOf ( ) ) );

        ComPtr<ID3D10Multithread> multiThread{ nullptr };
        BailIfFailed ( _device->QueryInterface ( multiThread.GetAddressOf ( ) ) );
        multiThread->SetMultithreadProtected ( true );
        
        BailIfFailed ( _dxgiManager->ResetDevice ( _device.Get ( ), resetToken ) );

        _interopHandle = wglDXOpenDeviceNV ( _device.Get ( ) );
        _isValid = _interopHandle != nullptr;
    }

    SharedTextureRef InteropContext::CreateSharedTexture ( const ivec2& size )
    {
        static AX::Video::SharedTextureDeleter kSharedTextureDeleter;

        auto texture = SharedTextureRef ( new SharedTexture ( size ), kSharedTextureDeleter );
        if ( texture->IsValid ( ) ) return std::move ( texture );

        return nullptr;
    }

    InteropContext::~InteropContext ( )
    {
        if ( _interopHandle != nullptr )
        {
            wglDXCloseDeviceNV ( _interopHandle );
            _interopHandle = nullptr;
        }

        _dxgiManager = nullptr;

        // @leak(andrew): Debug layer is whinging about live objects but is this 
        // this because the ComPtr destructors haven't had a chance to fire yet?
#ifndef NDEBUG
        if ( _device )
        {
            ComPtr<ID3D11Debug> debug{ nullptr };
            if ( CheckSucceeded ( _device->QueryInterface ( debug.GetAddressOf ( ) ) ) )
            {
                debug->ReportLiveDeviceObjects ( D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL );
            }
        }
#endif
    }
    
}

namespace AX::Video
{
    static Lib kCaptureLib{ "MFCaptureEngine.dll" };

    static std::vector<Capture::DeviceDescriptor> kCaptureDevices;
    std::vector<Capture::DeviceDescriptor> Capture::Impl::GetDevices ( bool refresh )
    {
        if ( kCaptureDevices.empty ( ) || refresh )
        {
            ComPtr<IMFAttributes> attributes;
            MFCreateAttributes ( &attributes, 1 );
            CheckSucceeded ( attributes->SetGUID ( MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID ) );

            ComArray<IMFActivate> activates{};
            CheckSucceeded ( MFEnumDeviceSources ( attributes.Get ( ), &activates.Data, &activates.Count ) );

            kCaptureDevices.clear ( );
            kCaptureDevices.reserve ( activates.Count );

            for ( uint32_t i = 0; i < activates.Count; i++ )
            {
                Capture::DeviceDescriptor device;
                device.Name = "<Unknown Device>";
                wchar_t buffer[128] = {};
                UINT32 length{ 0 };
                if ( CheckSucceeded ( activates[i]->GetString ( MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, buffer, 128, &length ) ) )
                {
                    std::wstring w = buffer;
                    device.Name = msw::toUtf8String ( w );
                }

                if ( CheckSucceeded ( activates[i]->GetString ( MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, buffer, 128, &length ) ) )
                {
                    auto w = buffer;
                    device.ID = msw::toUtf8String ( w );
                }

                kCaptureDevices.push_back ( device );
            }
        }

        return kCaptureDevices;
    }

    ComPtr<IMFMediaSource> FindDeviceSource ( const Capture::DeviceDescriptor& descriptor )
    {
        ComPtr<IMFAttributes> attributes;
        MFCreateAttributes ( &attributes, 1 );
        CheckSucceeded ( attributes->SetGUID ( MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID ) );

        ComArray<IMFActivate> activates{};
        CheckSucceeded ( MFEnumDeviceSources ( attributes.Get ( ), &activates.Data, &activates.Count ) );

        kCaptureDevices.clear ( );
        kCaptureDevices.reserve ( activates.Count );

        for ( uint32_t i = 0; i < activates.Count; i++ )
        {
            wchar_t buffer[128] = {};
            UINT32 length{ 0 };
                
            if ( CheckSucceeded ( activates[i]->GetString ( MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, buffer, 128, &length ) ) )
            {
                auto w = buffer;
                if ( descriptor.ID == msw::toUtf8String ( w ) )
                {
                    ComPtr<IMFMediaSource> source;
                    if ( CheckSucceeded ( activates[i]->ActivateObject ( IID_PPV_ARGS ( &source ) ) ) )
                    {
                        return source;
                    }
                }
            }
        }

        return nullptr;
    }

    Capture::Impl::Impl ( Capture & owner, const Format& format )
        : _owner ( owner )
        , _format( format )
    {
        OnCaptureCreated ();
        
        MFCreateCaptureEngineFn MFCreateCaptureEngine = kCaptureLib.GetFunction<MFCreateCaptureEngineFn> ( "MFCreateCaptureEngine" );
        BailIfFailed ( MFCreateCaptureEngine ( _captureEngine.GetAddressOf ( ) ) );

        if ( auto source = FindDeviceSource ( format.Device() ) )
        {
            {
                ComPtr<IKsControl> control;
                CheckSucceeded ( source->QueryInterface ( control.GetAddressOf ( ) ) );

                std::unordered_map<std::string, std::pair<int, GUID>> keys =
                {
                    { "Brightness", { KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Contrast", { KSPROPERTY_VIDEOPROCAMP_CONTRAST, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Hue", { KSPROPERTY_VIDEOPROCAMP_HUE, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Saturation", { KSPROPERTY_VIDEOPROCAMP_SATURATION, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Sharpness", { KSPROPERTY_VIDEOPROCAMP_SHARPNESS, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Gamma", { KSPROPERTY_VIDEOPROCAMP_GAMMA, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Color Enable", { KSPROPERTY_VIDEOPROCAMP_COLORENABLE, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "White Balance", { KSPROPERTY_VIDEOPROCAMP_WHITEBALANCE, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Backlight Compensation", { KSPROPERTY_VIDEOPROCAMP_BACKLIGHT_COMPENSATION, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Gain", { KSPROPERTY_VIDEOPROCAMP_GAIN, PROPSETID_VIDCAP_VIDEOPROCAMP } },
                    { "Zoom", { KSPROPERTY_CAMERACONTROL_ZOOM, PROPSETID_VIDCAP_CAMERACONTROL } },
                    { "Focus", { KSPROPERTY_CAMERACONTROL_FOCUS, PROPSETID_VIDCAP_CAMERACONTROL } }

                };

                for ( auto& [name, key] : keys )
                {
                    auto ctrl = std::make_unique<ControlMSW> ( name, control, key.first, key.second );
                    ctrl->InitState ( );
                    if ( ctrl->IsSupported() )
                    {
                        _owner._controls.push_back ( std::move ( ctrl ) );
                    }
                }
            }

            ComPtr<IMFAttributes> attributes;
            MFCreateAttributes ( &attributes, 3 );
            attributes->SetUINT32 ( MF_CAPTURE_ENGINE_USE_VIDEO_DEVICE_ONLY, 1 );

            bool hardware = format.IsHardwareAccelerated ( );
            if ( !hardware ) attributes->SetUINT32 ( MF_CAPTURE_ENGINE_DISABLE_HARDWARE_TRANSFORMS, 1 );

            if ( hardware )
            {
                InteropContext::StaticInitialize ( _format );
                auto& ic = InteropContext::Get ( );

                _sharedTextures[0] = ic.CreateSharedTexture ( _format.Size ( ) );
                _sharedTextures[1] = ic.CreateSharedTexture ( _format.Size ( ) );

                if ( !_sharedTextures[0] || !_sharedTextures[1] )
                {
                    std::printf ( "Error allocating shared textures\n" );
                    _isValid = false;
                    return;
                }


                attributes->SetUnknown ( MF_CAPTURE_ENGINE_D3D_MANAGER, ic.DXGIManager() );
            }

            BailIfFailed ( _captureEngine->Initialize ( this, attributes.Get ( ), nullptr, source.Get ( ) ) );
            _isValid = true;
        }
    }

    void Capture::Impl::Start ( )
    {
        if ( !_isInitialized ) return;
        if ( IsStarted ( ) ) return;
        _isStarted.store ( true );
        CheckSucceeded ( _captureEngine->StartPreview ( ) );
    }

    void Capture::Impl::Stop ( )
    {
        if ( IsStopped ( ) ) return;
        _isStarted.store ( false );
        CheckSucceeded ( _captureEngine->StopPreview ( ) );
    }

    bool Capture::Impl::IsStarted ( ) const
    {
        return _isStarted.load ( );
    }

    bool Capture::Impl::IsStopped ( ) const
    {
        return !IsStarted ( );
    }

    const Surface8uRef & Capture::Impl::GetSurface ( ) const
    {
        _hasNewFrame.store ( false );
        return _surfaces[_readIndex];
    }

    Capture::FrameLeaseRef Capture::Impl::GetTexture ( ) const
    {
        _hasNewFrame.store ( false );
        return std::make_unique<DXGIRenderPathFrameLease> ( _sharedTextures[_readIndex] );
    }

    HRESULT STDMETHODCALLTYPE Capture::Impl::OnEvent ( IMFMediaEvent* pEvent )
    {
        auto threadId = GetCurrentThreadId ( );
        MediaEventType type{};
        pEvent->GetType ( &type );
        if ( type == MEExtendedType )
        {
            GUID extendedType;
            pEvent->GetExtendedType ( &extendedType );
            if ( extendedType == MF_CAPTURE_ENGINE_INITIALIZED )
            {
                ComPtr<IMFCaptureSink> sink;
                ComPtr<IMFCapturePreviewSink> previewSink;
                HRESULT hr;
                ReturnIfFailed ( _captureEngine->GetSink ( MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW, sink.GetAddressOf ( ) ) );
                ReturnIfFailed ( sink->QueryInterface ( previewSink.GetAddressOf ( ) ) );

                DWORD streamIndex = -1;

                ComPtr<IMFMediaType> streamType;
                MFCreateMediaType ( &streamType );

                CheckSucceeded ( streamType->SetGUID ( MF_MT_MAJOR_TYPE, MFMediaType_Video ) );
                CheckSucceeded ( streamType->SetGUID ( MF_MT_SUBTYPE, MFVideoFormat_RGB32 ) );
                CheckSucceeded ( MFSetAttributeRatio ( streamType.Get ( ), MF_MT_FRAME_RATE, _format.FPS().x, _format.FPS().y ) );
                CheckSucceeded ( MFSetAttributeSize ( streamType.Get ( ), MF_MT_FRAME_SIZE, _format.Size().x, _format.Size().y ) );

                CheckSucceeded ( previewSink->AddStream ( 0, streamType.Get ( ), nullptr, &streamIndex ) );
                CheckSucceeded ( previewSink->SetSampleCallback ( 0, this ) );
                CheckSucceeded ( previewSink->SetRotation ( 0, (int)_format.RotationAngle() * 90 ) );

                _isInitialized.store ( true );
                app::App::get ( )->dispatchAsync ( [=] { _owner.OnInitialize.emit ( ); } );
                
                if ( _format.AutoStart ( ) )
                {
                    Start ( );
                }

            } else if ( extendedType == MF_CAPTURE_ENGINE_PREVIEW_STARTED )
            {
                app::App::get ( )->dispatchAsync ( [=] { _owner.OnStart.emit ( ); } );

            } else if ( extendedType == MF_CAPTURE_ENGINE_PREVIEW_STOPPED )
            {
                app::App::get ( )->dispatchAsync ( [=] { _owner.OnStop.emit ( ); } );
            } else if ( extendedType == MF_CAPTURE_ENGINE_ERROR )
            {
                HRESULT status{};
                CheckSucceeded ( pEvent->GetStatus ( &status ) );
                switch ( status )
                {
                    case MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED :
                    {
                        app::App::get ( )->dispatchAsync ( [=] 
                        { 
                            _isInitialized = false; 
                            _isStarted.store ( false ); // @NOTE(andrew): Don't call ::Stop() here or it'll trigger some async events to fire
                                                        // likely after the client has destroyed their Capture instance as a result of the lost device.
                            _owner.OnDeviceLost.emit ( ); 
                        } );
                        break;
                    }

                    default :
                    {
                        app::App::get ( )->dispatchAsync ( [=] { _owner.OnError.emit ( status ); } );
                    }
                }
                
            } else
            {
                std::printf ( "  Unhandled Event: %S\n", GUIDToString ( extendedType ).c_str ( ) );
            }
        }
        
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Capture::Impl::OnSample ( IMFSample* sample )
    {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr;
        ReturnIfFailed ( sample->GetBufferByIndex ( 0, &buffer ) );

        if ( _format.IsHardwareAccelerated ( ) )
        {
            ComPtr<IMFDXGIBuffer> dxgiBuffer;
            if ( SUCCEEDED ( buffer.As ( &dxgiBuffer ) ) )
            {
                ComPtr<ID3D11Texture2D> texture;
                ReturnIfFailed ( dxgiBuffer->GetResource ( IID_PPV_ARGS ( &texture ) ) );

                D3D11_TEXTURE2D_DESC desc;
                texture->GetDesc ( &desc );

                auto& ic = InteropContext::Get ( );
                ic.DeviceContext ( )->CopyResource ( _sharedTextures[_writeIndex]->DXTextureHandle ( ), texture.Get ( ) );

                std::swap ( _readIndex, _writeIndex );
                return S_OK;
            }
        } else
        {
            ComPtr<IMFMediaBuffer> mediaBuffer = nullptr;
            DWORD bmpLength = 0;
            BYTE* bmpBuffer = nullptr;

            ReturnIfFailed ( sample->ConvertToContiguousBuffer ( &mediaBuffer ) );
            ReturnIfFailed ( mediaBuffer->Lock ( &bmpBuffer, NULL, &bmpLength ) );

            auto& surface = _surfaces[_writeIndex];
            auto allocatedSurfaceBytes = surface ? surface->getRowBytes ( ) * surface->getHeight ( ) : 0;
            if ( !surface || allocatedSurfaceBytes < bmpLength )
            {
                surface = Surface::create ( _format.Size ( ).x, _format.Size ( ).y, _format.Size ( ).x * 4, SurfaceChannelOrder::BGRA );
            }
            
            std::memcpy ( surface->getData ( ), bmpBuffer, bmpLength );
            CheckSucceeded ( mediaBuffer->Unlock ( ) );
            std::swap ( _readIndex, _writeIndex );                           
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Capture::Impl::QueryInterface ( REFIID riid, _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppvObject )
    {
        if ( riid == __uuidof ( IMFCaptureEngineOnSampleCallback ) )
        {
            *ppvObject = this;
            return S_OK;
        } else if ( riid == __uuidof ( IMFCaptureEngineOnEventCallback ) )
        {
            *ppvObject = this;
            return S_OK;
        } else
        {
            *ppvObject = nullptr;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE Capture::Impl::AddRef ( void )
    {
        return 1;
    }

    ULONG STDMETHODCALLTYPE Capture::Impl::Release ( void )
    {
        return 1;
    }

    Capture::Impl::~Impl ( )
    {
        _hasNewFrame.store ( false );
        _captureEngine = nullptr;
        _sharedTextures[0].reset ( );
        _sharedTextures[1].reset ( );
        
        OnCaptureDestroyed ( );
    }
}