#include "input.h"
#include "window.h"
#include "system.h"
#include "logger.h"
#include "config.h"

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <future>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <sys/ioctl.h>
#include <sys/select.h>

namespace {
    struct ScopedFd {
        int fd{-1};
        explicit ScopedFd(const std::string& path, int flags = O_RDONLY) {
            fd = open(path.c_str(), flags);
        }
        ~ScopedFd() {
            if (fd >= 0) close(fd);
        }
        ScopedFd(const ScopedFd&) = delete;
        ScopedFd& operator=(const ScopedFd&) = delete;
        ScopedFd(ScopedFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
        ScopedFd& operator=(ScopedFd&& other) noexcept {
            if (this != &other) {
                if (fd >= 0) close(fd);
                fd = other.fd;
                other.fd = -1;
            }
            return *this;
        }
        bool valid() const { return fd >= 0; }
        int get() const { return fd; }
    };
}

// Utility macros for bit manipulation
#define BITS_PER_LONG (sizeof(long) * 8)
#define BITS_TO_LONGS(nr) DIV_ROUND_UP(nr, BITS_PER_LONG)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static inline int test_bit(int nr, const unsigned long *addr) {
    return 1UL & (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG));
}

namespace input {

    // ============================================================================
    // KeyboardHandler Implementation
    // ============================================================================

    bool KeyboardHandler::initialize(const DeviceInfo& deviceInfo) {
        LOG_INFO() << "Initialized keyboard handler for: " << deviceInfo.name << std::endl;
        return true;
    }

    void KeyboardHandler::cleanup() {
        LOG_INFO() << "Cleaned up keyboard handler." << std::endl;
    }

