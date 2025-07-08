#ifndef _INPUT_H_
#define _INPUT_H_

#include "types.h"
#include "tcp.h"
#include "guard.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>

namespace input {

    /**
     * @brief Enumeration of input device types supported by the system
     */
    enum class DeviceType {
        UNKNOWN,
        KEYBOARD,
        MOUSE,
        TOUCHPAD,
        TOUCHSCREEN,
        GAMEPAD,
        JOYSTICK,
        STYLUS,
        TABLET
    };

    /**
     * @brief Enumeration of input event types
     */
    enum class EventType {
        UNKNOWN,
        KEY_PRESS,
        KEY_RELEASE,
        MOUSE_MOVE,
        MOUSE_PRESS,
        MOUSE_RELEASE,
        MOUSE_SCROLL,
        TOUCH_START,
        TOUCH_MOVE,
        TOUCH_END,
        GAMEPAD_BUTTON,
        GAMEPAD_AXIS
    };

    /**
     * @brief Raw input event data structure
     */
    struct RawEvent {
        EventType type;
        DeviceType deviceType;
        std::string devicePath;
        uint64_t timestamp;
        int code;       // Linux input event code
        int value;      // Event value
        types::iVector2 position; // For mouse/touch events
        
        RawEvent() : type(EventType::UNKNOWN), deviceType(DeviceType::UNKNOWN), 
                    timestamp(0), code(0), value(0), position(0, 0) {}
    };

    /**
     * @brief Input device capabilities and information
     */
    struct DeviceInfo {
        std::string name;
        std::string path;
        DeviceType type;
        int fd;
        bool isActive;
        std::vector<int> supportedKeys;
        std::vector<int> supportedAxes;
        types::iVector2 resolution;  // For absolute devices
        types::iVector2 minValues;   // Minimum axis values
        types::iVector2 maxValues;   // Maximum axis values
        
        DeviceInfo() : type(DeviceType::UNKNOWN), fd(-1), isActive(false),
                      resolution(0, 0), minValues(0, 0), maxValues(0, 0) {}
    };

    /**
     * @brief Abstract base class for input device handlers
     */
    class IDeviceHandler {
    public:
        virtual ~IDeviceHandler() = default;
        virtual bool initialize(const DeviceInfo& deviceInfo) = 0;
        virtual void cleanup() = 0;
        virtual bool processEvent(const RawEvent& rawEvent, packet::input::base& processedEvent) = 0;
        virtual DeviceType getDeviceType() const = 0;
        virtual bool isDeviceSupported(const DeviceInfo& deviceInfo) const = 0;
    };

    /**
     * @brief Keyboard device handler
     */
    class KeyboardHandler : public IDeviceHandler {
    private:
        packet::input::controlKey currentModifiers;
        
    public:
        bool initialize(const DeviceInfo& deviceInfo) override;
        void cleanup() override;
        bool processEvent(const RawEvent& rawEvent, packet::input::base& processedEvent) override;
        DeviceType getDeviceType() const override { return DeviceType::KEYBOARD; }
        bool isDeviceSupported(const DeviceInfo& deviceInfo) const override;
    };

    /**
     * @brief Mouse device handler
     */
    class MouseHandler : public IDeviceHandler {
    private:
        types::iVector2 currentPosition;
        bool buttonStates[8]; // Support for 8 mouse buttons
        
    public:
        MouseHandler();
        bool initialize(const DeviceInfo& deviceInfo) override;
        void cleanup() override;
        bool processEvent(const RawEvent& rawEvent, packet::input::base& processedEvent) override;
        DeviceType getDeviceType() const override { return DeviceType::MOUSE; }
        bool isDeviceSupported(const DeviceInfo& deviceInfo) const override;
    };

    /**
     * @brief Touchpad device handler
     */
    class TouchpadHandler : public IDeviceHandler {
    private:
        types::iVector2 currentPosition;
        bool isTouching;
        
    public:
        TouchpadHandler();
        bool initialize(const DeviceInfo& deviceInfo) override;
        void cleanup() override;
        bool processEvent(const RawEvent& rawEvent, packet::input::base& processedEvent) override;
        DeviceType getDeviceType() const override { return DeviceType::TOUCHPAD; }
        bool isDeviceSupported(const DeviceInfo& deviceInfo) const override;
    };

    /**
     * @brief Input device registry and manager
     */
    class DeviceManager {
    private:
        std::vector<std::unique_ptr<DeviceInfo>> devices;
        std::vector<std::unique_ptr<IDeviceHandler>> handlers;
        std::atomic<bool> isRunning;
        
    public:
        DeviceManager();
        ~DeviceManager();
        
        bool scanDevices();
        bool addDevice(const std::string& devicePath);
        bool removeDevice(const std::string& devicePath);
        std::vector<DeviceInfo> getActiveDevices() const;
        bool isDeviceActive(const std::string& devicePath) const;
        
        // Device capability queries
        DeviceType identifyDeviceType(const std::string& devicePath) const;
        std::unique_ptr<DeviceInfo> queryDeviceInfo(const std::string& devicePath) const;
        
        // Handler management
        void registerHandler(std::unique_ptr<IDeviceHandler> handler);
        IDeviceHandler* getHandler(DeviceType deviceType) const;
        
        void start();
        void stop();
        bool isActive() const { return isRunning.load(); }
    };

    /**
     * @brief Input event processor and dispatcher
     */
    class EventProcessor {
    private:
        DeviceManager* deviceManager;
        std::function<void(const packet::input::base&)> eventCallback;
        std::thread processingThread;
        std::atomic<bool> isRunning;
        
        void processRawEvent(const RawEvent& rawEvent);
        void pollDevices();
        
    public:
        EventProcessor();
        ~EventProcessor();
        
        void setDeviceManager(DeviceManager* manager);
        void setEventCallback(std::function<void(const packet::input::base&)> callback);
        
        void start();
        void stop();
        bool isActive() const { return isRunning.load(); }
    };

    /**
     * @brief Main input system manager
     */
    namespace manager {
        extern atomic::guard<DeviceManager> deviceManager;
        extern atomic::guard<EventProcessor> eventProcessor;
        
        // Forward declaration for window handle
        extern void* focusedHandle;  // Using void* to avoid circular dependency
        
        void init();
        void exit();
        void poll();
        
        // Focus management
        void setFocusedHandle(void* handle);
        void* getFocusedHandle();
        
        // Device management
        bool addInputDevice(const std::string& devicePath);
        bool removeInputDevice(const std::string& devicePath);
        std::vector<DeviceInfo> getInputDevices();
        bool refreshDevices();
        
        // Event handling
        void processInputEvent(const packet::input::base& inputEvent);
        void sendInputToFocusedHandle(const packet::input::base& inputEvent);
    }

    /**
     * @brief Utility functions for input device discovery
     */
    namespace utils {
        std::vector<std::string> scanInputDevices();
        bool isInputDevice(const std::string& devicePath);
        std::string getDeviceName(const std::string& devicePath);
        DeviceType classifyDevice(const std::string& devicePath);
        bool hasCapability(const std::string& devicePath, int capability);
        std::vector<int> getSupportedKeys(const std::string& devicePath);
        std::vector<int> getSupportedAxes(const std::string& devicePath);
        types::iVector2 getDeviceResolution(const std::string& devicePath);
        types::iVector2 getAxisRange(const std::string& devicePath, int axis);
    }

    // Legacy compatibility functions
    void init();  // Initializes input thread and sets up necessary resources
    void exit();  // Cleans up input resources and exits the input thread
    void poll();  // Called from input thread to poll for input events and to then send them to the focused handle

}

#endif