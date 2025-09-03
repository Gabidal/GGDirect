#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include "types.h"
#include <vector>
#include <memory>
#include <string>
#include <map>
#include <functional>
#include <cstdint>

// Forward declarations for DRM types
extern "C" {
    struct _drmModeRes;
    struct _drmModeConnector;
    struct _drmModeEncoder;
    struct _drmModeCrtc;
    struct _drmModePlane;
    struct _drmModePlaneRes;
    struct _drmModeProperty;
    struct _drmModeFB;
    typedef struct _drmModeRes drmModeRes;
    typedef struct _drmModeConnector drmModeConnector;
    typedef struct _drmModeEncoder drmModeEncoder;
    typedef struct _drmModeCrtc drmModeCrtc;
    typedef struct _drmModePlane drmModePlane;
    typedef struct _drmModePlaneRes drmModePlaneRes;
    typedef struct _drmModeProperty drmModeProperty;
    typedef struct _drmModeFB drmModeFB;
}

namespace display {

    // Forward declarations
    class device;
    class connector;
    class encoder;
    class crtc;
    class plane;
    class frameBuffer;
    class property;
    class mode;

    /**
     * @brief Represents a display mode (resolution, refresh rate, etc.)
     */
    class mode {
    public:
        struct ModeInfo {
            uint32_t width;
            uint32_t height;
            uint32_t refreshRate;
            uint32_t flags;
            std::string name;
            bool preferred;
        };

        mode(const ModeInfo& info);
        mode(const void* drmMode);
        ~mode() = default;

        // Copy and move semantics
        mode(const mode&) = default;
        mode& operator=(const mode&) = default;
        mode(mode&&) = default;
        mode& operator=(mode&&) = default;

        // Getters
        uint32_t getWidth() const { return info.width; }
        uint32_t getHeight() const { return info.height; }
        uint32_t getRefreshRate() const { return info.refreshRate; }
        const std::string& getName() const { return info.name; }
        bool isPreferred() const { return info.preferred; }
        types::iVector2 getResolution() const { return {static_cast<int>(info.width), static_cast<int>(info.height)}; }

        // Comparison operators
        bool operator==(const mode& other) const;
        bool operator!=(const mode& other) const { return !(*this == other); }

    private:
        ModeInfo info;
    };

    /**
     * @brief Represents a DRM property with its value and metadata
     */
    class property {
    public:
        enum class Type {
            RANGE,
            ENUM,
            BITMASK,
            BLOB,
            OBJECT
        };

        property(uint32_t id, const std::string& name, Type type);
        ~property() = default;

        // Getters
        uint32_t getId() const { return id; }
        const std::string& getName() const { return name; }
        Type getType() const { return type; }
        uint64_t getValue() const { return value; }

        // Setters
        void setValue(uint64_t Value) { this->value = Value; }

        // Type-specific methods
        bool isEnum() const { return type == Type::ENUM; }
        bool isRange() const { return type == Type::RANGE; }
        bool isBitmask() const { return type == Type::BITMASK; }

    private:
        uint32_t id;
        std::string name;
        Type type;
        uint64_t value;
        std::map<std::string, uint64_t> enumValues;
    };

    /**
     * @brief Represents a DRM framebuffer
     */
    class frameBuffer {
    public:
        struct FramebufferInfo {
            uint32_t width;
            uint32_t height;
            uint32_t pitch;
            uint32_t bpp;
            uint32_t depth;
            uint32_t format;
            size_t size;
        };

        frameBuffer(const FramebufferInfo& info);
        ~frameBuffer();

        // Disable copy, enable move
        frameBuffer(const frameBuffer&) = delete;
        frameBuffer& operator=(const frameBuffer&) = delete;
        frameBuffer(frameBuffer&&) noexcept;
        frameBuffer& operator=(frameBuffer&&) noexcept;