    bool KeyboardHandler::processEvent(const RawEvent& rawEvent, packet::input::base& processedEvent) {
        if (rawEvent.type != EventType::KEY_PRESS && rawEvent.type != EventType::KEY_RELEASE) {
            return false;
        }

        const bool isPressed = (rawEvent.type == EventType::KEY_PRESS) || (rawEvent.value == 2);

        // Always update the held-key state.
        keyStates[rawEvent.code] = isPressed;

        // Released keys do not contribute to keybinds or processed events.
        if (!isPressed) {
            processedEvent = packet::input::base();
            return false;
        }

        auto isDown = [this](int keyCode) -> bool {
            auto it = keyStates.find(keyCode);
            return it != keyStates.end() && it->second;
        };

        const bool ctrlDown = isDown(KEY_LEFTCTRL) || isDown(KEY_RIGHTCTRL);
        const bool altDown = isDown(KEY_LEFTALT) || isDown(KEY_RIGHTALT);
        const bool shiftDown = isDown(KEY_LEFTSHIFT) || isDown(KEY_RIGHTSHIFT);
        const bool superDown = isDown(KEY_LEFTMETA) || isDown(KEY_RIGHTMETA);

        config::KeyCombination combo(rawEvent.code, ctrlDown, altDown, shiftDown, superDown);

        // Check for global keybinds first; if consumed, suppress forwarding.
        if (config::manager::processKeyInput(combo)) {
            // Clear the keys responsible for the combo so they don't keep contributing.
            keyStates[combo.keyCode] = false;
            if (combo.ctrl) {
                keyStates[KEY_LEFTCTRL] = false;
                keyStates[KEY_RIGHTCTRL] = false;
            }
            if (combo.alt) {
                keyStates[KEY_LEFTALT] = false;
                keyStates[KEY_RIGHTALT] = false;
            }
            if (combo.shift) {
                keyStates[KEY_LEFTSHIFT] = false;
                keyStates[KEY_RIGHTSHIFT] = false;
            }
            if (combo.super) {
                keyStates[KEY_LEFTMETA] = false;
                keyStates[KEY_RIGHTMETA] = false;
            }

            processedEvent = packet::input::base();
            return false;
        }

        // Not a keybind: translate to a packet::input::base key event.
        processedEvent = packet::input::base();

        // Modifiers bitmask
        int modifierBits = 0;
        if (shiftDown) modifierBits |= static_cast<int>(packet::input::controlKey::SHIFT);
        if (ctrlDown) modifierBits |= static_cast<int>(packet::input::controlKey::CTRL);
        if (superDown) modifierBits |= static_cast<int>(packet::input::controlKey::SUPER);
        if (altDown) modifierBits |= static_cast<int>(packet::input::controlKey::ALT);
        modifierBits |= static_cast<int>(packet::input::controlKey::PRESSED_DOWN);
        processedEvent.modifiers = static_cast<packet::input::controlKey>(modifierBits);

        // Special keys that are not ASCII
        switch (rawEvent.code) {
            case KEY_F1: processedEvent.additional = packet::input::additionalKey::F1; break;
            case KEY_F2: processedEvent.additional = packet::input::additionalKey::F2; break;
            case KEY_F3: processedEvent.additional = packet::input::additionalKey::F3; break;
            case KEY_F4: processedEvent.additional = packet::input::additionalKey::F4; break;
            case KEY_F5: processedEvent.additional = packet::input::additionalKey::F5; break;
            case KEY_F6: processedEvent.additional = packet::input::additionalKey::F6; break;
            case KEY_F7: processedEvent.additional = packet::input::additionalKey::F7; break;
            case KEY_F8: processedEvent.additional = packet::input::additionalKey::F8; break;
            case KEY_F9: processedEvent.additional = packet::input::additionalKey::F9; break;
            case KEY_F10: processedEvent.additional = packet::input::additionalKey::F10; break;
            case KEY_F11: processedEvent.additional = packet::input::additionalKey::F11; break;
            case KEY_F12: processedEvent.additional = packet::input::additionalKey::F12; break;
            case KEY_UP: processedEvent.additional = packet::input::additionalKey::ARROW_UP; break;
            case KEY_DOWN: processedEvent.additional = packet::input::additionalKey::ARROW_DOWN; break;
            case KEY_LEFT: processedEvent.additional = packet::input::additionalKey::ARROW_LEFT; break;
            case KEY_RIGHT: processedEvent.additional = packet::input::additionalKey::ARROW_RIGHT; break;
            case KEY_HOME: processedEvent.additional = packet::input::additionalKey::HOME; break;
            case KEY_END: processedEvent.additional = packet::input::additionalKey::END; break;
            case KEY_PAGEUP: processedEvent.additional = packet::input::additionalKey::PAGE_UP; break;
            case KEY_PAGEDOWN: processedEvent.additional = packet::input::additionalKey::PAGE_DOWN; break;
            case KEY_INSERT: processedEvent.additional = packet::input::additionalKey::INSERT; break;
            case KEY_DELETE: processedEvent.additional = packet::input::additionalKey::DELETE; break;
            default:
                break;
        }

        // ASCII mapping for common keys; shift is applied for letters and digits where sensible.
        if (processedEvent.additional == packet::input::additionalKey::UNKNOWN) {
            unsigned char ascii = 0;

            // Letters
            if (rawEvent.code >= KEY_A && rawEvent.code <= KEY_Z) {
                const char base = shiftDown ? 'A' : 'a';
                ascii = static_cast<unsigned char>(base + (rawEvent.code - KEY_A));
            }
            // Number row
            else if (rawEvent.code >= KEY_1 && rawEvent.code <= KEY_9) {
                static const char normal[] = {'1','2','3','4','5','6','7','8','9'};
                static const char shifted[] = {'!','@','#','$','%','^','&','*','('};
                const int idx = rawEvent.code - KEY_1;
                ascii = static_cast<unsigned char>(shiftDown ? shifted[idx] : normal[idx]);
            } else if (rawEvent.code == KEY_0) {
                ascii = static_cast<unsigned char>(shiftDown ? ')' : '0');
            }
            // Whitespace / control
            else if (rawEvent.code == KEY_SPACE) ascii = ' ';
            else if (rawEvent.code == KEY_TAB) ascii = '\t';
            else if (rawEvent.code == KEY_ENTER) ascii = '\n';
            else if (rawEvent.code == KEY_BACKSPACE) ascii = '\b';
            else if (rawEvent.code == KEY_ESC) ascii = 27;
            // Punctuation (minimal set)
            else if (rawEvent.code == KEY_MINUS) ascii = static_cast<unsigned char>(shiftDown ? '_' : '-');
            else if (rawEvent.code == KEY_EQUAL) ascii = static_cast<unsigned char>(shiftDown ? '+' : '=');
            else if (rawEvent.code == KEY_LEFTBRACE) ascii = static_cast<unsigned char>(shiftDown ? '{' : '[');
            else if (rawEvent.code == KEY_RIGHTBRACE) ascii = static_cast<unsigned char>(shiftDown ? '}' : ']');
            else if (rawEvent.code == KEY_SEMICOLON) ascii = static_cast<unsigned char>(shiftDown ? ':' : ';');
            else if (rawEvent.code == KEY_APOSTROPHE) ascii = static_cast<unsigned char>(shiftDown ? '"' : '\'');
            else if (rawEvent.code == KEY_GRAVE) ascii = static_cast<unsigned char>(shiftDown ? '~' : '`');
            else if (rawEvent.code == KEY_BACKSLASH) ascii = static_cast<unsigned char>(shiftDown ? '|' : '\\');
            else if (rawEvent.code == KEY_COMMA) ascii = static_cast<unsigned char>(shiftDown ? '<' : ',');
            else if (rawEvent.code == KEY_DOT) ascii = static_cast<unsigned char>(shiftDown ? '>' : '.');
            else if (rawEvent.code == KEY_SLASH) ascii = static_cast<unsigned char>(shiftDown ? '?' : '/');

            processedEvent.key = ascii;
        }

        return true;
    }

