//
//  AX-VideoCaptureMSWImpl.h
//  AX-VideoCapture
//
//  Created by Andrew Wright (@axjxwright) on 21/04/25.
//  (c) 2025 AX Interactive (axinteractive.com.au)
//  
//

#pragma once

#include <mutex>
#include <queue>

#ifdef WIN32

#ifdef WINVER
    #undef WINVER
    #undef NTDDI_VERSION
#endif
#define WINVER _WIN32_WINNT_WIN10
#define NTDDI_VERSION NTDDI_WIN10

#include <wrl/client.h>
#include <mfapi.h>
#include <mfcaptureengine.h>

using namespace Microsoft::WRL;

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

#else

#error("Unsupported platform")

#endif

#include "AX-VideoCapture.h"

namespace AX::Video
{
    class  SharedTexture;
    struct SharedTextureDeleter { void operator() ( SharedTexture* ) const; };
    using  SharedTextureRef = std::unique_ptr<SharedTexture, SharedTextureDeleter>;

    class Capture::Impl : public IMFCaptureEngineOnSampleCallback, public IMFCaptureEngineOnEventCallback
    {
    public:
        Impl    ( Capture & owner, const Format& format );

        static std::vector<Capture::DeviceDescriptor> GetDevices ( bool refresh );
        
        const   ci::ivec2 &         GetSize ( ) const { return _format.Size(); }
        bool                        CheckNewFrame ( ) const { return _hasNewFrame.load ( ); }
        const   ci::Surface8uRef &  GetSurface ( ) const;
        Capture::FrameLeaseRef      GetTexture ( ) const;

        void                        Start ( );
        void                        Stop ( );
        bool                        IsStarted ( ) const;
        bool                        IsStopped ( ) const;
        bool                        IsValid ( ) const { return _isValid; }

        HRESULT STDMETHODCALLTYPE   OnEvent ( IMFMediaEvent* pEvent ) override;
        HRESULT STDMETHODCALLTYPE   OnSample ( IMFSample* pSample ) override;

        HRESULT STDMETHODCALLTYPE   QueryInterface ( REFIID riid, _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppvObject ) override;
        ULONG STDMETHODCALLTYPE     AddRef ( void ) override;
        ULONG STDMETHODCALLTYPE     Release ( void ) override;
        
        ~Impl ( );

    protected:
        
        Capture &                   _owner;
        Capture::Format             _format;
        ComPtr<IMFCaptureEngine>    _captureEngine{ nullptr };
        mutable std::atomic_bool    _hasNewFrame{ false };
        bool                        _isValid{ false };
        std::atomic_bool            _isStarted{ false };
        std::atomic_bool            _isInitialized{ false };
        
        ci::Surface8uRef            _surfaces[2];
        SharedTextureRef            _sharedTextures[2];
        int                         _readIndex{ 0 };
        int                         _writeIndex{ 1 };
        
    };
}
