#ifndef _WINDOW_H_
#define _WINDOW_H_

#include "types.h"
#include "tcp.h"
#include "guard.h"
#include "font.h"

#include <vector>
#include <map>


namespace window {
    // In GGDirect, we dont give free positions to each window, instead we put them into interchangable predefined presets, defined as:
    enum class position {
        FULLSCREEN,
        LEFT, RIGHT,    // anchored half of the screen either to the right or left
        TOP, BOTTOM,    // anchored half of the screen either at the top or at the bottom
    };

    // Forward declaration for the handle class
    class handle;

    extern types::rectangle positionToCoordinates(position pos, const handle& windowHandle);
    extern types::rectangle positionToCoordinates(position pos, uint32_t displayId = 0);  // Legacy function for backward compatibility
    
    // New functions to distinguish between pixel and cell coordinates
    extern types::rectangle positionToPixelCoordinates(position pos, const handle& windowHandle);
    extern types::rectangle positionToPixelCoordinates(position pos, uint32_t displayId = 0);
    extern types::rectangle positionToCellCoordinates(position pos, const handle& windowHandle);
    extern types::rectangle positionToCellCoordinates(position pos, uint32_t displayId = 0);
    
    // Utility functions for display management
    extern uint32_t getPrimaryDisplayId();
    extern bool isValidDisplayId(uint32_t displayId);
    extern types::iVector2 getDisplayResolution(uint32_t displayId);

    namespace stain {
        enum class type {
            clear           = 0 << 0,
            resize          = 1 << 0,
            closed          = 1 << 1,
        };

        constexpr bool has(stain::type a, stain::type b) {
            return (static_cast<int>(a) & static_cast<int>(b)) != 0;
        }

        constexpr bool is(stain::type a, stain::type b) {
            return (static_cast<int>(a) & static_cast<int>(b)) == static_cast<int>(b);
        }
    }

    /*
    As each GGUI gets its input from the terminal hosting it. We currently need to first instate a new terminal and then host GGUI on top of it, for GGUI to get input from it.
    We can later on, give each handle Focused mode, and perpetrate the inputs from here and give them through sockets to each individual GGUI instance. 
    */
    class handle {
    public:
        // I'm not sure where we can get the position of hosting terminal for the current GGUI handle, this will be more important once we start to give inputs from here and ditch terminal hosting.
        position preset;   // Z represents draw order, higher = later draw.
        position previousPreset;  // Previous position preset for resize stain system
        constexpr static unsigned int maxAllowedErrorCount = 100;
        unsigned int errorCount;

        stain::type dirty;

        float zoom;          // 1.0f is the default 100% scale.

        // This is the final and upholding socket that is formed after the handshake and reroute.
        tcp::connection connection;

        std::string name;   // This is given from the GGUI client, to remember on next startup the location and size of window for more continuous experience.

        std::vector<types::Cell>* cellBuffer;
        
        // Display management - track which display this handle is positioned on
        uint32_t displayId;  // ID of the display this handle is associated with

        std::unique_ptr<font::font> customFont = nullptr;

        handle(tcp::connection&& conn) : preset(position::FULLSCREEN), previousPreset(position::FULLSCREEN), errorCount(0), dirty(stain::type::clear), zoom(1.0f), connection(std::move(conn)), name(""), cellBuffer(new std::vector<types::Cell>()), displayId(0) {}

        ~handle() {
            delete cellBuffer;
        }

        handle(const window::handle&) = default;
        handle& operator=(const window::handle&) = default;
        handle(window::handle&&) noexcept = default;
        handle& operator=(window::handle&&) noexcept = default;

        void close() {
            connection.close();
        }

        // polls from GGUI dimensions and cell buffer
        void poll();

        font::font* getFont() const;

        // for staining
        void set(stain::type t, bool val);
        
        // Resize stain system - get area that needs to be cleared
        types::rectangle getResizeClearArea() const;
        
        // Display management methods
        void setDisplayId(uint32_t newDisplayId) { displayId = newDisplayId; }
        uint32_t getDisplayId() const { return displayId; }
        types::rectangle getCoordinates() const { return positionToCellCoordinates(preset, *this); }
        types::rectangle getPixelCoordinates() const { return positionToPixelCoordinates(preset, *this); }
        types::rectangle getCellCoordinates() const { return positionToCellCoordinates(preset, *this); }
        
    private:
        // Helper method to flush TCP receive buffer to prevent misalignment
        void flushTcpReceiveBuffer();
    };

    // Manages all of the handles and their handshake protocol steps.
    namespace manager {
        extern atomic::guard<std::vector<handle>> handles;
        
        // First we will make an listener with port number zero, to invoke the kernel giving us an empty port number to use.
        extern atomic::guard<tcp::listener> listener;
        
        // Shutdown control
        extern std::atomic<bool> shouldShutdown;

        extern void init();
        extern void close();
        
        // Focus management for input system
        extern void setFocusedHandle(handle* focusedHandle);
        extern handle* getFocusedHandle();
        extern void setFocusedHandleByIndex(size_t index);
        extern void setFocusOnNextAvailableHandle();
        extern size_t getActiveHandleCount();
        
        // Cleanup management
        extern void cleanupDeadHandles();
        
        // Display management functions
        extern void distributeHandlesAcrossDisplays();
        extern void moveHandleToDisplay(handle* windowHandle, uint32_t displayId);
        extern std::vector<uint32_t> getAvailableDisplayIds();
        
        // Display assignment strategies
        enum class DisplayAssignmentStrategy {
            ROUND_ROBIN,    // Distribute handles across displays in round-robin fashion
            PRIMARY_ONLY,   // All handles go to primary display
            FILL_THEN_NEXT  // Fill one display before moving to next
        };
        extern void assignDisplaysToHandles(DisplayAssignmentStrategy strategy = DisplayAssignmentStrategy::ROUND_ROBIN);
        
        // Display monitoring and updates
        extern void updateHandleDisplays();
        extern std::map<uint32_t, size_t> getHandleDistribution();
        extern void printHandleDisplayMapping();
    };
}

#endif