    bool KeyboardHandler::isDeviceSupported(const DeviceInfo& deviceInfo) const {
        return deviceInfo.type == DeviceType::KEYBOARD;
    }

    // ============================================================================
    // MouseHandler Implementation
    // ============================================================================

    MouseHandler::MouseHandler() : currentPosition(0, 0) {
        std::memset(buttonStates, 0, sizeof(buttonStates));
    }

    bool MouseHandler::initialize(const DeviceInfo& deviceInfo) {
        currentPosition = types::iVector2(0, 0);
        std::memset(buttonStates, 0, sizeof(buttonStates));
        LOG_INFO() << "Initialized mouse handler for: " << deviceInfo.name << std::endl;
        return true;
    }

    void MouseHandler::cleanup() {
        currentPosition = types::iVector2(0, 0);
        std::memset(buttonStates, 0, sizeof(buttonStates));
        LOG_INFO() << "Cleaned up mouse handler." << std::endl;
    }

    bool MouseHandler::processEvent(const RawEvent& rawEvent, packet::input::base& processedEvent) {
        processedEvent.mouse = currentPosition;
        
        switch (rawEvent.type) {
            case EventType::MOUSE_MOVE:
                if (rawEvent.code == REL_X) {
                    currentPosition.x += rawEvent.value;
                } else if (rawEvent.code == REL_Y) {
                    currentPosition.y += rawEvent.value;
                }
                processedEvent.mouse = currentPosition;
                break;
                
            case EventType::MOUSE_PRESS:
            case EventType::MOUSE_RELEASE:
                {
                    bool isPressed = (rawEvent.type == EventType::MOUSE_PRESS);
                    packet::input::additionalKey mouseKey = packet::input::additionalKey::UNKNOWN;
                    
                    switch (rawEvent.code) {
                        case BTN_LEFT:
                            mouseKey = packet::input::additionalKey::LEFT_CLICK;
                            buttonStates[0] = isPressed;
                            break;
                        case BTN_RIGHT:
                            mouseKey = packet::input::additionalKey::RIGHT_CLICK;
                            buttonStates[1] = isPressed;
                            break;
                        case BTN_MIDDLE:
                            mouseKey = packet::input::additionalKey::MIDDLE_CLICK;
                            buttonStates[2] = isPressed;
                            break;
                    }
                    
                    processedEvent.additional = mouseKey;
                    if (isPressed) {
                        processedEvent.modifiers = static_cast<packet::input::controlKey>(
                            static_cast<int>(processedEvent.modifiers) | static_cast<int>(packet::input::controlKey::PRESSED_DOWN)
                        );
                    }
                }
                break;
                
            case EventType::MOUSE_SCROLL:
                if (rawEvent.code == REL_WHEEL) {
                    processedEvent.additional = (rawEvent.value > 0) ? 
                        packet::input::additionalKey::SCROLL_UP : packet::input::additionalKey::SCROLL_DOWN;
                }
                break;
                
            default:
                return false;
        }
        
        return true;
    }