        // Getters
        uint32_t getId() const { return framebufferId; }
        uint32_t getWidth() const { return info.width; }
        uint32_t getHeight() const { return info.height; }
        uint32_t getPitch() const { return info.pitch; }
        uint32_t getFormat() const { return info.format; }
        void* getBuffer() const { return buffer; }
        size_t getSize() const { return info.size; }
        types::iVector2 getRenderableArea() const;      // <-- Use this one to get indexable buffer dimensions!

        // Buffer operations
        bool map();
        void unmap();
        void clear(uint32_t color = 0);
        void fillRect(const types::iVector2& pos, const types::iVector2& size, uint32_t color);

    private:
        uint32_t framebufferId;
        FramebufferInfo info;
        void* buffer;
        bool mapped;
        int dmaBufFd;
    };

    /**
     * @brief Represents a DRM plane (overlay, primary, cursor)
     */
    class plane {
    public:
        enum class Type {
            OVERLAY,
            PRIMARY,
            CURSOR
        };

        plane(uint32_t id, Type type, uint32_t crtcId);
        ~plane() = default;

        // Getters
        uint32_t getId() const { return id; }
        Type getType() const { return type; }
        uint32_t getCrtcId() const { return crtcId; }
        const std::vector<uint32_t>& getSupportedFormats() const { return supportedFormats; }

        // Plane operations
        bool setFramebuffer(std::shared_ptr<frameBuffer> fb);
        bool setPosition(const types::iVector2& pos);
        bool setSize(const types::iVector2& size);
        bool setProperty(const std::string& name, uint64_t value);
        bool commit();

        // Property management
        void addProperty(std::shared_ptr<property> prop);
        std::shared_ptr<property> getProperty(const std::string& name) const;
        void addSupportedFormat(uint32_t format) { supportedFormats.push_back(format); }  // Allow device class to add supported formats

    private:
        uint32_t id;
        Type type;
        uint32_t crtcId;
        std::vector<uint32_t> supportedFormats;
        std::map<std::string, std::shared_ptr<property>> properties;
        std::shared_ptr<frameBuffer> currentFb;
        types::iVector2 position;
        types::iVector2 size;
    };

    /**
     * @brief Represents a DRM CRTC (display controller)
     */
    class crtc {
    public:
        crtc(uint32_t id, uint32_t bufferId);
        ~crtc() = default;

        // Getters
        uint32_t getId() const { return id; }
        uint32_t getBufferId() const { return bufferId; }
        const mode& getCurrentMode() const { return currentMode; }
        const std::vector<std::shared_ptr<plane>>& getPlanes() const { return planes; }

        // Mode setting
        bool setMode(const mode& mode);
        bool setFramebuffer(std::shared_ptr<frameBuffer> fb);
        
        // Plane management
        void addPlane(std::shared_ptr<plane> plane);
        std::shared_ptr<plane> getPrimaryPlane() const;
        std::shared_ptr<plane> getCursorPlane() const;
        std::vector<std::shared_ptr<plane>> getOverlayPlanes() const;

        // Property management
        void addProperty(std::shared_ptr<property> prop);
        std::shared_ptr<property> getProperty(const std::string& name) const;

        // CRTC operations
        bool commit();
        bool pageFlip(std::shared_ptr<frameBuffer> fb, void* user_data = nullptr);

    private:
        uint32_t id;
        uint32_t bufferId;
        mode currentMode;
        std::vector<std::shared_ptr<plane>> planes;
        std::map<std::string, std::shared_ptr<property>> properties;
        std::shared_ptr<frameBuffer> currentFb;
    };

    /**
     * @brief Represents a DRM encoder
     */
    class encoder {
    public:
        enum class Type {
            NONE,
            DAC,
            TMDS,
            LVDS,
            TVDAC,
            VIRTUAL,
            DSI,
            DPMST,
            DPI
        };

        encoder(uint32_t id, Type type, uint32_t crtcId);
        ~encoder() = default;

