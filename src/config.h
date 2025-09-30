#ifndef _CONFIG_H_
#define _CONFIG_H_

#include "types.h"
#include "window.h"
#include "guard.h"

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <unordered_map>

namespace config {

    /**
     * @brief Represents a key combination (key + modifiers)
     */
    struct KeyCombination {
        int keyCode;                    // Linux input keycode (e.g., KEY_TAB)
        bool ctrl;
        bool alt;
        bool shift;
        bool super;                     // Windows/Super/Meta key
        
        KeyCombination() : keyCode(0), ctrl(false), alt(false), shift(false), super(false) {}
        
        KeyCombination(int key, bool ctrl_mod = false, bool alt_mod = false, bool shift_mod = false, bool super_mod = false) : keyCode(key), ctrl(ctrl_mod), alt(alt_mod), shift(shift_mod), super(super_mod) {}
        
        bool operator==(const KeyCombination& other) const {
            return keyCode == other.keyCode && ctrl == other.ctrl && alt == other.alt && shift == other.shift && super == other.super;
        }
        
        bool operator<(const KeyCombination& other) const {
            if (keyCode != other.keyCode) return keyCode < other.keyCode;
            if (ctrl != other.ctrl) return ctrl < other.ctrl;
            if (alt != other.alt) return alt < other.alt;
            if (shift != other.shift) return shift < other.shift;
            return super < other.super;
        }
        
        std::string toString() const;
        static KeyCombination fromString(const std::string& str);
    };

    /**
     * @brief Custom hash function for KeyCombination to use in unordered_map
     */
    struct KeyCombinationHash {
        std::size_t operator()(const KeyCombination& k) const {
            return std::hash<int>()(k.keyCode) ^ 
                   (std::hash<bool>()(k.ctrl) << 1) ^
                   (std::hash<bool>()(k.alt) << 2) ^
                   (std::hash<bool>()(k.shift) << 3) ^
                   (std::hash<bool>()(k.super) << 4);
        }
    };

    /**
     * @brief Types of actions that can be bound to keys
     */
    // Bitmask-based action flags to allow combining behaviors (e.g., MOVE + DIR_UP + DIR_LEFT)
    enum class ActionBits : uint32_t {
        NONE                = 0,

        // Single, non-combinable actions (category bits)
        SWITCH_FOCUS_NEXT   = 1u << 0,
        SWITCH_FOCUS_PREV   = 1u << 1,
        CLOSE_WINDOW        = 1u << 2,
        TOGGLE_ZOOM         = 1u << 3,
        ZOOM_IN             = 1u << 4,
        ZOOM_OUT            = 1u << 5,

        // Movement category
        MOVE                = 1u << 8,
        FULLSCREEN          = 1u << 9,   // Can be used alone or with MOVE

        // Direction sub-flags (can be combined with MOVE)
        DIR_UP              = 1u << 16,
        DIR_DOWN            = 1u << 17,
        DIR_LEFT            = 1u << 18,
        DIR_RIGHT           = 1u << 19,

        CUSTOM              = 1u << 31
    };