    bool MouseHandler::isDeviceSupported(const DeviceInfo& deviceInfo) const {
        return deviceInfo.type == DeviceType::MOUSE;
    }

    // ============================================================================
    // TouchpadHandler Implementation
    // ============================================================================

    TouchpadHandler::TouchpadHandler() : currentPosition(0, 0), isTouching(false) {}

    bool TouchpadHandler::initialize(const DeviceInfo& deviceInfo) {
        currentPosition = types::iVector2(0, 0);
        isTouching = false;
        LOG_INFO() << "Initialized touchpad handler for: " << deviceInfo.name << std::endl;
        return true;
    }

    void TouchpadHandler::cleanup() {
        currentPosition = types::iVector2(0, 0);
        isTouching = false;
        LOG_INFO() << "Cleaned up touchpad handler." << std::endl;
    }

    bool TouchpadHandler::processEvent(const RawEvent& rawEvent, packet::input::base& processedEvent) {
        processedEvent.mouse = currentPosition;
        
        switch (rawEvent.type) {
            case EventType::MOUSE_MOVE:
                if (rawEvent.code == ABS_X) {
                    currentPosition.x = rawEvent.value;
                } else if (rawEvent.code == ABS_Y) {
                    currentPosition.y = rawEvent.value;
                }
                processedEvent.mouse = currentPosition;
                break;
                
            case EventType::TOUCH_START:
                isTouching = true;
                processedEvent.additional = packet::input::additionalKey::LEFT_CLICK;
                processedEvent.modifiers = static_cast<packet::input::controlKey>(
                    static_cast<int>(processedEvent.modifiers) | static_cast<int>(packet::input::controlKey::PRESSED_DOWN)
                );
                break;
                
            case EventType::TOUCH_END:
                isTouching = false;
                processedEvent.additional = packet::input::additionalKey::LEFT_CLICK;
                break;
                
            default:
                return false;
        }
        
        return true;
    }

    bool TouchpadHandler::isDeviceSupported(const DeviceInfo& deviceInfo) const {
        return deviceInfo.type == DeviceType::TOUCHPAD;
    }

    // ============================================================================
    // DeviceManager Implementation
    // ============================================================================

    DeviceManager::DeviceManager() : isRunning(false) {}

    DeviceManager::~DeviceManager() {
        stop();
    }

    bool DeviceManager::scanDevices() {
        std::vector<std::string> devicePaths = utils::scanInputDevices();
        
        for (const auto& devicePath : devicePaths) {
            if (utils::isInputDevice(devicePath)) {
                addDevice(devicePath);
            }
        }
        
        logger::info("Scanned " + std::to_string(devices.size()) + " input devices.");
        return !devices.empty();
    }

    bool DeviceManager::addDevice(const std::string& devicePath) {
        auto deviceInfo = queryDeviceInfo(devicePath);
        if (!deviceInfo) {
            return false;
        }
        
        // Check if device is already registered
        auto it = std::find_if(devices.begin(), devices.end(),
            [&devicePath](const std::unique_ptr<DeviceInfo>& device) {
                return device->path == devicePath;
            });
        
        if (it != devices.end()) {
            return false; // Device already exists
        }
        
        // Open the device
        deviceInfo->fd = open(devicePath.c_str(), O_RDONLY | O_NONBLOCK);
        if (deviceInfo->fd < 0) {
            LOG_ERROR() << "Failed to open device: " << devicePath << std::endl;
            return false;
        }
        
        deviceInfo->isActive = true;
        devices.push_back(std::move(deviceInfo));
        
        LOG_VERBOSE() << "Added input device: " << devicePath << std::endl;
        return true;
    }

    bool DeviceManager::removeDevice(const std::string& devicePath) {
        auto it = std::find_if(devices.begin(), devices.end(),
            [&devicePath](const std::unique_ptr<DeviceInfo>& device) {
                return device->path == devicePath;
            });
        
        if (it != devices.end()) {
            if ((*it)->fd >= 0) {
                close((*it)->fd);
            }
            devices.erase(it);
            LOG_INFO() << "Removed input device: " << devicePath << std::endl;
            return true;
        }
        
        return false;
    }

    std::vector<DeviceInfo> DeviceManager::getActiveDevices() const {
        std::vector<DeviceInfo> activeDevices;
        for (const auto& device : devices) {
            if (device->isActive) {
                activeDevices.push_back(*device);
            }
        }
        return activeDevices;
    }

