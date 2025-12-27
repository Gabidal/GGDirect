#include "config.h"
#include "logger.h"
#include "window.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <unistd.h>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <libevdev/libevdev.h>
#include <cctype>

namespace config {

    // Dynamic key mapping built from libevdev at runtime for generality
    static std::unordered_map<std::string, int> keyNameToCode;
    static std::unordered_map<int, std::string> codeToKeyName;

    static inline std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
        return s;
    }

    static inline std::string stripKeyPrefix(const std::string& name) {
        // Convert "KEY_ENTER" -> "enter"
        if (name.rfind("KEY_", 0) == 0) {
            return toLower(name.substr(4));
        }
        return toLower(name);
    }

    static inline std::string toFriendlyKeyName(const std::string& kernelName) {
        std::string s = stripKeyPrefix(kernelName);
        // Make function keys like F1..F24 lowercase (libevdev already uses F1)
        return s;
    }

    static void ensureKeyMappingsBuilt() {
        if (!keyNameToCode.empty() && !codeToKeyName.empty()) return;

        // Build from libevdev names for all EV_KEY codes
        for (int code = 0; code <= KEY_MAX; ++code) {
            const char* nm = libevdev_event_code_get_name(EV_KEY, code);
            if (!nm) continue;
            std::string kernelName(nm);
            std::string friendly = toFriendlyKeyName(kernelName);
            codeToKeyName[code] = friendly;               // e.g., "enter"
            keyNameToCode[friendly] = code;               // primary alias

            // Also add the raw kernel name (lowercased) as an alias, e.g., "key_enter"
            keyNameToCode[toLower(kernelName)] = code;

            // Function keys: map "f1".."f24"
            if (friendly.size() > 1 && friendly[0] == 'f') {
                keyNameToCode[friendly] = code;
            }
        }

        // Minimal punctuation aliases for terminal-like input (ANSI-ish)
        // These complement the generic generation above for symbols that don't map as KEY_+
        auto addAlias = [](const std::string& alias, int code){ keyNameToCode[alias] = code; };
        addAlias("-", KEY_MINUS);
        addAlias("=", KEY_EQUAL);
        addAlias("[", KEY_LEFTBRACE);
        addAlias("]", KEY_RIGHTBRACE);
        addAlias(";", KEY_SEMICOLON);
        addAlias("'", KEY_APOSTROPHE);
        addAlias(",", KEY_COMMA);
        addAlias(".", KEY_DOT);
        addAlias("/", KEY_SLASH);
        addAlias("\\", KEY_BACKSLASH);
        addAlias("`", KEY_GRAVE);

        // Common synonyms
        auto aliasIfPresent = [&](const char* alias, const char* kernel){
            int c = libevdev_event_code_from_name(EV_KEY, kernel);
            if (c > 0) keyNameToCode[alias] = c;
        };
        aliasIfPresent("esc", "KEY_ESC");
        aliasIfPresent("escape", "KEY_ESC");
        aliasIfPresent("return", "KEY_ENTER");
        aliasIfPresent("super", "KEY_LEFTMETA");
        aliasIfPresent("meta", "KEY_LEFTMETA");
        aliasIfPresent("win", "KEY_LEFTMETA");

        // Alphanumeric single-char shortcuts (e.g., 'a' -> KEY_A, '1' -> KEY_1)
        for (char ch = 'a'; ch <= 'z'; ++ch) {
            std::string key = std::string("KEY_") + static_cast<char>(std::toupper(ch));
            int code = libevdev_event_code_from_name(EV_KEY, key.c_str());
            if (code > 0) keyNameToCode[std::string(1, ch)] = code;
        }
        for (char d = '0'; d <= '9'; ++d) {
            std::string key = std::string("KEY_") + d;
            int code = libevdev_event_code_from_name(EV_KEY, key.c_str());
            if (code > 0) keyNameToCode[std::string(1, d)] = code;
        }
    }

    // KeyCombination implementation
    std::string KeyCombination::toString() const {
    ensureKeyMappingsBuilt();
        
        std::string result;
        if (ctrl) result += "ctrl+";
        if (alt) result += "alt+";
        if (shift) result += "shift+";
        if (super) result += "super+";
        
        auto it = codeToKeyName.find(keyCode);
        if (it != codeToKeyName.end()) {
            result += it->second; // user-friendly name from libevdev
        } else {
            // Fall back to kernel name if available
            if (const char* nm = libevdev_event_code_get_name(EV_KEY, keyCode)) {
                result += stripKeyPrefix(nm);
            } else {
                result += "key" + std::to_string(keyCode);
            }
        }
        
        return result;
    }

    KeyCombination KeyCombination::fromString(const std::string& str) {
        KeyCombination result;
        std::string remaining = str;
        
        // Parse modifiers
        size_t pos = 0;
        while ((pos = remaining.find('+')) != std::string::npos) {
            std::string modifier = remaining.substr(0, pos);
            std::transform(modifier.begin(), modifier.end(), modifier.begin(), ::tolower);
            
            if (modifier == "ctrl") result.ctrl = true;
            else if (modifier == "alt") result.alt = true;
            else if (modifier == "shift") result.shift = true;
            else if (modifier == "super" || modifier == "meta" || modifier == "win") result.super = true;
            
            remaining = remaining.substr(pos + 1);
        }
        
        // Parse main key
        remaining = toLower(remaining);
        ensureKeyMappingsBuilt();
        auto it = keyNameToCode.find(remaining);
        if (it != keyNameToCode.end()) {
            result.keyCode = it->second;
        } else if (!remaining.empty()) {
            // Try kernel-style lookup by constructing KEY_* name
            std::string upper;
            upper.reserve(remaining.size());
            for (char c : remaining) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            std::string kernel = "KEY_" + upper;
            int code = libevdev_event_code_from_name(EV_KEY, kernel.c_str());
            if (code > 0) {
                result.keyCode = code;
            } else if (remaining.rfind("key", 0) == 0 && remaining.size() > 3) {
                // As a last resort, allow numeric raw code e.g., key30
                try {
                    result.keyCode = std::stoi(remaining.substr(3));
                } catch (...) {
                    result.keyCode = 0;
                }
            }
        }
        
        return result;
    }

    /**
     * @brief Executes the action associated with this Action instance.
     *
     * Depending on the ActionType, this method performs various window management operations,
     * such as switching focus between windows, moving the focused window to a specific position,
     * closing the focused window, adjusting zoom, or executing a custom callback.
     *
     * The method handles the following ActionTypes:
     * - SWITCH_FOCUS_NEXT: Switches focus to the next available window that is not marked for removal.
     * - SWITCH_FOCUS_PREVIOUS: Switches focus to the previous available window that is not marked for removal.
     * - MOVE_WINDOW_FULLSCREEN: Moves the currently focused window to fullscreen.
     * - MOVE_WINDOW_LEFT: Moves the currently focused window to the left half of the screen.
     * - MOVE_WINDOW_RIGHT: Moves the currently focused window to the right half of the screen.
     * - MOVE_WINDOW_TOP: Moves the currently focused window to the top half of the screen.
     * - MOVE_WINDOW_BOTTOM: Moves the currently focused window to the bottom half of the screen.
     * - CLOSE_FOCUSED_WINDOW: Closes the currently focused window.
     * - TOGGLE_ZOOM: Toggles the zoom level of the currently focused window between 1.0x and 1.5x.
     * - INCREASE_ZOOM: Increases the zoom level of the currently focused window, up to a maximum of 3.0x.
     * - DECREASE_ZOOM: Decreases the zoom level of the currently focused window, down to a minimum of 0.5x.
     * - CUSTOM: Executes a user-defined callback if provided; logs an error otherwise.
     *
     * Logs information or errors for each action performed.
     */
    void Action::execute() const {
        char packetBuffer[packet::size];
        packet::base* basePacket = (packet::base*)packetBuffer;
        
        bool closeConnectionAfterwards = false;

        window::handle* current = window::manager::getFocusedHandle();

        if (!current)   // No valid handle found
            return;
        // Focus switching
        if (hasFlag(flags, ActionBits::SWITCH_FOCUS_NEXT)) {
            window::manager::setFocusOnNextAvailableHandle();
        } else if (hasFlag(flags, ActionBits::SWITCH_FOCUS_PREV)) {
            window::manager::setFocusOnNextAvailableHandle();
        }

        // Closing
        else if (hasFlag(flags, ActionBits::CLOSE_WINDOW)) {
            if (!current->connection.isClosed()) {
                LOG_INFO() << "Closing window: " << current->name << std::endl;
                new(packetBuffer) packet::notify::base(packet::notify::type::CLOSED);
                closeConnectionAfterwards = true;
                current->set(window::stain::type::closed, true);
            }
        }

        // Zooms
        else if (hasFlag(flags, ActionBits::TOGGLE_ZOOM)) {
            current->zoom = (current->zoom == 1.0f) ? 1.5f : 1.0f;
            LOG_INFO() << "Toggled zoom for window: " << current->name << " (zoom: " << current->zoom << ")" << std::endl;
        } else if (hasFlag(flags, ActionBits::ZOOM_IN)) {
            current->zoom = std::min(current->zoom + 0.1f, 3.0f);
            LOG_INFO() << "Increased zoom for window: " << current->name << " (zoom: " << current->zoom << ")" << std::endl;
        } else if (hasFlag(flags, ActionBits::ZOOM_OUT)) {
            current->zoom = std::max(current->zoom - 0.1f, 0.5f);
            LOG_INFO() << "Decreased zoom for window: " << current->name << " (zoom: " << current->zoom << ")" << std::endl;
        }

        // Movement and fullscreen
        else if (hasFlag(flags, ActionBits::FULLSCREEN)) {
            if (current->preset != window::position::FULLSCREEN) {
                current->previousPreset = current->preset;
                current->preset = window::position::FULLSCREEN;
                LOG_INFO() << "Moved window to fullscreen: " << current->name << std::endl;
                types::rectangle newrect = window::positionToPixelCoordinates(window::position::FULLSCREEN, current->displayId);
                new(packetBuffer) packet::resize::base(types::cellCoordinates(newrect.size));
                current->set(window::stain::type::resize, true);
            }
        } else if (hasFlag(flags, ActionBits::MOVE)) {
            // Resolve direction(s)
            bool up = hasFlag(flags, ActionBits::DIR_UP);
            bool down = hasFlag(flags, ActionBits::DIR_DOWN);
            bool left = hasFlag(flags, ActionBits::DIR_LEFT);
            bool right = hasFlag(flags, ActionBits::DIR_RIGHT);

            window::position target = current->preset;
            bool found = true;
            if (up && left) target = window::position::TOP_LEFT;
            else if (up && right) target = window::position::TOP_RIGHT;
            else if (down && left) target = window::position::BOTTOM_LEFT;
            else if (down && right) target = window::position::BOTTOM_RIGHT;
            else if (up) target = window::position::TOP;
            else if (down) target = window::position::BOTTOM;
            else if (left) target = window::position::LEFT;
            else if (right) target = window::position::RIGHT;
            else found = false;

            if (found && current->preset != target) {
                current->previousPreset = current->preset;
                current->preset = target;
                LOG_INFO() << "Moved window: " << current->name << std::endl;
                types::rectangle newrect = window::positionToPixelCoordinates(target, current->displayId);
                new(packetBuffer) packet::resize::base(types::cellCoordinates(newrect.size));
                current->set(window::stain::type::resize, true);
            }
        } else if (hasFlag(flags, ActionBits::CUSTOM)) {
            if (callback) { callback(); }
            else { LOG_ERROR() << "Custom action without callback: " << customCommand << std::endl; }
        } else {
            LOG_ERROR() << "Unknown action flags executed: " << static_cast<uint32_t>(flags) << std::endl;
        }

        if (basePacket->packetType == packet::type::UNKNOWN)
            return; // no valid packets to send

        if (!current->connection.Send(packetBuffer, packet::size)) {
            LOG_ERROR() << "Failed to send action packet to GGUI client" << std::endl;
        }

        if (closeConnectionAfterwards) {
            current->close();
        }
    }

    std::string Action::toString() const {
        if (hasFlag(flags, ActionBits::SWITCH_FOCUS_NEXT)) return "switch_focus_next";
        if (hasFlag(flags, ActionBits::SWITCH_FOCUS_PREV)) return "switch_focus_previous";
        if (hasFlag(flags, ActionBits::FULLSCREEN)) return "move_window_fullscreen";
        if (hasFlag(flags, ActionBits::MOVE)) {
            bool up = hasFlag(flags, ActionBits::DIR_UP);
            bool down = hasFlag(flags, ActionBits::DIR_DOWN);
            bool left = hasFlag(flags, ActionBits::DIR_LEFT);
            bool right = hasFlag(flags, ActionBits::DIR_RIGHT);
            if (up && left) return "move_window_top_left";
            if (up && right) return "move_window_top_right";
            if (down && left) return "move_window_bottom_left";
            if (down && right) return "move_window_bottom_right";
            if (up) return "move_window_top";
            if (down) return "move_window_bottom";
            if (left) return "move_window_left";
            if (right) return "move_window_right";
            return "move_window";
        }
        if (hasFlag(flags, ActionBits::CLOSE_WINDOW)) return "close_focused_window";
        if (hasFlag(flags, ActionBits::TOGGLE_ZOOM)) return "toggle_zoom";
        if (hasFlag(flags, ActionBits::ZOOM_IN)) return "increase_zoom";
        if (hasFlag(flags, ActionBits::ZOOM_OUT)) return "decrease_zoom";
        if (hasFlag(flags, ActionBits::CUSTOM)) return "custom:" + customCommand;
        return "unknown";
    }

    Action Action::fromString(const std::string& str) {
        if (str == "switch_focus_next") return Action(ActionBits::SWITCH_FOCUS_NEXT);
        if (str == "switch_focus_previous") return Action(ActionBits::SWITCH_FOCUS_PREV);
        if (str == "move_window_fullscreen") return Action(ActionBits::FULLSCREEN);
        if (str == "move_window_left") return Action(ActionBits::MOVE | ActionBits::DIR_LEFT);
        if (str == "move_window_right") return Action(ActionBits::MOVE | ActionBits::DIR_RIGHT);
        if (str == "move_window_top") return Action(ActionBits::MOVE | ActionBits::DIR_UP);
        if (str == "move_window_bottom") return Action(ActionBits::MOVE | ActionBits::DIR_DOWN);
        if (str == "move_window_top_left") return Action(ActionBits::MOVE | ActionBits::DIR_UP | ActionBits::DIR_LEFT);
        if (str == "move_window_top_right") return Action(ActionBits::MOVE | ActionBits::DIR_UP | ActionBits::DIR_RIGHT);
        if (str == "move_window_bottom_left") return Action(ActionBits::MOVE | ActionBits::DIR_DOWN | ActionBits::DIR_LEFT);
        if (str == "move_window_bottom_right") return Action(ActionBits::MOVE | ActionBits::DIR_DOWN | ActionBits::DIR_RIGHT);
        if (str == "close_focused_window") return Action(ActionBits::CLOSE_WINDOW);
        if (str == "toggle_zoom") return Action(ActionBits::TOGGLE_ZOOM);
        if (str == "increase_zoom") return Action(ActionBits::ZOOM_IN);
        if (str == "decrease_zoom") return Action(ActionBits::ZOOM_OUT);
        if (str.substr(0, 7) == "custom:") return Action(str.substr(7));
        return Action(ActionBits::NONE);
    }

    // KeyBindSettings implementation
    void KeyBindSettings::loadDefaults() {
        windowManagement.clear();
        focusManagement.clear();
        customBinds.clear();
        
        // Focus management defaults
        focusManagement.emplace_back(
            KeyCombination(KEY_TAB, false, true, false, false), // Alt+Tab
            Action(ActionBits::SWITCH_FOCUS_NEXT),
            "Switch to next window"
        );
        
        focusManagement.emplace_back(
            KeyCombination(KEY_TAB, false, true, true, false), // Alt+Shift+Tab
            Action(ActionBits::SWITCH_FOCUS_PREV),
            "Switch to previous window"
        );
        
        // Window management defaults
        windowManagement.emplace_back(
            KeyCombination(KEY_UP, false, false, false, true), // Super+Up
            Action(ActionBits::MOVE | ActionBits::DIR_UP),
            "Move window to top half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_DOWN, false, false, false, true), // Super+Down
            Action(ActionBits::MOVE | ActionBits::DIR_DOWN),
            "Move window to bottom half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_LEFT, false, false, false, true), // Super+Left
            Action(ActionBits::MOVE | ActionBits::DIR_LEFT),
            "Move window to left half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_RIGHT, false, false, false, true), // Super+Right
            Action(ActionBits::MOVE | ActionBits::DIR_RIGHT),
            "Move window to right half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_F, false, false, false, true), // Super+F
            Action(ActionBits::FULLSCREEN),
            "Move window to fullscreen"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_Q, false, false, false, true), // Super+Q
            Action(ActionBits::CLOSE_WINDOW),
            "Close focused window"
        );
        
        // Zoom controls
        windowManagement.emplace_back(
            KeyCombination(KEY_EQUAL, true, false, false, false), // Ctrl+=
            Action(ActionBits::ZOOM_IN),
            "Increase zoom"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_MINUS, true, false, false, false), // Ctrl+-
            Action(ActionBits::ZOOM_OUT),
            "Decrease zoom"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_0, true, false, false, false), // Ctrl+0
            Action(ActionBits::TOGGLE_ZOOM),
            "Reset/toggle zoom"
        );
    }

    // DisplaySettings implementation
    void DisplaySettings::loadDefaults() {
        autoDistributeWindows = true;
        displayAssignmentStrategy = "FILL_THEN_NEXT";
        primaryDisplayId = 0;
        wallpaperPath = "";               // No wallpaper by default
        backgroundColorRGB = 0x00000000;  // Black in XRGB8888 format
    }

    // Helper function to parse hex color strings
    uint32_t parseHexColor(const std::string& hexColor) {
        if (hexColor.empty() || hexColor[0] != '#') {
            return 0x00000000; // Default to black
        }
        
        std::string hex = hexColor.substr(1); // Remove '#'
        if (hex.length() == 6) {
            try {
                uint32_t color = std::stoul(hex, nullptr, 16);
                return color; // Already in RGB format, suitable for XRGB8888
            } catch (const std::exception&) {
                return 0x00000000; // Default to black on error
            }
        }
        return 0x00000000; // Default to black for invalid format
    }

    // Simple bitmap image structure for wallpaper support
    struct BitmapImage {
        int width;
        int height;
        std::vector<uint32_t> pixels; // XRGB8888 format
        bool isValid;
        
        BitmapImage() : width(0), height(0), isValid(false) {}
        
        bool loadFromFile(const std::string& filePath) {
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                return false;
            }
            
            // Read BMP header (simplified - assumes 24-bit RGB bitmap)
            char header[54];
            file.read(header, 54);
            
            if (header[0] != 'B' || header[1] != 'M') {
                return false; // Not a valid BMP file
            }
            
            // Extract dimensions (little-endian)
            width = *reinterpret_cast<int*>(&header[18]);
            height = *reinterpret_cast<int*>(&header[22]);
            
            // Check if it's 24-bit RGB
            int bitsPerPixel = *reinterpret_cast<short*>(&header[28]);
            if (bitsPerPixel != 24) {
                return false; // Only support 24-bit RGB for simplicity
            }
            
            // Calculate padding for 4-byte alignment
            int padding = (4 - (width * 3) % 4) % 4;
            
            // Read pixel data (BMP is stored bottom-up)
            pixels.resize(width * height);
            for (int y = height - 1; y >= 0; y--) {
                for (int x = 0; x < width; x++) {
                    char bgr[3];
                    file.read(bgr, 3);
                    
                    // Convert BGR to XRGB8888
                    uint32_t pixel = (static_cast<uint8_t>(bgr[2]) << 16) |  // R
                                   (static_cast<uint8_t>(bgr[1]) << 8) |   // G
                                   static_cast<uint8_t>(bgr[0]);           // B
                    
                    pixels[y * width + x] = pixel;
                }
                
                // Skip padding bytes
                file.seekg(padding, std::ios::cur);
            }
            
            isValid = true;
            return true;
        }
        
        // Get pixel at specific coordinates with bounds checking
        uint32_t getPixel(int x, int y) const {
            if (!isValid || x < 0 || y < 0 || x >= width || y >= height) {
                return 0x00000000; // Black default
            }
            return pixels[y * width + x];
        }
        
        // Get a pointer to a rectangular region of pixels for efficient copying
        const uint32_t* getRegionData(int startX, int startY, int regionWidth, int regionHeight) const {
            if (!isValid || startX < 0 || startY < 0 || 
                startX + regionWidth > width || startY + regionHeight > height ||
                regionWidth <= 0 || regionHeight <= 0) {
                return nullptr;
            }
            return &pixels[startY * width + startX];
        }
        
        // Copy a rectangular region to a destination buffer
        bool copyRegionToBuffer(int startX, int startY, int regionWidth, int regionHeight, uint32_t* destBuffer, int destWidth) const {
            if (!isValid || startX < 0 || startY < 0 || startX + regionWidth > width || startY + regionHeight > height || regionWidth <= 0 || regionHeight <= 0 || destBuffer == nullptr) {
                return false;
            }
            
            // Copy row by row for efficiency
            for (int y = 0; y < regionHeight; y++) {
                const uint32_t* srcRow = &pixels[(startY + y) * width + startX];
                uint32_t* destRow = &destBuffer[y * destWidth];
                std::memcpy(destRow, srcRow, regionWidth * sizeof(uint32_t));
            }
            
            return true;
        }
    };
    
    // Global wallpaper image instance
    static BitmapImage wallpaperImage;

    // Configuration implementation
    void Configuration::loadDefaults() {
        keybinds.loadDefaults();
        display.loadDefaults();
        
        input.enableGlobalKeybinds = true;
        input.inputPollRate = 60;
        
        configVersion = "1.0";
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        lastModified = ss.str();
    }

    // ConfigurationManager implementation
    ConfigurationManager::ConfigurationManager() {
        config.loadDefaults();
        rebuildKeybindMap();
    }

    bool ConfigurationManager::load(const std::string& configPath) {
        std::string path;
        
        if (!configPath.empty()) {
            // User provided explicit path
            path = configPath;
        } else {
            // Check for local config file first (same directory as executable)
            std::string localPath = getLocalConfigPath();
            if (utils::fileExists(localPath)) {
                path = localPath;
                LOG_INFO() << "Using local config file: " << path << std::endl;
            } else {
                // Local config doesn't exist, create it with defaults
                LOG_INFO() << "No local config found, creating default config: " << localPath << std::endl;
                config.loadDefaults();
                rebuildKeybindMap();
                if (createDefaultConfig(localPath)) {
                    path = localPath;
                    configFilePath = path;
                    LOG_INFO() << "Default configuration created successfully at: " << path << std::endl;
                    return true;
                } else {
                    LOG_ERROR() << "Failed to create default config file: " << localPath << std::endl;
                    return false;
                }
            }
        }
        
        configFilePath = path;
        
        if (!utils::fileExists(path)) {
            LOG_ERROR() << "Configuration file not found: " << path << std::endl;
            return false;
        }
        
        if (loadFromFile(path)) {
            rebuildKeybindMap();
            LOG_INFO() << "Configuration loaded successfully from: " << path << std::endl;
            return true;
        }
        
        LOG_ERROR() << "Failed to load configuration from: " << path << std::endl;
        return false;
    }

    // Loads custom or user given configuration and then saves it back if empty.
    bool ConfigurationManager::save(const std::string& configPath) const {
        std::string path = configPath.empty() ? configFilePath : configPath;
        if (path.empty()) {
            path = getDefaultConfigPath();
        }
        
        return saveToFile(path);
    }

    bool ConfigurationManager::reload() {
        return load(configFilePath);
    }

    bool ConfigurationManager::loadFromFile(const std::string& filePath) {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            return false;
        }
        
        // Simple JSON-like parsing (for a full implementation, consider using a JSON library)
        std::string line;
        std::string section;
        config.loadDefaults(); // Start with defaults
        
        while (std::getline(file, line)) {
            // Remove whitespace
            line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());
            
            if (line.empty() || line[0] == '#' || line.substr(0, 2) == "//") {
                continue; // Skip comments and empty lines
            }
            
            // Simple section detection
            if (line.find("\"keybinds\"") != std::string::npos) {
                section = "keybinds";
            } else if (line.find("\"display\"") != std::string::npos) {
                section = "display";
            } else if (line.find("\"input\"") != std::string::npos) {
                section = "input";
            }
            
            // Parse key-value pairs (simplified)
            size_t colonPos = line.find(':');
            if (colonPos != std::string::npos && section == "keybinds") {
                // Parse keybind entries
                size_t keyStart = line.find('"');
                size_t keyEnd = line.find('"', keyStart + 1);
                size_t valueStart = line.find('"', colonPos);
                size_t valueEnd = line.find('"', valueStart + 1);
                
                if (keyStart != std::string::npos && keyEnd != std::string::npos &&
                    valueStart != std::string::npos && valueEnd != std::string::npos) {
                    std::string keyStr = line.substr(keyStart + 1, keyEnd - keyStart - 1);
                    std::string actionStr = line.substr(valueStart + 1, valueEnd - valueStart - 1);
                    
                    KeyCombination key = KeyCombination::fromString(keyStr);
                    Action action = Action::fromString(actionStr);
                    
                    if (key.keyCode != 0 && action.flags != ActionBits::NONE) {
                        addKeybind(key, action, "Loaded from config");
                    }
                }
            } else if (colonPos != std::string::npos && section == "display") {
                // Parse display settings
                size_t keyStart = line.find('"');
                size_t keyEnd = line.find('"', keyStart + 1);
                size_t valueStart = line.find('"', colonPos);
                size_t valueEnd = line.find('"', valueStart + 1);
                
                if (keyStart != std::string::npos && keyEnd != std::string::npos) {
                    std::string key = line.substr(keyStart + 1, keyEnd - keyStart - 1);
                    
                    if (key == "backgroundColor" && valueStart != std::string::npos && valueEnd != std::string::npos) {
                        // Convert hex string to RGB value
                        config.display.backgroundColorRGB = parseHexColor(line.substr(valueStart + 1, valueEnd - valueStart - 1));
                    } else if (key == "wallpaperPath" && valueStart != std::string::npos && valueEnd != std::string::npos) {
                        config.display.wallpaperPath = line.substr(valueStart + 1, valueEnd - valueStart - 1);
                    } else if (key == "autoDistributeWindows") {
                        // Parse boolean value
                        size_t boolStart = line.find_first_of("tf", colonPos); // true/false
                        if (boolStart != std::string::npos) {
                            config.display.autoDistributeWindows = line.substr(boolStart, 4) == "true";
                        }
                    } else if (key == "displayAssignmentStrategy" && valueStart != std::string::npos && valueEnd != std::string::npos) {
                        config.display.displayAssignmentStrategy = line.substr(valueStart + 1, valueEnd - valueStart - 1);
                    } else if (key == "primaryDisplayId") {
                        // Parse numeric value
                        size_t numStart = line.find_first_of("0123456789", colonPos);
                        if (numStart != std::string::npos) {
                            config.display.primaryDisplayId = std::stoul(line.substr(numStart));
                        }
                    }
                }
            }
        }
        
        return true;
    }

    bool ConfigurationManager::saveToFile(const std::string& filePath) const {
        // Ensure directory exists
        std::filesystem::path path(filePath);
        std::filesystem::create_directories(path.parent_path());
        
        std::ofstream file(filePath);
        if (!file.is_open()) {
            return false;
        }
        
        file << "{\n";
        file << "  \"configVersion\": \"" << config.configVersion << "\",\n";
        file << "  \"lastModified\": \"" << config.lastModified << "\",\n";
        file << "  \"keybinds\": {\n";
        file << "    \"focusManagement\": {\n";
        
        // Write focus management keybinds
        for (size_t i = 0; i < config.keybinds.focusManagement.size(); ++i) {
            const auto& kb = config.keybinds.focusManagement[i];
            file << "      \"" << kb.key.toString() << "\": \"" << kb.action.toString() << "\"";
            if (i < config.keybinds.focusManagement.size() - 1) file << ", \n";
        }
        
        file << "    },\n";
        file << "    \"windowManagement\": {\n";
        
        // Write window management keybinds
        for (size_t i = 0; i < config.keybinds.windowManagement.size(); ++i) {
            const auto& kb = config.keybinds.windowManagement[i];
            file << "      \"" << kb.key.toString() << "\": \"" << kb.action.toString() << "\"";
            if (i < config.keybinds.windowManagement.size() - 1) file << ",\n";
        }
        
        file << "    },\n";
        file << "    \"customBinds\": {\n";
        
        // Write custom keybinds
        for (size_t i = 0; i < config.keybinds.customBinds.size(); ++i) {
            const auto& kb = config.keybinds.customBinds[i];
            file << "      \"" << kb.key.toString() << "\": \"" << kb.action.toString() << "\"";
            if (i < config.keybinds.customBinds.size() - 1) file << ",\n";
        }
        
        file << "    }\n";
        file << "  },\n";
        file << "  \"display\": {\n";
        file << "    \"autoDistributeWindows\": " << (config.display.autoDistributeWindows ? "true" : "false") << ",\n";
        file << "    \"displayAssignmentStrategy\": \"" << config.display.displayAssignmentStrategy << "\",\n";
        file << "    \"primaryDisplayId\": " << config.display.primaryDisplayId << ",\n";
        file << "    \"backgroundColor\": \"" << config.display.backgroundColor << "\",\n";
        file << "    \"wallpaperPath\": \"" << config.display.wallpaperPath << "\"\n";
        file << "  },\n";
        file << "  \"input\": {\n";
        file << "    \"enableGlobalKeybinds\": " << (config.input.enableGlobalKeybinds ? "true" : "false") << ",\n";
        file << "    \"inputPollRate\": " << config.input.inputPollRate << "\n";
        file << "  }\n";
        file << "}\n";
        
        return true;
    }

    bool ConfigurationManager::createDefaultConfig(const std::string& filePath) {
        // Create a temporary configuration with defaults
        Configuration defaultConfig;
        defaultConfig.loadDefaults();
        
        // Ensure directory exists
        std::filesystem::path path(filePath);
        std::filesystem::create_directories(path.parent_path());
        
        std::ofstream file(filePath);
        if (!file.is_open()) {
            return false;
        }
        
        // Write the default configuration without comments
        file << "{\n";
        file << "  \"configVersion\": \"" << defaultConfig.configVersion << "\",\n";
        file << "  \"lastModified\": \"" << defaultConfig.lastModified << "\",\n";
        file << "  \"keybinds\": {\n";
        file << "    \"focusManagement\": {\n";
        
        // Write focus management keybinds
        for (size_t i = 0; i < defaultConfig.keybinds.focusManagement.size(); ++i) {
            const auto& kb = defaultConfig.keybinds.focusManagement[i];
            file << "      \"" << kb.key.toString() << "\": \"" << kb.action.toString() << "\"";
            if (i < defaultConfig.keybinds.focusManagement.size() - 1) file << ",";
            file << "\n";
        }
        
        file << "    },\n";
        file << "    \"windowManagement\": {\n";
        
        // Write window management keybinds
        for (size_t i = 0; i < defaultConfig.keybinds.windowManagement.size(); ++i) {
            const auto& kb = defaultConfig.keybinds.windowManagement[i];
            file << "      \"" << kb.key.toString() << "\": \"" << kb.action.toString() << "\"";
            if (i < defaultConfig.keybinds.windowManagement.size() - 1) file << ",";
            file << "\n";
        }
        
        file << "    },\n";
        file << "    \"customBinds\": {\n";
        file << "    }\n";
        file << "  },\n";
        file << "  \"display\": {\n";
        file << "    \"autoDistributeWindows\": " << (defaultConfig.display.autoDistributeWindows ? "true" : "false") << ",\n";
        file << "    \"displayAssignmentStrategy\": \"" << defaultConfig.display.displayAssignmentStrategy << "\",\n";
        file << "    \"primaryDisplayId\": " << defaultConfig.display.primaryDisplayId << "\n";
        file << "  },\n";
        file << "  \"input\": {\n";
        file << "    \"enableGlobalKeybinds\": " << (defaultConfig.input.enableGlobalKeybinds ? "true" : "false") << ",\n";
        file << "    \"inputPollRate\": " << defaultConfig.input.inputPollRate << "\n";
        file << "  }\n";
        file << "}\n";
        
        return true;
    }

    void ConfigurationManager::rebuildKeybindMap() {
        activeKeybinds.clear();
        
        // Add all keybinds to the active map
        for (const auto& kb : config.keybinds.focusManagement) {
            if (kb.enabled) {
                activeKeybinds[kb.key] = kb.action;
            }
        }
        
        for (const auto& kb : config.keybinds.windowManagement) {
            if (kb.enabled) {
                activeKeybinds[kb.key] = kb.action;
            }
        }
        
        for (const auto& kb : config.keybinds.customBinds) {
            if (kb.enabled) {
                activeKeybinds[kb.key] = kb.action;
            }
        }
    }

    bool ConfigurationManager::addKeybind(const KeyCombination& key, const Action& action, const std::string& description) {
        KeyBind newBind(key, action, description);
        
        // Determine which category to add to
        if (hasFlag(action.flags, ActionBits::SWITCH_FOCUS_NEXT) || hasFlag(action.flags, ActionBits::SWITCH_FOCUS_PREV)) {
            config.keybinds.focusManagement.push_back(newBind);
        } else if (hasFlag(action.flags, ActionBits::MOVE) || hasFlag(action.flags, ActionBits::FULLSCREEN) ||
                   hasFlag(action.flags, ActionBits::CLOSE_WINDOW) || hasFlag(action.flags, ActionBits::TOGGLE_ZOOM) ||
                   hasFlag(action.flags, ActionBits::ZOOM_IN) || hasFlag(action.flags, ActionBits::ZOOM_OUT)) {
            config.keybinds.windowManagement.push_back(newBind);
        } else {
            config.keybinds.customBinds.push_back(newBind);
        }
        
        rebuildKeybindMap();
        return true;
    }

    bool ConfigurationManager::removeKeybind(const KeyCombination& key) {
        bool removed = false;
        
        // Remove from all categories
        auto removeFromVector = [&key, &removed](std::vector<KeyBind>& vec) {
            auto it = std::remove_if(vec.begin(), vec.end(),
                [&key](const KeyBind& kb) { return kb.key == key; });
            if (it != vec.end()) {
                vec.erase(it, vec.end());
                removed = true;
            }
        };
        
        removeFromVector(config.keybinds.focusManagement);
        removeFromVector(config.keybinds.windowManagement);
        removeFromVector(config.keybinds.customBinds);
        
        if (removed) {
            rebuildKeybindMap();
        }
        
        return removed;
    }

    bool ConfigurationManager::isKeybindActive(const KeyCombination& key) const {
        return activeKeybinds.find(key) != activeKeybinds.end();
    }

    Action ConfigurationManager::getAction(const KeyCombination& key) const {
        auto it = activeKeybinds.find(key);
        if (it != activeKeybinds.end()) {
            return it->second;
        }
    return Action(ActionBits::NONE);
    }

    bool ConfigurationManager::processKeyInput(const KeyCombination& key) {
        if (!config.input.enableGlobalKeybinds) {
            return false;
        }
        
        auto it = activeKeybinds.find(key);
        if (it != activeKeybinds.end()) {
            it->second.execute();
            return true; // Key was handled
        }
        
        return false; // Key was not handled
    }

    std::string ConfigurationManager::getDefaultConfigPath() {
        // Priority order: local (same directory) -> user -> system
        std::string localPath = getLocalConfigPath();
        if (utils::fileExists(localPath)) {
            return localPath;
        }
        
        std::string userPath = getUserConfigPath();
        if (utils::fileExists(userPath)) {
            return userPath;
        }
        
        // If neither exists, return local path for creation
        return localPath;
    }
    
    std::string ConfigurationManager::getLocalConfigPath() {
        return utils::getExecutableDirectory() + "/config.json";
    }

    std::string ConfigurationManager::getUserConfigPath() {
        return utils::getHomeDirectory() + "/.config/GGDirect/config.json";
    }

    std::string ConfigurationManager::getSystemConfigPath() {
        return "/etc/GGDirect/config.json";
    }

    // Global manager implementation
    namespace manager {
        atomic::guard<ConfigurationManager> configManager;
        
        void init() {
            LOG_INFO() << "Initializing configuration manager..." << std::endl;
            
            configManager([](ConfigurationManager& manager) {
                if (!manager.load()) {
                    LOG_ERROR() << "Failed to load configuration, using defaults" << std::endl;
                }
            });
        }
        
        void cleanup() {
            configManager([](ConfigurationManager& manager) {
                manager.save();
            });
            LOG_INFO() << "Configuration manager cleaned up." << std::endl;
        }
        
        bool processKeyInput(const KeyCombination& key) {
            bool result = false;
            configManager([&result, &key](ConfigurationManager& manager) {
                result = manager.processKeyInput(key);
            });
            return result;
        }
        
        bool loadConfiguration(const std::string& configPath) {
            bool result = false;
            configManager([&result, &configPath](ConfigurationManager& manager) {
                result = manager.load(configPath);
            });
            return result;
        }
        
        bool saveConfiguration(const std::string& configPath) {
            bool result = false;
            configManager([&result, &configPath](ConfigurationManager& manager) {
                result = manager.save(configPath);
            });
            return result;
        }
        
        void modifyConfig(std::function<void(Configuration&)> modifier) {
            configManager([&modifier](ConfigurationManager& manager) {
                modifier(manager.getConfiguration());
            });
        }
        
        Configuration getConfigCopy() {
            Configuration copy;
            configManager([&copy](ConfigurationManager& manager) {
                copy = manager.getConfiguration();
            });
            return copy;
        }
        
        bool addKeybind(const KeyCombination& key, const Action& action, const std::string& description) {
            bool result = false;
            configManager([&result, &key, &action, &description](ConfigurationManager& manager) {
                result = manager.addKeybind(key, action, description);
            });
            return result;
        }
        
        bool removeKeybind(const KeyCombination& key) {
            bool result = false;
            configManager([&result, &key](ConfigurationManager& manager) {
                result = manager.removeKeybind(key);
            });
            return result;
        }
        
        std::vector<KeyBind> getAllKeybinds() {
            std::vector<KeyBind> result;
            configManager([&result](ConfigurationManager& manager) {
                result = manager.getAllKeybinds();
            });
            return result;
        }

        bool isKeybindActive(const KeyCombination& key) {
            bool result = false;
            configManager([&](ConfigurationManager& manager){ result = manager.isKeybindActive(key); });
            return result;
        }

        Action getAction(const KeyCombination& key) {
            Action a;
            configManager([&](ConfigurationManager& manager){ a = manager.getAction(key); });
            return a;
        }
        
        uint32_t getBackgroundColor() {
            uint32_t result = 0x00000000;
            configManager([&result](ConfigurationManager& manager) {
                result = manager.getConfiguration().display.backgroundColorRGB;
            });
            return result;
        }
        
        std::string getWallpaperPath() {
            std::string result;
            configManager([&result](ConfigurationManager& manager) {
                result = manager.getConfiguration().display.wallpaperPath;
            });
            return result;
        }
        
        bool loadWallpaper(const std::string& wallpaperPath) {
            if (wallpaperPath.empty()) {
                wallpaperImage.isValid = false;
                return true; // No wallpaper is valid
            }
            
            if (wallpaperImage.loadFromFile(wallpaperPath)) {
                LOG_INFO() << "Wallpaper loaded successfully: " << wallpaperPath << " (" 
                          << wallpaperImage.width << "x" << wallpaperImage.height << ")" << std::endl;
                return true;
            } else {
                LOG_ERROR() << "Failed to load wallpaper: " << wallpaperPath << std::endl;
                wallpaperImage.isValid = false;
                return false;
            }
        }
        
        bool getWallpaperPixel(int x, int y, uint32_t& pixel) {
            if (!wallpaperImage.isValid) {
                return false;
            }
            
            pixel = wallpaperImage.getPixel(x, y);
            return true;
        }
        
        bool getWallpaperRegion(int startX, int startY, int regionWidth, int regionHeight, uint32_t* destBuffer, int destWidth) {
            if (!wallpaperImage.isValid || config::manager::getWallpaperPath().empty()) {
                return false;
            }
            
            return wallpaperImage.copyRegionToBuffer(startX, startY, regionWidth, regionHeight, destBuffer, destWidth);
        }
    }

    // Utility functions implementation
    namespace utils {
        int stringToKeyCode(const std::string& keyName) {
            std::string lowerKey = keyName;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
            ensureKeyMappingsBuilt();
            auto it = keyNameToCode.find(lowerKey);
            if (it != keyNameToCode.end()) return it->second;
            // Try kernel name form
            std::string upper;
            for (char c : lowerKey) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            int code = libevdev_event_code_from_name(EV_KEY, ("KEY_" + upper).c_str());
            return code > 0 ? code : 0;
        }
        
        std::string keyCodeToString(int keyCode) {
            ensureKeyMappingsBuilt();
            auto it = codeToKeyName.find(keyCode);
            if (it != codeToKeyName.end()) return it->second;
            if (const char* nm = libevdev_event_code_get_name(EV_KEY, keyCode)) return stripKeyPrefix(nm);
            return "key" + std::to_string(keyCode);
        }
        
        bool fileExists(const std::string& path) {
            return std::filesystem::exists(path);
        }
        
        bool createDirectory(const std::string& path) {
            return std::filesystem::create_directories(path);
        }
        
        std::string getHomeDirectory() {
            const char* home = std::getenv("HOME");
            return home ? std::string(home) : "/tmp";
        }
        
        std::string getConfigDirectory() {
            const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
            if (xdgConfig) {
                return std::string(xdgConfig);
            }
            return getHomeDirectory() + "/.config";
        }
        
        std::string getExecutableDirectory() {
            char exePath[1024];
            ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
            if (len != -1) {
                exePath[len] = '\0';
                std::filesystem::path path(exePath);
                return path.parent_path().string();
            }
            return getCurrentWorkingDirectory();
        }
        
        std::string getCurrentWorkingDirectory() {
            char* cwd = getcwd(nullptr, 0);
            if (cwd) {
                std::string result(cwd);
                free(cwd);
                return result;
            }
            return ".";
        }
    }

    std::vector<KeyBind> ConfigurationManager::getAllKeybinds() const {
        std::vector<KeyBind> result;
        
        result.insert(result.end(), config.keybinds.focusManagement.begin(), config.keybinds.focusManagement.end());
        result.insert(result.end(), config.keybinds.windowManagement.begin(), config.keybinds.windowManagement.end());
        result.insert(result.end(), config.keybinds.customBinds.begin(), config.keybinds.customBinds.end());
        
        return result;
    }
}
