//
//  AX-VideoCapture.h
//  AX-VideoCapture
//
//  Created by Andrew Wright (@axjxwright) on 21/04/25.
//  (c) 2025 AX Interactive (axinteractive.com.au)
//  
//

#pragma once

#include "cinder/Cinder.h"
#include "cinder/Surface.h"
#include "cinder/Signals.h"
#include "cinder/gl/Texture.h"
#include "cinder/Filesystem.h"
#include "cinder/DataSource.h"
#include "cinder/Noncopyable.h"

namespace AX::Video
{
    using CaptureRef = std::shared_ptr<class Capture>;
    class Capture : public ci::Noncopyable
    {
    public:

        class Impl;
        
        class FrameLease
        {
        public:
            virtual ~FrameLease ( ) { };

            operator bool ( ) const { return IsValid ( ); }
            operator ci::gl::TextureRef ( ) const { return ToTexture ( ); }
            virtual ci::gl::TextureRef ToTexture ( ) const { return nullptr; }

        protected:
            virtual bool IsValid ( ) const { return false; };
        };

        struct DeviceDescriptor
        {
            std::string Name;
            std::string ID;

            bool operator == ( const DeviceDescriptor& other ) const
            {
                return Name == other.Name && ID == other.ID;
            }

            bool operator < ( const DeviceDescriptor& other ) const
            {
                return Name < other.Name && ID < other.ID;
            }
        };

        enum class Rotation
        {
            R0,
            R90,
            R180,
            R270
        };

        struct Control
        {
            int32_t             Min ( ) const { return _min; }
            int32_t             Max ( ) const { return _max; }
            int32_t             Step ( ) const { return _step; }
            int32_t             Value ( ) const { return _value; }
            int32_t             Default ( ) const { return _default; }
            const std::string&  Name ( ) const { return _name; }
            bool                IsSupported ( ) const { return _isSupported; }

            virtual int32_t     LoadValue ( ) { return _value; }
            int32_t&            Value ( ) { return _value; }
            void                Value ( int32_t value ) { StoreValue ( value ); }

            virtual ~Control ( ) {};

        protected:

            virtual void        LoadInitialState ( ) {};
            virtual void        StoreValue ( int32_t value ) {};
            
            int32_t             _min{ 0 };
            int32_t             _max{ 1 };
            int32_t             _step{ 1 };
            int32_t             _value{ 0 };
            int32_t             _default{ 0 };
            std::string         _name;
            bool                _isSupported{ false };
        };

        using ControlRef = std::unique_ptr<Control>;

        struct Format
        {
            Format ( ) { };
    
            Format& Size ( const ci::ivec2& size ) { _size = size; return *this; }
            Format& FPS ( int fps ) { _fps.x = fps; return *this; }
            Format& FPS ( int numerator, int denominator ) { _fps = { numerator, denominator }; return *this; }
            Format& Device ( const DeviceDescriptor& device ) { _device = device; return *this; }
            Format& HardwareAccelerated ( bool accelerated ) { _hardwareAccelerated = accelerated; return *this; }
            Format& RotationAngle ( Rotation rotation ) { _rotation = rotation; return *this; }
            Format& AutoStart ( bool autoStart ) { _autoStart = autoStart; return *this; }

            const ci::ivec2& Size ( ) const { return _size; }
            const ci::ivec2& FPS ( ) const { return _fps; }
            const DeviceDescriptor& Device ( ) const { return _device; }
            bool  IsHardwareAccelerated ( ) const { return _hardwareAccelerated; }
            Rotation RotationAngle ( ) const { return _rotation; }
            bool AutoStart ( ) const { return _autoStart; }

        protected:
            
            ci::ivec2               _size{ 640, 480 };
            ci::ivec2               _fps{ 30, 1 };
            DeviceDescriptor        _device;
            bool                    _hardwareAccelerated{ true };
            Rotation                _rotation{ Rotation::R0 };
            bool                    _autoStart{ true };
        };

        using  FrameLeaseRef        = std::unique_ptr<FrameLease>;
        using  EventSignal          = ci::signals::Signal<void ( )>;
        using  ErrorSignal          = ci::signals::Signal<void ( int )>;
        using  ControlChangedSignal = ci::signals::Signal<void ( Control& control )>;

        static std::vector<DeviceDescriptor> GetDevices ( bool refresh = false );
        static ci::signals::Signal<void ( DeviceDescriptor )>& OnDeviceAdded ( );
        static ci::signals::Signal<void ( DeviceDescriptor )>& OnDeviceRemoved ( );
        
        static  CaptureRef          Create ( const Format & fmt = Format ( ) );
        
        const Format &              GetFormat ( ) const { return _format; }

        const   ci::ivec2&          GetSize ( ) const;
        inline  ci::Area            GetBounds ( ) const { return ci::Area ( ci::ivec2(0), GetSize() ); }
        inline  bool                IsHardwareAccelerated ( ) const { return _format.IsHardwareAccelerated ( ); }
        bool                        IsValid ( ) const;

        bool                        CheckNewFrame ( ) const;
        const DeviceDescriptor&     GetDevice ( ) const { return _format.Device ( ); }

        const ci::Surface8uRef &    GetSurface ( ) const;
        FrameLeaseRef               GetTexture ( ) const;

        void                        Start ( );
        void                        Stop ( );
        bool                        IsStarted ( ) const;
        bool                        IsStopped ( ) const;

        const std::vector<ControlRef>& GetControls ( ) const { return _controls; }

        EventSignal                     OnInitialize;
        EventSignal                     OnStart;
        EventSignal                     OnStop;
        EventSignal                     OnDeviceLost;
        ErrorSignal                     OnError;
        ControlChangedSignal            OnControlChanged;
        
        ~Capture ( );

    protected:

        Capture ( const Format & format );
        
        Format                          _format;
        std::unique_ptr<Impl>           _impl;
        std::vector<ControlRef>         _controls;
        bool                            _isValid{ false };
    };
}

inline std::ostream& operator << ( std::ostream& stream, const AX::Video::Capture::DeviceDescriptor& descriptor )
{
    return stream << descriptor.Name << " (" << descriptor.ID << ")";
}