    bool DeviceManager::isDeviceActive(const std::string& devicePath) const {
        auto it = std::find_if(devices.begin(), devices.end(),
            [&devicePath](const std::unique_ptr<DeviceInfo>& device) {
                return device->path == devicePath;
            });
        
        return it != devices.end() && (*it)->isActive;
    }

    DeviceType DeviceManager::identifyDeviceType(const std::string& devicePath) const {
        return utils::classifyDevice(devicePath);
    }

    std::unique_ptr<DeviceInfo> DeviceManager::queryDeviceInfo(const std::string& devicePath) const {
        auto deviceInfo = std::make_unique<DeviceInfo>();
        deviceInfo->path = devicePath;

        ScopedFd fd(devicePath);
        if (fd.valid()) {
            deviceInfo->name = utils::getDeviceName(fd.get());
            deviceInfo->type = utils::classifyDevice(fd.get());
            deviceInfo->supportedKeys = utils::getSupportedKeys(fd.get());
            deviceInfo->supportedAxes = utils::getSupportedAxes(fd.get());
            deviceInfo->resolution = utils::getDeviceResolution(fd.get());
            deviceInfo->minValues = utils::getAxisRange(fd.get(), ABS_X);
            deviceInfo->maxValues = utils::getAxisRange(fd.get(), ABS_Y);
        } else {
            deviceInfo->name = "Unknown Device";
            deviceInfo->type = DeviceType::UNKNOWN;
        }
        
        return deviceInfo;
    }

    void DeviceManager::registerHandler(std::unique_ptr<IDeviceHandler> handler) {
        handlers.push_back(std::move(handler));
    }

    IDeviceHandler* DeviceManager::getHandler(DeviceType deviceType) const {
        for (const auto& handler : handlers) {
            if (handler->getDeviceType() == deviceType) {
                return handler.get();
            }
        }
        return nullptr;
    }

    void DeviceManager::start() {
        isRunning = true;
        LOG_INFO() << "Input device manager started." << std::endl;
    }

    void DeviceManager::stop() {
        isRunning = false;
        
        // Close all device file descriptors
        for (auto& device : devices) {
            if (device->fd >= 0) {
                close(device->fd);
                device->fd = -1;
                device->isActive = false;
            }
        }
        
        LOG_INFO() << "Input device manager stopped." << std::endl;
    }

    // ============================================================================
    // EventProcessor Implementation
    // ============================================================================

    EventProcessor::EventProcessor() : deviceManager(nullptr), isRunning(false) {}

    EventProcessor::~EventProcessor() {
        stop();
    }

    void EventProcessor::setDeviceManager(DeviceManager* manager) {
        deviceManager = manager;
    }

    void EventProcessor::setEventCallback(std::function<void(const packet::input::base&)> callback) {
        eventCallback = std::move(callback);
    }

    void EventProcessor::start() {
        if (isRunning || !deviceManager) {
            return;
        }
        
        isRunning = true;
        processingThread = std::thread(&EventProcessor::pollDevices, this);
        LOG_INFO() << "Input event processor started." << std::endl;
    }

    void EventProcessor::stop() {
        if (!isRunning) {
            return;
        }
        
        LOG_VERBOSE() << "Stopping input event processor..." << std::endl;
        isRunning = false;
        
        if (processingThread.joinable()) {
            // Use a future to implement a timeout for the join operation
            auto future = std::async(std::launch::async, [this]() {
                processingThread.join();
            });
            
            // Wait for the thread to finish with a timeout
            auto status = future.wait_for(std::chrono::milliseconds(200));
            
            if (status == std::future_status::timeout) {
                // Thread didn't finish in time, we need to detach it
                LOG_VERBOSE() << "Input event processor thread didn't respond to stop signal in time, detaching..." << std::endl;
                processingThread.detach();
            } else {
                LOG_VERBOSE() << "Input event processor thread finished cleanly." << std::endl;
            }
        }
        
        LOG_INFO() << "Input event processor stopped." << std::endl;
    }

