#include "config.h"
#include "logger.h"
#include "window.h"
#include "input.h"

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

namespace config {

    // Static data for key code mappings
    static const std::map<std::string, int> keyNameToCode = {
        {"tab", KEY_TAB}, {"enter", KEY_ENTER}, {"escape", KEY_ESC}, {"space", KEY_SPACE},
        {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
        {"home", KEY_HOME}, {"end", KEY_END}, {"pageup", KEY_PAGEUP}, {"pagedown", KEY_PAGEDOWN},
        {"delete", KEY_DELETE}, {"backspace", KEY_BACKSPACE}, {"insert", KEY_INSERT},
        {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4}, {"f5", KEY_F5},
        {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8}, {"f9", KEY_F9}, {"f10", KEY_F10},
        {"f11", KEY_F11}, {"f12", KEY_F12},
        {"a", KEY_A}, {"b", KEY_B}, {"c", KEY_C}, {"d", KEY_D}, {"e", KEY_E}, {"f", KEY_F},
        {"g", KEY_G}, {"h", KEY_H}, {"i", KEY_I}, {"j", KEY_J}, {"k", KEY_K}, {"l", KEY_L},
        {"m", KEY_M}, {"n", KEY_N}, {"o", KEY_O}, {"p", KEY_P}, {"q", KEY_Q}, {"r", KEY_R},
        {"s", KEY_S}, {"t", KEY_T}, {"u", KEY_U}, {"v", KEY_V}, {"w", KEY_W}, {"x", KEY_X},
        {"y", KEY_Y}, {"z", KEY_Z},
        {"0", KEY_0}, {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5},
        {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9}
    };

    static std::map<int, std::string> codeToKeyName;

    // Initialize reverse mapping
    void initializeKeyMappings() {
        if (codeToKeyName.empty()) {
            for (const auto& pair : keyNameToCode) {
                codeToKeyName[pair.second] = pair.first;
            }
        }
    }

    // KeyCombination implementation
    std::string KeyCombination::toString() const {
        initializeKeyMappings();
        
        std::string result;
        if (ctrl) result += "ctrl+";
        if (alt) result += "alt+";
        if (shift) result += "shift+";
        if (super) result += "super+";
        
        auto it = codeToKeyName.find(keyCode);
        if (it != codeToKeyName.end()) {
            result += it->second;
        } else {
            result += "key" + std::to_string(keyCode);
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
        std::transform(remaining.begin(), remaining.end(), remaining.begin(), ::tolower);
        auto it = keyNameToCode.find(remaining);
        if (it != keyNameToCode.end()) {
            result.keyCode = it->second;
        } else if (remaining.substr(0, 3) == "key" && remaining.length() > 3) {
            try {
                result.keyCode = std::stoi(remaining.substr(3));
            } catch (...) {
                result.keyCode = 0;
            }
        }
        
        return result;
    }

    // Action implementation
    void Action::execute() const {
        switch (type) {
            case ActionType::SWITCH_FOCUS_NEXT: {
                window::manager::handles([](std::vector<window::handle>& handles) {
                    if (handles.size() > 1) {
                        window::handle* current = window::manager::getFocusedHandle();
                        if (current && !current->shouldRemove) {
                            // Find current handle index
                            for (size_t i = 0; i < handles.size(); ++i) {
                                if (&handles[i] == current) {
                                    size_t nextIndex = (i + 1) % handles.size();
                                    // Skip handles marked for removal
                                    while (nextIndex != i && handles[nextIndex].shouldRemove) {
                                        nextIndex = (nextIndex + 1) % handles.size();
                                    }
                                    if (!handles[nextIndex].shouldRemove) {
                                        window::manager::setFocusedHandleByIndex(nextIndex);
                                        LOG_INFO() << "Switched focus to window: " << handles[nextIndex].name << std::endl;
                                    }
                                    break;
                                }
                            }
                        } else {
                            // Find first handle that is not marked for removal
                            for (size_t i = 0; i < handles.size(); ++i) {
                                if (!handles[i].shouldRemove) {
                                    window::manager::setFocusedHandleByIndex(i);
                                    break;
                                }
                            }
                        }
                    }
                });
                break;
            }
            case ActionType::SWITCH_FOCUS_PREVIOUS: {
                window::manager::handles([](std::vector<window::handle>& handles) {
                    if (handles.size() > 1) {
                        window::handle* current = window::manager::getFocusedHandle();
                        if (current && !current->shouldRemove) {
                            for (size_t i = 0; i < handles.size(); ++i) {
                                if (&handles[i] == current) {
                                    size_t prevIndex = (i == 0) ? handles.size() - 1 : i - 1;
                                    // Skip handles marked for removal
                                    while (prevIndex != i && handles[prevIndex].shouldRemove) {
                                        prevIndex = (prevIndex == 0) ? handles.size() - 1 : prevIndex - 1;
                                    }
                                    if (!handles[prevIndex].shouldRemove) {
                                        window::manager::setFocusedHandleByIndex(prevIndex);
                                        LOG_INFO() << "Switched focus to window: " << handles[prevIndex].name << std::endl;
                                    }
                                    break;
                                }
                            }
                        } else {
                            // Find last handle that is not marked for removal
                            for (int i = static_cast<int>(handles.size()) - 1; i >= 0; --i) {
                                if (!handles[i].shouldRemove) {
                                    window::manager::setFocusedHandleByIndex(i);
                                    break;
                                }
                            }
                        }
                    }
                });
                break;
            }
            case ActionType::MOVE_WINDOW_FULLSCREEN: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current && !current->shouldRemove) {
                    current->preset = window::position::FULLSCREEN;
                    LOG_INFO() << "Moved window to fullscreen: " << current->name << std::endl;
                }
                break;
            }
            case ActionType::MOVE_WINDOW_LEFT: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current && !current->shouldRemove) {
                    current->preset = window::position::LEFT;
                    LOG_INFO() << "Moved window to left half: " << current->name << std::endl;
                }
                break;
            }
            case ActionType::MOVE_WINDOW_RIGHT: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current && !current->shouldRemove) {
                    current->preset = window::position::RIGHT;
                    LOG_INFO() << "Moved window to right half: " << current->name << std::endl;
                }
                break;
            }
            case ActionType::MOVE_WINDOW_TOP: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current && !current->shouldRemove) {
                    current->preset = window::position::TOP;
                    LOG_INFO() << "Moved window to top half: " << current->name << std::endl;
                }
                break;
            }
            case ActionType::MOVE_WINDOW_BOTTOM: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current && !current->shouldRemove) {
                    current->preset = window::position::BOTTOM;
                    LOG_INFO() << "Moved window to bottom half: " << current->name << std::endl;
                }
                break;
            }
            case ActionType::CLOSE_FOCUSED_WINDOW: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current && !current->shouldRemove) {
                    LOG_INFO() << "Closing window: " << current->name << std::endl;
                    current->close();
                }
                break;
            }
            case ActionType::TOGGLE_ZOOM: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current && !current->shouldRemove) {
                    current->zoom = (current->zoom == 1.0f) ? 1.5f : 1.0f;
                    LOG_INFO() << "Toggled zoom for window: " << current->name << " (zoom: " << current->zoom << ")" << std::endl;
                }
                break;
            }
            case ActionType::INCREASE_ZOOM: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current) {
                    current->zoom = std::min(current->zoom + 0.1f, 3.0f);
                    LOG_INFO() << "Increased zoom for window: " << current->name << " (zoom: " << current->zoom << ")" << std::endl;
                }
                break;
            }
            case ActionType::DECREASE_ZOOM: {
                window::handle* current = window::manager::getFocusedHandle();
                if (current) {
                    current->zoom = std::max(current->zoom - 0.1f, 0.5f);
                    LOG_INFO() << "Decreased zoom for window: " << current->name << " (zoom: " << current->zoom << ")" << std::endl;
                }
                break;
            }
            case ActionType::CUSTOM: {
                if (callback) {
                    callback();
                } else {
                    LOG_ERROR() << "Custom action without callback: " << customCommand << std::endl;
                }
                break;
            }
            default:
                LOG_ERROR() << "Unknown action type executed" << std::endl;
                break;
        }
    }

    std::string Action::toString() const {
        switch (type) {
            case ActionType::SWITCH_FOCUS_NEXT: return "switch_focus_next";
            case ActionType::SWITCH_FOCUS_PREVIOUS: return "switch_focus_previous";
            case ActionType::MOVE_WINDOW_FULLSCREEN: return "move_window_fullscreen";
            case ActionType::MOVE_WINDOW_LEFT: return "move_window_left";
            case ActionType::MOVE_WINDOW_RIGHT: return "move_window_right";
            case ActionType::MOVE_WINDOW_TOP: return "move_window_top";
            case ActionType::MOVE_WINDOW_BOTTOM: return "move_window_bottom";
            case ActionType::CLOSE_FOCUSED_WINDOW: return "close_focused_window";
            case ActionType::TOGGLE_ZOOM: return "toggle_zoom";
            case ActionType::INCREASE_ZOOM: return "increase_zoom";
            case ActionType::DECREASE_ZOOM: return "decrease_zoom";
            case ActionType::CUSTOM: return "custom:" + customCommand;
            default: return "unknown";
        }
    }

    Action Action::fromString(const std::string& str) {
        if (str == "switch_focus_next") return Action(ActionType::SWITCH_FOCUS_NEXT);
        if (str == "switch_focus_previous") return Action(ActionType::SWITCH_FOCUS_PREVIOUS);
        if (str == "move_window_fullscreen") return Action(ActionType::MOVE_WINDOW_FULLSCREEN);
        if (str == "move_window_left") return Action(ActionType::MOVE_WINDOW_LEFT);
        if (str == "move_window_right") return Action(ActionType::MOVE_WINDOW_RIGHT);
        if (str == "move_window_top") return Action(ActionType::MOVE_WINDOW_TOP);
        if (str == "move_window_bottom") return Action(ActionType::MOVE_WINDOW_BOTTOM);
        if (str == "close_focused_window") return Action(ActionType::CLOSE_FOCUSED_WINDOW);
        if (str == "toggle_zoom") return Action(ActionType::TOGGLE_ZOOM);
        if (str == "increase_zoom") return Action(ActionType::INCREASE_ZOOM);
        if (str == "decrease_zoom") return Action(ActionType::DECREASE_ZOOM);
        if (str.substr(0, 7) == "custom:") return Action(str.substr(7));
        return Action(ActionType::UNKNOWN);
    }

    // KeyBindSettings implementation
    void KeyBindSettings::loadDefaults() {
        windowManagement.clear();
        focusManagement.clear();
        customBinds.clear();
        
        // Focus management defaults
        focusManagement.emplace_back(
            KeyCombination(KEY_TAB, false, true, false, false), // Alt+Tab
            Action(ActionType::SWITCH_FOCUS_NEXT),
            "Switch to next window"
        );
        
        focusManagement.emplace_back(
            KeyCombination(KEY_TAB, false, true, true, false), // Alt+Shift+Tab
            Action(ActionType::SWITCH_FOCUS_PREVIOUS),
            "Switch to previous window"
        );
        
        // Window management defaults
        windowManagement.emplace_back(
            KeyCombination(KEY_UP, false, false, false, true), // Super+Up
            Action(ActionType::MOVE_WINDOW_TOP),
            "Move window to top half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_DOWN, false, false, false, true), // Super+Down
            Action(ActionType::MOVE_WINDOW_BOTTOM),
            "Move window to bottom half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_LEFT, false, false, false, true), // Super+Left
            Action(ActionType::MOVE_WINDOW_LEFT),
            "Move window to left half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_RIGHT, false, false, false, true), // Super+Right
            Action(ActionType::MOVE_WINDOW_RIGHT),
            "Move window to right half"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_F, false, false, false, true), // Super+F
            Action(ActionType::MOVE_WINDOW_FULLSCREEN),
            "Move window to fullscreen"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_Q, false, false, false, true), // Super+Q
            Action(ActionType::CLOSE_FOCUSED_WINDOW),
            "Close focused window"
        );
        
        // Zoom controls
        windowManagement.emplace_back(
            KeyCombination(KEY_EQUAL, true, false, false, false), // Ctrl+=
            Action(ActionType::INCREASE_ZOOM),
            "Increase zoom"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_MINUS, true, false, false, false), // Ctrl+-
            Action(ActionType::DECREASE_ZOOM),
            "Decrease zoom"
        );
        
        windowManagement.emplace_back(
            KeyCombination(KEY_0, true, false, false, false), // Ctrl+0
            Action(ActionType::TOGGLE_ZOOM),
            "Reset/toggle zoom"
        );
    }

    // Configuration implementation
    void Configuration::loadDefaults() {
        keybinds.loadDefaults();
        
        display.autoDistributeWindows = true;
        display.displayAssignmentStrategy = "round_robin";
        display.primaryDisplayId = 0;
        
        input.enableGlobalKeybinds = true;
        input.passUnhandledInput = true;
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
                    
                    if (key.keyCode != 0 && action.type != ActionType::UNKNOWN) {
                        addKeybind(key, action, "Loaded from config");
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
            if (i < config.keybinds.focusManagement.size() - 1) file << ",";
            file << "  // " << kb.description << "\n";
        }
        
        file << "    },\n";
        file << "    \"windowManagement\": {\n";
        
        // Write window management keybinds
        for (size_t i = 0; i < config.keybinds.windowManagement.size(); ++i) {
            const auto& kb = config.keybinds.windowManagement[i];
            file << "      \"" << kb.key.toString() << "\": \"" << kb.action.toString() << "\"";
            if (i < config.keybinds.windowManagement.size() - 1) file << ",";
            file << "  // " << kb.description << "\n";
        }
        
        file << "    },\n";
        file << "    \"customBinds\": {\n";
        
        // Write custom keybinds
        for (size_t i = 0; i < config.keybinds.customBinds.size(); ++i) {
            const auto& kb = config.keybinds.customBinds[i];
            file << "      \"" << kb.key.toString() << "\": \"" << kb.action.toString() << "\"";
            if (i < config.keybinds.customBinds.size() - 1) file << ",";
            file << "  // " << kb.description << "\n";
        }
        
        file << "    }\n";
        file << "  },\n";
        file << "  \"display\": {\n";
        file << "    \"autoDistributeWindows\": " << (config.display.autoDistributeWindows ? "true" : "false") << ",\n";
        file << "    \"displayAssignmentStrategy\": \"" << config.display.displayAssignmentStrategy << "\",\n";
        file << "    \"primaryDisplayId\": " << config.display.primaryDisplayId << "\n";
        file << "  },\n";
        file << "  \"input\": {\n";
        file << "    \"enableGlobalKeybinds\": " << (config.input.enableGlobalKeybinds ? "true" : "false") << ",\n";
        file << "    \"passUnhandledInput\": " << (config.input.passUnhandledInput ? "true" : "false") << ",\n";
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
        file << "    \"passUnhandledInput\": " << (defaultConfig.input.passUnhandledInput ? "true" : "false") << ",\n";
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
        switch (action.type) {
            case ActionType::SWITCH_FOCUS_NEXT:
            case ActionType::SWITCH_FOCUS_PREVIOUS:
                config.keybinds.focusManagement.push_back(newBind);
                break;
            case ActionType::MOVE_WINDOW_FULLSCREEN:
            case ActionType::MOVE_WINDOW_LEFT:
            case ActionType::MOVE_WINDOW_RIGHT:
            case ActionType::MOVE_WINDOW_TOP:
            case ActionType::MOVE_WINDOW_BOTTOM:
            case ActionType::CLOSE_FOCUSED_WINDOW:
            case ActionType::TOGGLE_ZOOM:
            case ActionType::INCREASE_ZOOM:
            case ActionType::DECREASE_ZOOM:
                config.keybinds.windowManagement.push_back(newBind);
                break;
            default:
                config.keybinds.customBinds.push_back(newBind);
                break;
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
        return Action(ActionType::UNKNOWN);
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
    }

    // Utility functions implementation
    namespace utils {
        int stringToKeyCode(const std::string& keyName) {
            std::string lowerKey = keyName;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
            
            auto it = keyNameToCode.find(lowerKey);
            return (it != keyNameToCode.end()) ? it->second : 0;
        }
        
        std::string keyCodeToString(int keyCode) {
            initializeKeyMappings();
            auto it = codeToKeyName.find(keyCode);
            return (it != codeToKeyName.end()) ? it->second : ("key" + std::to_string(keyCode));
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
