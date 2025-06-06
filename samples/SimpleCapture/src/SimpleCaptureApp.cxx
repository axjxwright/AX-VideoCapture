//
//  SimpleCaptureApp.cxx
//  SimpleCaptureApp
//
//  Created by Andrew Wright on 21/04/25.
//  (c) 2025 AX Interactive
//

#include "cinder/app/RendererGl.h"
#include "cinder/app/App.h"
#include "cinder/gl/gl.h"
#include "cinder/audio/audio.h"
#include "AX-VideoCapture.h"
#include <optional>

#if __has_include( "cinder/CinderImGui.h")
    #include "cinder/CinderImGui.h"
    #define HAS_DEBUG_UI
    namespace ui = ImGui;
#endif

#ifdef CINDER_MSW
    #define FORCE_NVIDIA_CARD_IF_PRESENT
#endif

#ifdef FORCE_NVIDIA_CARD_IF_PRESENT
extern "C" 
{
    __declspec(dllexport) int NvOptimusEnablement = 0x00000001; // This forces Intel GPU / Nvidia card selection
}
#endif

using namespace ci;
using namespace ci::app;

class SimpleCaptureApp : public app::App
{
public:
    void setup ( ) override;
    void update ( ) override;
    void draw ( ) override;
    
protected:

    using DeviceDescriptor  = AX::Video::Capture::DeviceDescriptor;
    using DeviceProfile     = AX::Video::Capture::DeviceProfile;
    
    void                                MakeCapture ( const DeviceDescriptor& device, std::optional<DeviceProfile> profile = {} );

    AX::Video::CaptureRef               _capture;
    bool                                _hardwareAccelerated{ true };
    gl::TextureRef                      _texture;
    AX::Video::Capture::Rotation        _rotation{ AX::Video::Capture::Rotation::R0 };
    std::vector<AX::Video::Capture::DeviceProfile> _profiles;
};

void SimpleCaptureApp::setup ( )
{
#ifdef HAS_DEBUG_UI
    ui::Initialize ( );
#endif

    console() << gl::getString(GL_RENDERER) << std::endl;
    console() << gl::getString(GL_VERSION) << std::endl;

    if ( !AX::Video::Capture::GetDevices ( ).empty ( ) )
    {
        MakeCapture ( AX::Video::Capture::GetDevices ( ).back ( ) );
    }

    AX::Video::Capture::OnDeviceAdded ( ).connect ( [=]( AX::Video::Capture::DeviceDescriptor device )
    {
        std::cout << device << " added!\n";
    } );

    AX::Video::Capture::OnDeviceRemoved ( ).connect ( [=]( AX::Video::Capture::DeviceDescriptor device )
    {
        std::cout << device << " removed!\n";
    } );
    
}

void SimpleCaptureApp::MakeCapture ( const AX::Video::Capture::DeviceDescriptor& device, std::optional<DeviceProfile> profile )
{
    if ( !profile ) _profiles = AX::Video::Capture::GetProfiles ( device );

    AX::Video::Capture::Format fmt;
    fmt.HardwareAccelerated ( _hardwareAccelerated )
       .Size ( { 1280, 720 } )
       .FPS ( 60 )
       .RotationAngle ( _rotation )
       .Device ( device );

    if ( profile ) fmt.Profile ( *profile );

    _capture = AX::Video::Capture::Create ( fmt );
    _capture->OnStart.connect ( [] { std::cout << "Device started.\n"; } );
    _capture->OnStop.connect ( [] { std::cout << "Device stopped.\n"; } );
    _capture->OnControlChanged.connect ( []( const AX::Video::Capture::Control& control )
    {
        std::printf ( "Device control '%s' is now %d\n", control.Name ( ).c_str ( ), control.Value ( ) );
    } );
    _capture->OnOcclusionChanged.connect ( [=]( AX::Video::Capture::OcclusionState state )
    {
        switch ( state )
        {
            case AX::Video::Capture::OcclusionState::Open:
            {
                std::printf ( "OcclusionState: Open\n" );
                break;
            }

            case AX::Video::Capture::OcclusionState::OccludedByLid:
            {
                std::printf ( "OcclusionState: OccludedByLid\n" );
                break;
            }

            case AX::Video::Capture::OcclusionState::OccludedByHardware:
            {
                std::printf ( "OcclusionState: OccludedByHardware\n" );
                break;
            }
        }
    } );
    _capture->OnDeviceLost.connect ( [=] 
    { 
        std::cout << "Device lost.\n"; 
        // Destroy the CaptureRef next frame, doing so from
        // directly within the event signal would crash.
        dispatchAsync ( [=] { _capture = nullptr; } );
    } );
}