    void EventProcessor::pollDevices() {
        const auto pollInterval = std::chrono::milliseconds(10);
        
        while (isRunning) {
            auto devices = deviceManager->getActiveDevices();
            
            for (const auto& device : devices) {
                // Check if we should exit early
                if (!isRunning) {
                    break;
                }
                
                if (device.fd < 0) continue;
                
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(device.fd, &readfds);
                
                timeval timeout = {0, 0}; // Non-blocking
                int result = select(device.fd + 1, &readfds, nullptr, nullptr, &timeout);
                
                if (result > 0 && FD_ISSET(device.fd, &readfds)) {
                    struct input_event ev;
                    ssize_t bytesRead = read(device.fd, &ev, sizeof(ev));
                    
                    if (bytesRead == sizeof(ev)) {
                        RawEvent rawEvent;
                        rawEvent.devicePath = device.path;
                        rawEvent.deviceType = device.type;
                        rawEvent.timestamp = ev.time.tv_sec * 1000 + ev.time.tv_usec / 1000;
                        rawEvent.code = ev.code;
                        rawEvent.value = ev.value;
                        
                        // Convert Linux input event type to our event type
                        switch (ev.type) {
                            case EV_KEY:
                                // ev.value: 0=release, 1=press, 2=repeat
                                if (ev.value == 0) {
                                    rawEvent.type = EventType::KEY_RELEASE;
                                } else {
                                    rawEvent.type = EventType::KEY_PRESS;
                                }
                                break;
                            case EV_REL:
                                rawEvent.type = EventType::MOUSE_MOVE;
                                break;
                            case EV_ABS:
                                rawEvent.type = EventType::MOUSE_MOVE;
                                break;
                            default:
                                rawEvent.type = EventType::UNKNOWN;
                                break;
                        }
                        
                        if (rawEvent.type != EventType::UNKNOWN) {
                            processRawEvent(rawEvent);
                        }
                    }
                }
            }
            
            // Check again before sleeping
            if (!isRunning) {
                break;
            }
            
            std::this_thread::sleep_for(pollInterval);
        }
        
        LOG_VERBOSE() << "Input event processor polling thread exiting..." << std::endl;
    }

    void EventProcessor::processRawEvent(const RawEvent& rawEvent) {
        if (!deviceManager || !eventCallback) {
            return;
        }
        
        auto handler = deviceManager->getHandler(rawEvent.deviceType);
        if (!handler) {
            return;
        }
        
        packet::input::base processedEvent;
        if (handler->processEvent(rawEvent, processedEvent)) {
            eventCallback(processedEvent);
        }
    }

    // ============================================================================
    // Manager Implementation
    // ============================================================================

    namespace manager {
        atomic::guard<DeviceManager> deviceManager;
        atomic::guard<EventProcessor> eventProcessor;
        void* focusedHandle = nullptr;

        void init() {
            logger::info("Initializing input system...");
            
            // Initialize device manager
            deviceManager([](DeviceManager& dm) {
                // Register device handlers
                dm.registerHandler(std::make_unique<KeyboardHandler>());
                dm.registerHandler(std::make_unique<MouseHandler>());
                dm.registerHandler(std::make_unique<TouchpadHandler>());
                
                // Scan for devices
                dm.scanDevices();
                dm.start();
            });
            
            // Initialize event processor
            eventProcessor([](EventProcessor& ep) {
                deviceManager([&ep](DeviceManager& dm) {
                    ep.setDeviceManager(&dm);
                });
                
                ep.setEventCallback([](const packet::input::base& inputEvent) {
                    processInputEvent(inputEvent);
                });
                
                ep.start();
            });
            
            LOG_INFO() << "Input system initialized successfully." << std::endl;
        }

        void exit() {
            LOG_INFO() << "Shutting down input system..." << std::endl;
            
            eventProcessor([](EventProcessor& ep) {
                ep.stop();
            });
            
            deviceManager([](DeviceManager& dm) {
                dm.stop();
            });
            
            LOG_INFO() << "Input system shutdown complete." << std::endl;
        }

        void setFocusedHandle(void* handle) {
            focusedHandle = handle;
        }

        void* getFocusedHandle() {
            return focusedHandle;
        }

        bool addInputDevice(const std::string& devicePath) {
            bool result = false;
            deviceManager([&result, &devicePath](DeviceManager& dm) {
                result = dm.addDevice(devicePath);
            });
            return result;
        }

