//
//  AX-VideoCapture.cxx
//  AX-VideoCapture
//
//  Created by Andrew Wright (@axjxwright) on 21/04/25.
//  (c) 2025 AX Interactive (axinteractive.com.au)
//  
//

#include "AX-VideoCapture.h"
#include "cinder/app/App.h"

#include <cstdint>
#include <iostream>
#include <unordered_map>

#ifdef WIN32
    #include "msw/AX-VideoCaptureMSWImpl.h"
#else
    #error "Unsupported platform"
#endif

using namespace ci;

namespace AX
{
    namespace Video
    {
        CaptureRef Capture::Create ( const Capture::Format& fmt )
        {
            auto capture = CaptureRef ( new Capture ( fmt ) );
            if ( capture && capture->IsValid ( ) )
            {
                return capture;
            }

            return nullptr;
        }

        std::vector<Capture::DeviceDescriptor> Capture::GetDevices ( bool refresh )
        {
            return Impl::GetDevices ( refresh );
        }

        std::vector<Capture::DeviceProfile> Capture::GetProfiles ( const DeviceDescriptor& descriptor )
        {
            return Impl::GetProfiles ( descriptor );
        }

        signals::Signal<void ( AX::Video::Capture::DeviceDescriptor )>& Capture::OnDeviceAdded ( )
        {
            static signals::Signal<void ( AX::Video::Capture::DeviceDescriptor )> kSignal;
            return kSignal;
        }

        signals::Signal<void ( AX::Video::Capture::DeviceDescriptor )>& Capture::OnDeviceRemoved ( )
        {
            static signals::Signal<void ( AX::Video::Capture::DeviceDescriptor )> kSignal;
            return kSignal;
        }

        Capture::Capture ( const Format& fmt )
            : _format ( fmt )
        {
            GetDevices ( );

            if ( _format.Device ( ).ID.empty ( ) )
            {
                auto devices = GetDevices ( );
                if ( !devices.empty ( ) )
                {
                    _format.Device ( devices[0] );
                }
            }
            
            _impl = std::make_unique<Impl> ( *this, _format );
            _isValid = _impl->IsValid ( );
        }

        void Capture::Start ( )
        {
            _impl->Start ( );
        }

        void Capture::Stop ( )
        {
            _impl->Stop ( );
        }

        bool Capture::IsStarted ( ) const
        {
            return _impl->IsStarted ( );
        }

        bool Capture::IsStopped ( ) const
        {
            return _impl->IsStopped ( );
        }

        bool Capture::IsValid ( ) const
        {
            return _isValid;
        }

        bool Capture::CheckNewFrame ( ) const
        {
            return _impl->CheckNewFrame ( );
        }

        const ivec2& Capture::GetSize ( ) const
        {
            return _impl->GetSize ( );
        }

        const Surface8uRef & Capture::GetSurface ( ) const
        {
            return _impl->GetSurface ( );
        }

        Capture::FrameLeaseRef Capture::GetTexture ( ) const
        {
            return _impl->GetTexture ( );
        }

        Capture::~Capture ( )
        {
            _impl = nullptr;
        }
    }
}