        // Getters
        uint32_t getId() const { return id; }
        Type getType() const { return type; }
        uint32_t getCrtcId() const { return crtcId; }
        const std::vector<uint32_t>& getPossibleCrtcs() const { return possibleCrtcs; }

        // Encoder operations
        bool setCrtc(uint32_t crtcId);
        std::string getTypeString() const;
        void addPossibleCrtc(uint32_t crtcIdToAdd) { possibleCrtcs.push_back(crtcIdToAdd); }  // Allow device class to add possible CRTCs

    private:
        uint32_t id;
        Type type;
        uint32_t crtcId;
        std::vector<uint32_t> possibleCrtcs;
    };

    /**
     * @brief Represents a display connector (HDMI, DisplayPort, etc.)
     */
    class connector {
    public:
        enum class Type {
            UNKNOWN,
            VGA,
            DVI_I,
            DVI_D,
            DVI_A,
            COMPOSITE,
            SVIDEO,
            LVDS,
            COMPONENT,
            HDMI_A,
            HDMI_B,
            TV,
            EDP,
            VIRTUAL,
            DSI,
            DPI,
            WRITEBACK,
            SPI,
            USB
        };

        enum class Status {
            CONNECTED,
            DISCONNECTED,
            UNKNOWN
        };

        connector(uint32_t id, Type type, uint32_t encoderId);
        ~connector() = default;

        // Getters
        uint32_t getId() const { return id; }
        Type getType() const { return type; }
        Status getStatus() const { return status; }
        uint32_t getEncoderId() const { return encoderId; }
        const std::vector<mode>& getModes() const { return modes; }
        const mode& getPreferredMode();
        const std::string& getName() const { return name; }

        // Connection management
        bool isConnected() const { return status == Status::CONNECTED; }
        bool updateStatus();
        void refreshModes();
        void setStatus(Status newStatus) { status = newStatus; }  // Allow device class to update status

        // Mode operations
        void addMode(const mode& mode);
        std::vector<mode> getAvailableModes() const;
        mode findModeByResolution(uint32_t width, uint32_t height) const;

        // Property management
        void addProperty(std::shared_ptr<property> prop);
        std::shared_ptr<property> getProperty(const std::string& name) const;

        // Encoder management
        bool setEncoder(uint32_t encoderId);
        std::shared_ptr<encoder> getEncoder() const { return Encoder; }

        // Utility methods
        std::string getTypeString() const;
        std::string getStatusString() const;

    private:
        uint32_t id;
        Type type;
        Status status;
        uint32_t encoderId;
        std::vector<mode> modes;
        std::string name;
        std::map<std::string, std::shared_ptr<property>> properties;
        std::shared_ptr<encoder> Encoder;
        int preferredMode = -1;  // points to the modes as an index.
    };

    /**
     * @brief Main DRM device interface - manages the entire display subsystem
     */
    class device {
    public:
        device(const std::string& devicePath);
        ~device();

        // Disable copy, enable move
        device(const device&) = delete;
        device& operator=(const device&) = delete;
        device(device&&) noexcept;
        device& operator=(device&&) noexcept;

        // Initialization
        bool initialize();
        void cleanup();
        bool isInitialized() const { return initialized; }

        // Resource discovery
        bool discoverResources();
        void refreshResources();

        // Connector management
        const std::vector<std::shared_ptr<connector>>& getConnectors() const { return connectors; }
        std::shared_ptr<connector> getConnector(uint32_t id) const;
        std::vector<std::shared_ptr<connector>> getConnectedConnectors() const;

        // CRTC management
        const std::vector<std::shared_ptr<crtc>>& getCrtcs() const { return crtcs; }
        std::shared_ptr<crtc> getCrtc(uint32_t id) const;
        std::shared_ptr<crtc> getFreeCrtc() const;

        // Encoder management
        const std::vector<std::shared_ptr<encoder>>& getEncoders() const { return encoders; }
        std::shared_ptr<encoder> getEncoder(uint32_t id) const;