        bool removeInputDevice(const std::string& devicePath) {
            bool result = false;
            deviceManager([&result, &devicePath](DeviceManager& dm) {
                result = dm.removeDevice(devicePath);
            });
            return result;
        }

        std::vector<DeviceInfo> getInputDevices() {
            std::vector<DeviceInfo> devices;
            deviceManager([&devices](DeviceManager& dm) {
                devices = dm.getActiveDevices();
            });
            return devices;
        }

        bool refreshDevices() {
            bool result = false;
            deviceManager([&result](DeviceManager& dm) {
                result = dm.scanDevices();
            });
            return result;
        }

        void processInputEvent(const packet::input::base& inputEvent) {
            // Since keybinds are now handled at the keyboard handler level,
            // we just send all input events to the focused handle
            sendInputToFocusedHandle(inputEvent);
        }

        /**
         * @brief Sends an input event to the currently focused window handle.
         *
         * This function checks if there is a focused handle and, if so, casts it to a window handle.
         * It then prepares a packet buffer, copies the input event data into the buffer, and sends it
         * through the handle's connection. If sending fails, an error is logged.
         *
         * @param inputEvent The input event to be sent to the focused handle.
         */
        void sendInputToFocusedHandle(const packet::input::base& inputEvent) {
            if (focusedHandle) {
                // Cast to window::handle* and send the input event
                window::handle* handle = static_cast<window::handle*>(focusedHandle);
                if (handle->connection.getHandle() >= 0) {
                    // Create packet buffer and copy input event into it
                    char packetBuffer[packet::size];
                    packet::input::base* inputPacket = new(packetBuffer) packet::input::base();
                    
                    // Copy the input event data
                    inputPacket->mouse = inputEvent.mouse;
                    inputPacket->modifiers = inputEvent.modifiers;
                    inputPacket->additional = inputEvent.additional;
                    inputPacket->key = inputEvent.key;
                    
                    if (!handle->connection.Send<char>(packetBuffer, packet::size)) {
                        LOG_ERROR() << "Failed to send input event to focused handle" << std::endl;
                        return;
                    }
                }
            }
        }
    }

    // ============================================================================
    // Utils Implementation
    // ============================================================================

    namespace utils {
        std::vector<std::string> scanInputDevices() {
            std::vector<std::string> devices;
            const char* inputDir = "/dev/input";
            
            DIR* dir = opendir(inputDir);
            if (!dir) {
                LOG_ERROR() << "Failed to open input directory: " << inputDir << std::endl;
                return devices;
            }
            
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strncmp(entry->d_name, "event", 5) == 0) {
                    std::string devicePath = std::string(inputDir) + "/" + entry->d_name;
                    devices.push_back(devicePath);
                }
            }
            