void SimpleCaptureApp::update ( )
{
    if ( _capture )
    {
        if ( auto surf = _capture->GetSurface ( ) )
        {
            // note(andrew): loadTopDown() prevents a very expensive vertical flip in gl::Texture2d::setData
            _texture = gl::Texture::create ( *surf, gl::Texture::Format().loadTopDown() );
        }
    }
}

#ifdef HAS_DEBUG_UI
struct ScopedWindow2
{
    ScopedWindow2 ( const char * title, uint32_t flags )
    {
        ui::Begin ( title, nullptr, flags );
    }

    ~ScopedWindow2 ( )
    {
        ui::End ( );
    }
};
#endif

void SimpleCaptureApp::draw ( )
{
    gl::clear ( Colorf::gray ( 0.2f ) );

#ifdef HAS_DEBUG_UI
    {
        ScopedWindow2 window{ "Settings", ImGuiWindowFlags_AlwaysAutoResize };
        if ( ui::Checkbox ( "Hardware Accelerated", &_hardwareAccelerated ) )
        {
            if ( _capture )
            {
                std::optional<DeviceProfile> profile = DeviceProfile{ _capture->GetSize ( ), _capture->GetFormat ( ).FPS ( ) };
                MakeCapture ( _capture->GetDevice ( ), profile );
            }
        }

        std::string prompt = _capture ? _capture->GetDevice ( ).Name : "<No device>";
        if ( ui::BeginCombo ( "Camera", prompt.c_str ( ) ) )
        {
            for ( auto& device : AX::Video::Capture::GetDevices ( ) )
            {
                if ( ui::Selectable ( device.Name.c_str ( ) ) )
                {
                    std::optional<DeviceProfile> profile;
                    if ( _capture && _capture->GetDevice() == device ) profile = DeviceProfile{ _capture->GetSize ( ), _capture->GetFormat ( ).FPS ( ) };

                    MakeCapture ( device, profile );
                }
            }
            ui::EndCombo ( );
        }

        if ( _capture )
        {
            AX::Video::Capture::DeviceProfile profile{ _capture->GetSize ( ), _capture->GetFormat ( ).FPS() };
            if ( ui::BeginCombo ( "Profile", profile.Key().c_str() ) )
            {
                for ( auto& profile : _profiles )
                {
                    if ( ui::Selectable ( profile.Key ( ).c_str ( ) ) )
                    {
                        MakeCapture ( _capture->GetDevice ( ), profile );
                    }
                }
                ui::EndCombo ( );
            }

            float fps = (float)_capture->GetFormat ( ).FPS ( ).x / (float)_capture->GetFormat ( ).FPS ( ).y;
            ui::Text ( "Device: %s (%dx%d@%g) %s", _capture->GetDevice ( ).Name.c_str ( ), _capture->GetSize ( ).x, _capture->GetSize ( ).y, fps, _hardwareAccelerated ? "GPU" : "CPU" );
            ui::SameLine ( );
            if ( _capture->IsStarted ( ) ) if ( ui::SmallButton ( "Stop" ) ) _capture->Stop ( );
            if ( _capture->IsStopped ( ) ) if ( ui::SmallButton ( "Start" ) ) _capture->Start ( );
            for ( auto& ctrl : _capture->GetControls ( ) )
            {
                ui::ScopedId id{ &ctrl };
                if ( ui::Button ( "Default" ) ) ctrl->Value ( ctrl->Default ( ) );
                ui::SameLine ( );
                if ( ImGui::SliderInt ( ctrl->Name ( ).c_str ( ), &ctrl->Value ( ), ctrl->Min ( ), ctrl->Max ( ) ) )
                {
                    ctrl->Value ( ctrl->Value ( ) );
                }
            }
        }
    }
#endif

    if ( _capture )
    {
        if ( _texture ) gl::draw ( _texture );
        if ( auto lease = _capture->GetTexture ( ) )
        {
            gl::draw ( lease->ToTexture ( ) );
        }
    }
}

void Init ( App::Settings * settings )
{
#ifdef CINDER_MSW
    settings->setConsoleWindowEnabled ( );
    settings->setWindowSize ( 1280, 720 );
#endif
}

CINDER_APP ( SimpleCaptureApp, RendererGl ( RendererGl::Options() ), Init );