    inline constexpr ActionBits operator|(ActionBits a, ActionBits b) {
        return static_cast<ActionBits>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline constexpr ActionBits operator&(ActionBits a, ActionBits b) {
        return static_cast<ActionBits>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    inline constexpr ActionBits& operator|=(ActionBits& a, ActionBits b) {
        a = a | b; return a;
    }
    inline constexpr bool hasFlag(ActionBits a, ActionBits b) {
        return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0u;
    }

    /**
     * @brief Represents an action that can be executed
     */
    struct Action {
        ActionBits flags;
        std::string customCommand;      // For custom actions
        std::function<void()> callback; // Runtime callback function

        Action() : flags(ActionBits::NONE) {}
        explicit Action(ActionBits f) : flags(f) {}
        explicit Action(const std::string& command) : flags(ActionBits::CUSTOM), customCommand(command) {}

        void execute() const;
        std::string toString() const;
        static Action fromString(const std::string& str);
    };

    /**
     * @brief Represents a keybind mapping
     */
    struct KeyBind {
        KeyCombination key;
        Action action;
        std::string description;
        bool enabled;
        
        KeyBind() : enabled(true) {}
        KeyBind(const KeyCombination& keyComb, const Action& act, const std::string& desc = "") : key(keyComb), action(act), description(desc), enabled(true) {}
    };

    /**
     * @brief Configuration categories
     */
    struct KeyBindSettings {
        std::vector<KeyBind> windowManagement;
        std::vector<KeyBind> focusManagement;
        std::vector<KeyBind> customBinds;
        
        // Default keybinds
        void loadDefaults();
    };

    struct DisplaySettings {
        bool autoDistributeWindows;
        std::string displayAssignmentStrategy; // "round_robin", "primary_only", "fill_then_next"
        uint32_t primaryDisplayId;
        
        // Background and wallpaper settings
        std::string backgroundColor;      // Hex color string (e.g., "#000000" for black)
        std::string wallpaperPath;       // Path to wallpaper image file (bitmap format)
        uint32_t backgroundColorRGB;     // Cached RGB value for fast access
        
        void loadDefaults();
    };

    struct InputSettings {
        bool enableGlobalKeybinds;
        bool passUnhandledInput;        // Pass unhandled input to focused window
        int inputPollRate;              // Input polling rate in Hz
    };

    /**
     * @brief Main configuration structure
     */
    struct Configuration {
        KeyBindSettings keybinds;
        DisplaySettings display;
        InputSettings input;
        
        // Configuration metadata
        std::string configVersion;
        std::string lastModified;
        
        void loadDefaults();
    };

    /**
     * @brief Configuration manager class
     */
    class ConfigurationManager {
    private:
        Configuration config;
        std::string configFilePath;
        std::unordered_map<KeyCombination, Action, KeyCombinationHash> activeKeybinds;
        
        // File operations
        bool loadFromFile(const std::string& filePath);
        bool saveToFile(const std::string& filePath) const;
        bool createDefaultConfig(const std::string& filePath);
        
        // JSON parsing helpers
        KeyCombination parseKeyCombination(const std::string& keyStr) const;
        Action parseAction(const std::string& actionStr) const;
        
        void rebuildKeybindMap();
        
    public:
        ConfigurationManager();
        ~ConfigurationManager() = default;
        
        // Configuration file management
        bool load(const std::string& configPath = "");
        bool save(const std::string& configPath = "") const;
        bool reload();
        
        // Keybind management
        bool addKeybind(const KeyCombination& key, const Action& action, const std::string& description = "");
        bool removeKeybind(const KeyCombination& key);
        bool updateKeybind(const KeyCombination& oldKey, const KeyCombination& newKey, const Action& action);
        bool isKeybindActive(const KeyCombination& key) const;
        Action getAction(const KeyCombination& key) const;
        std::vector<KeyBind> getAllKeybinds() const;
        // Utility for checking flags quickly
        static inline bool actionHas(const Action& a, ActionBits f) { return hasFlag(a.flags, f); }
        
        // Configuration access
        Configuration& getConfiguration() { return config; }
        const Configuration& getConfiguration() const { return config; }
        
        // Input processing
        bool processKeyInput(const KeyCombination& key);
        
        // Validation
        bool validateConfiguration() const;
        std::vector<std::string> getValidationErrors() const;
        
        // Default paths
        static std::string getDefaultConfigPath();
        static std::string getLocalConfigPath();
        static std::string getUserConfigPath();
        static std::string getSystemConfigPath();
    };

    /**
     * @brief Global configuration manager instance
     */
    namespace manager {
        extern atomic::guard<ConfigurationManager> configManager;
        
        void init();
        void cleanup();
        
        // Quick access functions
        bool processKeyInput(const KeyCombination& key);
        bool loadConfiguration(const std::string& configPath = "");
        bool saveConfiguration(const std::string& configPath = "");
        void modifyConfig(std::function<void(Configuration&)> modifier);
        Configuration getConfigCopy();
        
        // Keybind shortcuts
        bool addKeybind(const KeyCombination& key, const Action& action, const std::string& description = "");
        bool removeKeybind(const KeyCombination& key);
        std::vector<KeyBind> getAllKeybinds();
        // New lightweight lookups for combo resolution
        bool isKeybindActive(const KeyCombination& key);
        Action getAction(const KeyCombination& key);
        
        // Configuration accessors
        uint32_t getBackgroundColor();
        std::string getWallpaperPath();
        bool loadWallpaper(const std::string& wallpaperPath);
        bool getWallpaperPixel(int x, int y, uint32_t& pixel);
        
        // Optimized wallpaper region access for efficient copying
        bool getWallpaperRegion(int startX, int startY, int regionWidth, int regionHeight, uint32_t* destBuffer, int destWidth);
        bool getWallpaperData(const uint32_t*& data, int& width, int& height);
    }

    /**
     * @brief Utility functions for key code conversion
     */
    namespace utils {
        // Convert between different key representations
        int stringToKeyCode(const std::string& keyName);
        std::string keyCodeToString(int keyCode);
        
        // Modifier key utilities
        std::string modifiersToString(bool ctrl, bool alt, bool shift, bool super);
        void parseModifiers(const std::string& modStr, bool& ctrl, bool& alt, bool& shift, bool& super);
        
        // Configuration file utilities
        bool fileExists(const std::string& path);
        bool createDirectory(const std::string& path);
        std::string getHomeDirectory();
        std::string getConfigDirectory();
        std::string getExecutableDirectory();
        std::string getCurrentWorkingDirectory();
        
        // JSON utilities
        std::string escapeJsonString(const std::string& str);
        std::string unescapeJsonString(const std::string& str);
    }
}

#endif