            closedir(dir);
            return devices;
        }

        bool isInputDevice(const std::string& devicePath) {
            ScopedFd fd(devicePath);
            return fd.valid() && isInputDevice(fd.get());
        }

        bool isInputDevice(int fd) {
            if (fd < 0) return false;

            unsigned long evbits[BITS_TO_LONGS(EV_CNT)]{};
            return ioctl(fd, EVIOCGBIT(0, EV_CNT), evbits) >= 0;
        }

        std::string getDeviceName(const std::string& devicePath) {
            ScopedFd fd(devicePath);
            return fd.valid() ? getDeviceName(fd.get()) : "Unknown Device";
        }

        std::string getDeviceName(int fd) {
            if (fd < 0) return "Unknown Device";

            char name[256] = {0};
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
                return "Unknown Device";
            }

            return std::string(name);
        }

        DeviceType classifyDevice(const std::string& devicePath) {
            ScopedFd fd(devicePath);
            return fd.valid() ? classifyDevice(fd.get()) : DeviceType::UNKNOWN;
        }

        DeviceType classifyDevice(int fd) {
            if (fd < 0) return DeviceType::UNKNOWN;

            unsigned long evbits[BITS_TO_LONGS(EV_CNT)]{};
            unsigned long keybits[BITS_TO_LONGS(KEY_CNT)]{};
            unsigned long relbits[BITS_TO_LONGS(REL_CNT)]{};

            if (ioctl(fd, EVIOCGBIT(0, EV_CNT), evbits) < 0) {
                return DeviceType::UNKNOWN;
            }

            const bool hasKeys = test_bit(EV_KEY, evbits);
            const bool hasRel = test_bit(EV_REL, evbits);
            const bool hasAbs = test_bit(EV_ABS, evbits);

            DeviceType deviceType = DeviceType::UNKNOWN;

            if (hasKeys) {
                ioctl(fd, EVIOCGBIT(EV_KEY, KEY_CNT), keybits);

                if (test_bit(KEY_A, keybits) && test_bit(KEY_Z, keybits)) {
                    deviceType = DeviceType::KEYBOARD;
                } else if (test_bit(BTN_LEFT, keybits) && test_bit(BTN_RIGHT, keybits)) {
                    deviceType = DeviceType::MOUSE;
                } else if (test_bit(BTN_TOUCH, keybits) && hasAbs) {
                    deviceType = DeviceType::TOUCHPAD;
                }
            }

            if (deviceType == DeviceType::UNKNOWN && hasRel) {
                ioctl(fd, EVIOCGBIT(EV_REL, REL_CNT), relbits);
                if (test_bit(REL_X, relbits) && test_bit(REL_Y, relbits)) {
                    deviceType = DeviceType::MOUSE;
                }
            }

            return deviceType;
        }

        bool hasCapability(const std::string& devicePath, int capability) {
            ScopedFd fd(devicePath);
            return fd.valid() && hasCapability(fd.get(), capability);
        }

        bool hasCapability(int fd, int capability) {
            if (fd < 0) return false;

            unsigned long evbits[BITS_TO_LONGS(EV_CNT)]{};
            if (ioctl(fd, EVIOCGBIT(0, EV_CNT), evbits) < 0) {
                return false;
            }

            return test_bit(capability, evbits);
        }

        std::vector<int> getSupportedKeys(const std::string& devicePath) {
            ScopedFd fd(devicePath);
            return fd.valid() ? getSupportedKeys(fd.get()) : std::vector<int>{};
        }

        std::vector<int> getSupportedKeys(int fd) {
            std::vector<int> keys;
            if (fd < 0) return keys;

            unsigned long keybits[BITS_TO_LONGS(KEY_CNT)]{};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, KEY_CNT), keybits) >= 0) {
                for (int i = 0; i < KEY_CNT; i++) {
                    if (test_bit(i, keybits)) {
                        keys.push_back(i);
                    }
                }
            }

            return keys;
        }

        std::vector<int> getSupportedAxes(const std::string& devicePath) {
            ScopedFd fd(devicePath);
            return fd.valid() ? getSupportedAxes(fd.get()) : std::vector<int>{};
        }

        std::vector<int> getSupportedAxes(int fd) {
            std::vector<int> axes;
            if (fd < 0) return axes;

            unsigned long absbits[BITS_TO_LONGS(ABS_CNT)]{};
            if (ioctl(fd, EVIOCGBIT(EV_ABS, ABS_CNT), absbits) >= 0) {
                for (int i = 0; i < ABS_CNT; i++) {
                    if (test_bit(i, absbits)) {
                        axes.push_back(i);
                    }
                }
            }

            return axes;
        }

        types::iVector2 getDeviceResolution(const std::string& devicePath) {
            ScopedFd fd(devicePath);
            return fd.valid() ? getDeviceResolution(fd.get()) : types::iVector2(0, 0);
        }

        types::iVector2 getDeviceResolution(int fd) {
            if (fd < 0) return types::iVector2(0, 0);

            struct input_absinfo absinfo;
            types::iVector2 resolution(0, 0);

            if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) >= 0) {
                resolution.x = absinfo.resolution;
            }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) >= 0) {
                resolution.y = absinfo.resolution;
            }

            return resolution;
        }

        types::iVector2 getAxisRange(const std::string& devicePath, int axis) {
            ScopedFd fd(devicePath);
            return fd.valid() ? getAxisRange(fd.get(), axis) : types::iVector2(0, 0);
        }

        types::iVector2 getAxisRange(int fd, int axis) {
            if (fd < 0) return types::iVector2(0, 0);

            struct input_absinfo absinfo;
            types::iVector2 range(0, 0);

            if (ioctl(fd, EVIOCGABS(axis), &absinfo) >= 0) {
                range.x = absinfo.minimum;
                range.y = absinfo.maximum;
            }

            return range;
        }
    }
}