        // Plane management
        const std::vector<std::shared_ptr<plane>>& getPlanes() const { return planes; }
        std::shared_ptr<plane> getPlane(uint32_t id) const;
        std::vector<std::shared_ptr<plane>> getPlanesByType(plane::Type type) const;

        // Framebuffer management
        std::shared_ptr<frameBuffer> createFramebuffer(const frameBuffer::FramebufferInfo& info);
        bool destroyFramebuffer(std::shared_ptr<frameBuffer> fb);

        // Mode setting operations
        bool setMode(std::shared_ptr<connector> connector, const mode& mode);
        bool setMode(uint32_t connectorId, const mode& mode);

        // Atomic operations
        bool beginAtomicCommit();
        bool addAtomicProperty(uint32_t objectId, const std::string& property, uint64_t value);
        bool commitAtomic(bool testOnly = false);

        // Page flipping
        bool pageFlip(std::shared_ptr<crtc> crtc, std::shared_ptr<frameBuffer> fb, void* userData = nullptr);

        // Event handling
        bool handleEvents(int timeoutMs = 0);
        void setPageFlipHandler(std::function<void(uint32_t, uint32_t, void*)> handler);

        // Utility methods
        int getDeviceFd() const { return deviceFd; }
        const std::string& getDevicePath() const { return devicePath; }
        bool supportsAtomic() const { return atomicSupported; }

    private:
        std::string devicePath;
        int deviceFd;
        bool initialized;
        bool atomicSupported;

        // DRM resources
        std::vector<std::shared_ptr<connector>> connectors;
        std::vector<std::shared_ptr<crtc>> crtcs;
        std::vector<std::shared_ptr<encoder>> encoders;
        std::vector<std::shared_ptr<plane>> planes;
        std::vector<std::shared_ptr<frameBuffer>> framebuffers;

        // Event handling
        std::function<void(uint32_t, uint32_t, void*)> pageFlipHandler;

        // Atomic commit state
        void* atomicReq;

        // Private methods
        bool openDevice();
        void closeDevice();
        bool loadResources();
        bool loadConnectors();
        bool loadCrtcs();
        bool loadEncoders();
        bool loadPlanes();
        std::shared_ptr<property> loadProperty(uint32_t propId);
        void createHeadlessResources();  // For development/testing without real hardware
    };

    /**
     * @brief Display manager - high-level interface for managing multiple displays
     */
    namespace manager {
        // Initialization
        bool initialize(const std::string& devicePath = "");
        void cleanup();

        // Display management
        std::vector<std::shared_ptr<connector>> getAvailableDisplays();
        bool enableDisplay(std::shared_ptr<connector> connector, const mode& mode);
        bool disableDisplay(std::shared_ptr<connector> connector);
        bool isDisplayEnabled(std::shared_ptr<connector> connector);

        // Multi-display operations
        bool setupClonedDisplays(const std::vector<std::shared_ptr<connector>>& connectors, const mode& mode);
        bool setupExtendedDisplays(const std::vector<std::shared_ptr<connector>>& connectors);

        // Rendering context
        std::shared_ptr<frameBuffer> createFramebuffer(uint32_t width, uint32_t height, uint32_t format);
        bool present(std::shared_ptr<connector> connector, std::shared_ptr<frameBuffer> fb);

        // Event handling
        bool processEvents(int timeoutMs = 0);
        void setHotplugHandler(std::function<void(std::shared_ptr<connector>, bool)> handler);

        extern std::shared_ptr<device> Device;
        extern std::map<uint32_t, std::shared_ptr<connector>> activeDisplays;
        extern std::function<void(std::shared_ptr<connector>, bool)> hotplugHandler;

        // Private methods
        bool setupDisplay(std::shared_ptr<connector> connector, const mode& mode);
        void handleHotplugEvent(std::shared_ptr<connector> connector, bool connected);
    };

}

#endif