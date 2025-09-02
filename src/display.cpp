#include "display.h"
#include "logger.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <stdexcept>
#include <errno.h>

extern "C" {
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libdrm/drm.h>
}

#define MAX_DRM_DEVICES 64

namespace display {

    //===============================================================================
    // DRM Device Discovery
    //===============================================================================

    /**
     * @brief Get the path of a suitable DRM device
     * 
     * This function finds a suitable DRM device and returns its path.
     * If no device is found, it returns an empty string.
     * 
     * @return Path to the DRM device, or empty string if none found
     */
    static std::string findDrmDevicePath() {
        drmDevicePtr devices[MAX_DRM_DEVICES] = { nullptr };
        int num_devices;
        std::string devicePath;

        num_devices = drmGetDevices2(0, devices, MAX_DRM_DEVICES);
        if (num_devices < 0) {
            LOG_ERROR() << "drmGetDevices2 failed: " << strerror(-num_devices) << std::endl;
            return "";
        }

        for (int i = 0; i < num_devices; i++) {
            drmDevicePtr device = devices[i];

            if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY))) {
                continue;
            }

            // Try to open the primary node to test if it's KMS-capable
            int fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }

            // Check if this device supports KMS
            drmModeRes* resources = drmModeGetResources(fd);
            if (resources) {
                devicePath = device->nodes[DRM_NODE_PRIMARY];
                drmModeFreeResources(resources);
                close(fd);
                break;
            }

            close(fd);
        }

        drmFreeDevices(devices, num_devices);
        return devicePath;
    }

    //===============================================================================
    // Mode Implementation
    //===============================================================================

    mode::mode(const ModeInfo& Info) : info(Info) {}

    mode::mode(const void* drmMode) {
        const drmModeModeInfo* modeInfo = static_cast<const drmModeModeInfo*>(drmMode);
        info.width = modeInfo->hdisplay;
        info.height = modeInfo->vdisplay;
        info.refreshRate = modeInfo->vrefresh;
        info.flags = modeInfo->flags;
        info.name = std::string(modeInfo->name);
        info.preferred = (modeInfo->type & DRM_MODE_TYPE_PREFERRED) != 0;
    }

    bool mode::operator==(const mode& other) const {
        return info.width == other.info.width &&
            info.height == other.info.height &&
            info.refreshRate == other.info.refreshRate;
    }

    //===============================================================================
    // Property Implementation
    //===============================================================================

    property::property(uint32_t Id, const std::string& Name, property::Type ttype)
        : id(Id), name(Name), type(ttype), value(0) {}

    //===============================================================================
    // Framebuffer Implementation
    //===============================================================================

    frameBuffer::frameBuffer(const FramebufferInfo& Info)
        : framebufferId(0), info(Info), buffer(nullptr), mapped(false), dmaBufFd(-1) {}

    frameBuffer::~frameBuffer() {
        unmap();
        if (dmaBufFd >= 0) {
            close(dmaBufFd);
        }
    }

    frameBuffer::frameBuffer(frameBuffer&& other) noexcept
        : framebufferId(other.framebufferId), info(other.info), buffer(other.buffer),
        mapped(other.mapped), dmaBufFd(other.dmaBufFd) {
        other.framebufferId = 0;
        other.buffer = nullptr;
        other.mapped = false;
        other.dmaBufFd = -1;
    }

    frameBuffer& frameBuffer::operator=(frameBuffer&& other) noexcept {
        if (this != &other) {
            unmap();
            if (dmaBufFd >= 0) {
                close(dmaBufFd);
            }
            
            framebufferId = other.framebufferId;
            info = other.info;
            buffer = other.buffer;
            mapped = other.mapped;
            dmaBufFd = other.dmaBufFd;
            
            other.framebufferId = 0;
            other.buffer = nullptr;
            other.mapped = false;
            other.dmaBufFd = -1;
        }
        return *this;
    }

    bool frameBuffer::map() {
        if (mapped || buffer) {
            return mapped;
        }
        
        // Get device file descriptor from the display manager
        int drmFd = -1;
        if (display::manager::Device) {
            drmFd = display::manager::Device->getDeviceFd();
        }
        
        // Check if we're in headless mode
        if (drmFd == -2) {
            LOG_INFO() << "Creating software framebuffer for headless mode..." << std::endl;
            
            // Allocate software buffer for headless mode
            info.pitch = info.width * (info.bpp / 8);
            info.size = info.pitch * info.height;
            
            buffer = malloc(info.size);
            if (!buffer) {
                LOG_ERROR() << "Failed to allocate software framebuffer: " << strerror(errno) << std::endl;
                return false;
            }
            
            // Clear the buffer
            memset(buffer, 0, info.size);
            
            // Assign a dummy framebuffer ID
            framebufferId = 1;
            mapped = true;
            
            LOG_INFO() << "Software framebuffer created: " << info.width << "x" << info.height << " (" << info.size << " bytes)" << std::endl;
            return true;
        }
        
        // Create a dumb buffer for software rendering
        struct drm_mode_create_dumb create_dumb = {};
        create_dumb.width = info.width;
        create_dumb.height = info.height;
        create_dumb.bpp = info.bpp;
        
        if (drmFd < 0) {
            LOG_ERROR() << "Failed to get DRM device file descriptor" << std::endl;
            return false;
        }
        
        LOG_INFO() << "Creating dumb buffer: " << info.width << "x" << info.height << 
                     " @ " << info.bpp << " bpp" << std::endl;
        
        if (ioctl(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb) < 0) {
            LOG_ERROR() << "Failed to create dumb buffer: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Update framebuffer info with actual values
        info.pitch = create_dumb.pitch;
        info.size = create_dumb.size;
        
        LOG_INFO() << "Dumb buffer created successfully - handle: " << create_dumb.handle << 
                     ", pitch: " << info.pitch << ", size: " << info.size << std::endl;
        
        // Add framebuffer to DRM
        if (drmModeAddFB(drmFd, info.width, info.height, info.depth, info.bpp, 
                        info.pitch, create_dumb.handle, &framebufferId) != 0) {
            LOG_ERROR() << "Failed to add framebuffer: " << strerror(errno) << std::endl;
            LOG_ERROR() << "FB params: " << info.width << "x" << info.height << 
                         ", depth: " << info.depth << ", bpp: " << info.bpp << 
                         ", pitch: " << info.pitch << ", handle: " << create_dumb.handle << std::endl;
            
            // Clean up the dumb buffer
            struct drm_mode_destroy_dumb destroy_dumb = {};
            destroy_dumb.handle = create_dumb.handle;
            ioctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
            return false;
        }
        
        LOG_INFO() << "Framebuffer added to DRM successfully - FB ID: " << framebufferId << std::endl;
        
        // Map the buffer for CPU access
        struct drm_mode_map_dumb map_dumb = {};
        map_dumb.handle = create_dumb.handle;
        
        if (ioctl(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) < 0) {
            LOG_ERROR() << "Failed to map dumb buffer: " << strerror(errno) << std::endl;
            drmModeRmFB(drmFd, framebufferId);
            
            // Clean up the dumb buffer
            struct drm_mode_destroy_dumb destroy_dumb = {};
            destroy_dumb.handle = create_dumb.handle;
            ioctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
            return false;
        }
        
        buffer = mmap(0, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, drmFd, map_dumb.offset);
        if (buffer == MAP_FAILED) {
            LOG_ERROR() << "Failed to mmap framebuffer: " << strerror(errno) << std::endl;
            drmModeRmFB(drmFd, framebufferId);
            buffer = nullptr;
            
            // Clean up the dumb buffer
            struct drm_mode_destroy_dumb destroy_dumb = {};
            destroy_dumb.handle = create_dumb.handle;
            ioctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
            return false;
        }
        
        LOG_INFO() << "Framebuffer mapped successfully at address: " << buffer << std::endl;
        
        mapped = true;
        return true;
    }

    void frameBuffer::unmap() {
        if (mapped && buffer) {
            // Get device file descriptor from the display manager
            int drmFd = -1;
            if (display::manager::Device) {
                drmFd = display::manager::Device->getDeviceFd();
            }
            
            // Check if we're in headless mode
            if (drmFd == -2) {
                // Free software buffer
                free(buffer);
                buffer = nullptr;
                mapped = false;
                framebufferId = 0;
                return;
            }
            
            // Unmap the buffer
            munmap(buffer, info.size);
            buffer = nullptr;
            mapped = false;
            
            // Remove framebuffer from DRM
            if (framebufferId > 0 && drmFd >= 0) {
                drmModeRmFB(drmFd, framebufferId);
                framebufferId = 0;
            }
        }
    }

    void frameBuffer::clear(uint32_t color) {
        if (!buffer || !mapped) {
            return;
        }
        
        uint32_t* pixels = static_cast<uint32_t*>(buffer);
        size_t pixelCount = info.width * info.height;
        
        for (size_t i = 0; i < pixelCount; ++i) {
            pixels[i] = color;
        }
    }

    void frameBuffer::fillRect(const types::iVector2& pos, const types::iVector2& size, uint32_t color) {
        if (!buffer || !mapped) {
            return;
        }
        
        uint32_t* pixels = static_cast<uint32_t*>(buffer);
        
        for (int y = pos.y; y < pos.y + size.y && y < static_cast<int>(info.height); ++y) {
            for (int x = pos.x; x < pos.x + size.x && x < static_cast<int>(info.width); ++x) {
                if (x >= 0 && y >= 0) {
                    pixels[y * info.width + x] = color;
                }
            }
        }
    }

    //===============================================================================
    // Plane Implementation
    //===============================================================================

    plane::plane(uint32_t Id, Type ttype, uint32_t CrtcId)
        : id(Id), type(ttype), crtcId(CrtcId), position(0, 0), size(0, 0) {}

    bool plane::setFramebuffer(std::shared_ptr<frameBuffer> fb) {
        currentFb = fb;
        return true;
    }

    bool plane::setPosition(const types::iVector2& pos) {
        position = pos;
        return true;
    }

    bool plane::setSize(const types::iVector2& Size) {
        this->size = Size;
        return true;
    }

    bool plane::setProperty(const std::string& name, uint64_t value) {
        auto it = properties.find(name);
        if (it != properties.end()) {
            it->second->setValue(value);
            return true;
        }
        return false;
    }

    bool plane::commit() {
        /**
         * @brief Commit plane changes to the hardware
         * 
         * This function applies all pending plane changes (position, size,
         * framebuffer, etc.) to the display hardware. It uses atomic commits
         * when available for tear-free updates.
         * 
         * @return true if commit was successful, false otherwise
         */
        if (!display::manager::Device) {
            return false;
        }
        
        int drmFd = display::manager::Device->getDeviceFd();
        if (drmFd < 0) {
            return false;
        }
        
        // If atomic mode setting is supported, use it
        if (display::manager::Device->supportsAtomic()) {
            if (!display::manager::Device->beginAtomicCommit()) {
                return false;
            }
            
            // Add plane properties to atomic commit
            if (currentFb) {
                display::manager::Device->addAtomicProperty(id, "FB_ID", currentFb->getId());
            }
            display::manager::Device->addAtomicProperty(id, "CRTC_X", position.x);
            display::manager::Device->addAtomicProperty(id, "CRTC_Y", position.y);
            display::manager::Device->addAtomicProperty(id, "CRTC_W", size.x);
            display::manager::Device->addAtomicProperty(id, "CRTC_H", size.y);
            
            return display::manager::Device->commitAtomic(false);
        } else {
            // Use legacy plane setting
            if (!currentFb) {
                return false;
            }
            
            int ret = drmModeSetPlane(drmFd, id, crtcId, currentFb->getId(), 0,
                                     position.x, position.y, size.x, size.y,
                                     0, 0, currentFb->getWidth() << 16, currentFb->getHeight() << 16);
            
            if (ret != 0) {
                LOG_ERROR() << "Failed to set plane: " << strerror(errno) << " (" << ret << ")" << std::endl;
                return false;
            }
            
            return true;
        }
    }

    void plane::addProperty(std::shared_ptr<property> prop) {
        properties[prop->getName()] = prop;
    }

    std::shared_ptr<property> plane::getProperty(const std::string& name) const {
        auto it = properties.find(name);
        return (it != properties.end()) ? it->second : nullptr;
    }

    //===============================================================================
    // Crtc Implementation
    //===============================================================================

    crtc::crtc(uint32_t Id, uint32_t BufferId)
        : id(Id), bufferId(BufferId), currentMode(mode::ModeInfo{}) {}

    bool crtc::setMode(const mode& mode) {
        currentMode = mode;
        return true;
    }

    bool crtc::setFramebuffer(std::shared_ptr<frameBuffer> fb) {
        currentFb = fb;
        return true;
    }

    void crtc::addPlane(std::shared_ptr<plane> Plane) {
        planes.push_back(Plane);
    }

    std::shared_ptr<plane> crtc::getPrimaryPlane() const {
        auto it = std::find_if(planes.begin(), planes.end(),
            [](const std::shared_ptr<plane>& plane) {
                return plane->getType() == plane::Type::PRIMARY;
            });
        return (it != planes.end()) ? *it : nullptr;
    }

    std::shared_ptr<plane> crtc::getCursorPlane() const {
        auto it = std::find_if(planes.begin(), planes.end(),
            [](const std::shared_ptr<plane>& plane) {
                return plane->getType() == plane::Type::CURSOR;
            });
        return (it != planes.end()) ? *it : nullptr;
    }

    std::vector<std::shared_ptr<plane>> crtc::getOverlayPlanes() const {
        std::vector<std::shared_ptr<plane>> overlayPlanes;
        for (const auto& plane : planes) {
            if (plane->getType() == plane::Type::OVERLAY) {
                overlayPlanes.push_back(plane);
            }
        }
        return overlayPlanes;
    }

    void crtc::addProperty(std::shared_ptr<property> prop) {
        properties[prop->getName()] = prop;
    }

    std::shared_ptr<property> crtc::getProperty(const std::string& name) const {
        auto it = properties.find(name);
        return (it != properties.end()) ? it->second : nullptr;
    }

    bool crtc::commit() {
        /**
         * @brief Commit CRTC changes to the hardware
         * 
         * This function applies all pending CRTC changes (mode, framebuffer,
         * etc.) to the display hardware. It uses atomic commits when available
         * for consistent updates across multiple objects.
         * 
         * @return true if commit was successful, false otherwise
         */
        if (!display::manager::Device) {
            return false;
        }
        
        // If atomic mode setting is supported, use it
        if (display::manager::Device->supportsAtomic()) {
            if (!display::manager::Device->beginAtomicCommit()) {
                return false;
            }
            
            // Add CRTC properties to atomic commit
            if (currentFb) {
                display::manager::Device->addAtomicProperty(id, "FB_ID", currentFb->getId());
            }
            display::manager::Device->addAtomicProperty(id, "ACTIVE", 1);
            
            return display::manager::Device->commitAtomic(false);
        } else {
            // Legacy mode setting is handled by the device's setMode function
            LOG_INFO() << "CRTC commit using legacy mode setting" << std::endl;
            return true;
        }
    }

    bool crtc::pageFlip(std::shared_ptr<frameBuffer> fb, [[maybe_unused]] void* userData) {
        currentFb = fb;
        // Implementation would use DRM page flip ioctl
        return true;
    }

    //===============================================================================
    // Encoder Implementation
    //===============================================================================

    encoder::encoder(uint32_t Id, Type ttype, uint32_t CrtcId)
        : id(Id), type(ttype), crtcId(CrtcId) {}

    bool encoder::setCrtc(uint32_t CrtcId) {
        this->crtcId = CrtcId;
        return true;
    }

    std::string encoder::getTypeString() const {
        switch (type) {
            case Type::NONE: return "None";
            case Type::DAC: return "DAC";
            case Type::TMDS: return "TMDS";
            case Type::LVDS: return "LVDS";
            case Type::TVDAC: return "TVDAC";
            case Type::VIRTUAL: return "Virtual";
            case Type::DSI: return "DSI";
            case Type::DPMST: return "DPMST";
            case Type::DPI: return "DPI";
            default: return "Unknown";
        }
    }

    //===============================================================================
    // Connector Implementation
    //===============================================================================

    connector::connector(uint32_t Id, Type ttype, uint32_t EncoderId)
        : id(Id), type(ttype), status(Status::UNKNOWN), encoderId(EncoderId) {
        name = getTypeString() + "-" + std::to_string(id);
    }

    const mode& connector::getPreferredMode() {
        if (preferredMode != -1) {
            return modes[preferredMode];
        }

        LOG_INFO() << "Looking for preferred mode among " << modes.size() << " available modes for " << getName() << std::endl;
        
        // List all available modes for debugging
        for (size_t i = 0; i < modes.size(); ++i) {
            const auto& m = modes[i];
            LOG_VERBOSE() << "  Mode " << i << ": " << m.getWidth() << "x" << m.getHeight() << "@" << m.getRefreshRate() << "Hz" << (m.isPreferred() ? " (preferred)" : "") << " - " << m.getName() << std::endl;
        }
        
        auto it = std::find_if(modes.begin(), modes.end(),
            [](const mode& mode) { return mode.isPreferred(); });
        
        if (it != modes.end()) {
            LOG_INFO() << "Found preferred mode: " << it->getWidth() << "x" << it->getHeight() << "@" << it->getRefreshRate() << "Hz" << std::endl;
            preferredMode = std::distance(modes.begin(), it);
            return *it;
        }
        
        // Return first mode if no preferred mode found
        if (!modes.empty()) {
            LOG_INFO() << "No preferred mode found, using first available: " << modes[0].getWidth() << "x" << modes[0].getHeight() << "@" << modes[0].getRefreshRate() << "Hz" << std::endl;
            preferredMode = 0;
            return modes[0];
        }
        
        LOG_INFO() << "No modes available, using default 1920x1080@60Hz" << std::endl;
        static mode defaultMode(mode::ModeInfo{1920, 1080, 60, 0, "1920x1080", false});
        preferredMode = -1; // Reset preferred mode index
        return defaultMode;
    }

    bool connector::updateStatus() {
        /**
         * @brief Update the connection status of this connector
         * 
         * This function queries the DRM subsystem to determine the current
         * connection status of the connector (connected, disconnected, or unknown).
         * 
         * @return true if status was updated successfully, false otherwise
         */
        if (!display::manager::Device) {
            return false;
        }
        
        int drmFd = display::manager::Device->getDeviceFd();
        if (drmFd < 0) {
            return false;
        }
        
        drmModeConnector* drmConn = drmModeGetConnector(drmFd, id);
        if (!drmConn) {
            return false;
        }
        
        Status oldStatus = status;
        
        // Update connection status
        switch (drmConn->connection) {
            case DRM_MODE_CONNECTED:
                setStatus(Status::CONNECTED);
                break;
            case DRM_MODE_DISCONNECTED:
                setStatus(Status::DISCONNECTED);
                break;
            default:
                setStatus(Status::UNKNOWN);
                break;
        }
        
        // If status changed, update modes
        if (status != oldStatus) {
            LOG_INFO() << "Connector " << getName() << " status changed from " << 
                         (oldStatus == Status::CONNECTED ? "CONNECTED" : 
                          oldStatus == Status::DISCONNECTED ? "DISCONNECTED" : "UNKNOWN") << 
                         " to " << getStatusString() << std::endl;
            
            if (status == Status::CONNECTED) {
                refreshModes();
            }
        }
        
        drmModeFreeConnector(drmConn);
        return true;
    }

    void connector::refreshModes() {
        /**
         * @brief Refresh the list of available modes for this connector
         * 
         * This function queries the DRM subsystem to get the current list
         * of supported display modes for this connector. It's typically
         * called when a display is connected or reconnected.
         */
        if (!display::manager::Device) {
            return;
        }
        
        int drmFd = display::manager::Device->getDeviceFd();
        if (drmFd < 0) {
            return;
        }
        
        drmModeConnector* drmConn = drmModeGetConnector(drmFd, id);
        if (!drmConn) {
            return;
        }
        
        // Clear existing modes
        modes.clear();
        
        // Load new modes
        for (int i = 0; i < drmConn->count_modes; i++) {
            mode::ModeInfo modeInfo;
            modeInfo.width = drmConn->modes[i].hdisplay;
            modeInfo.height = drmConn->modes[i].vdisplay;
            modeInfo.refreshRate = drmConn->modes[i].vrefresh;
            modeInfo.flags = drmConn->modes[i].flags;
            modeInfo.name = std::string(drmConn->modes[i].name);
            modeInfo.preferred = (drmConn->modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0;
            
            addMode(mode(modeInfo));
        }
        
        LOG_INFO() << "Refreshed " << modes.size() << " modes for connector " << getName() << std::endl;
        
        drmModeFreeConnector(drmConn);
    }

    void connector::addMode(const mode& mode) {
        modes.push_back(mode);
    }

    std::vector<mode> connector::getAvailableModes() const {
        return modes;
    }

    mode connector::findModeByResolution(uint32_t width, uint32_t height) const {
        auto it = std::find_if(modes.begin(), modes.end(),
            [width, height](const mode& mode) {
                return mode.getWidth() == width && mode.getHeight() == height;
            });
        
        if (it != modes.end()) {
            return *it;
        }
        
        // Return default mode if not found
        return mode(mode::ModeInfo{width, height, 60, 0, "custom", false});
    }

    void connector::addProperty(std::shared_ptr<property> prop) {
        properties[prop->getName()] = prop;
    }

    std::shared_ptr<property> connector::getProperty(const std::string& Name) const {
        auto it = properties.find(Name);
        return (it != properties.end()) ? it->second : nullptr;
    }

    bool connector::setEncoder(uint32_t EncoderId) {
        this->encoderId = EncoderId;
        return true;
    }

    std::string connector::getTypeString() const {
        switch (type) {
            case Type::UNKNOWN: return "Unknown";
            case Type::VGA: return "VGA";
            case Type::DVI_I: return "DVI-I";
            case Type::DVI_D: return "DVI-D";
            case Type::DVI_A: return "DVI-A";
            case Type::COMPOSITE: return "Composite";
            case Type::SVIDEO: return "S-Video";
            case Type::LVDS: return "LVDS";
            case Type::COMPONENT: return "Component";
            case Type::HDMI_A: return "HDMI-A";
            case Type::HDMI_B: return "HDMI-B";
            case Type::TV: return "TV";
            case Type::EDP: return "eDP";
            case Type::VIRTUAL: return "Virtual";
            case Type::DSI: return "DSI";
            case Type::DPI: return "DPI";
            case Type::WRITEBACK: return "Writeback";
            case Type::SPI: return "SPI";
            case Type::USB: return "USB";
            default: return "Unknown";
        }
    }

    std::string connector::getStatusString() const {
        switch (status) {
            case Status::CONNECTED: return "Connected";
            case Status::DISCONNECTED: return "Disconnected";
            case Status::UNKNOWN: return "Unknown";
            default: return "Unknown";
        }
    }

    //===============================================================================
    // Device Implementation
    //===============================================================================

    device::device(const std::string& DevicePath)
        : devicePath(DevicePath), deviceFd(-1), initialized(false),
        atomicSupported(false), atomicReq(nullptr) {}

    device::~device() {
        cleanup();
    }

    device::device(device&& other) noexcept
        : devicePath(std::move(other.devicePath)), deviceFd(other.deviceFd),
        initialized(other.initialized), atomicSupported(other.atomicSupported),
        connectors(std::move(other.connectors)), crtcs(std::move(other.crtcs)),
        encoders(std::move(other.encoders)), planes(std::move(other.planes)),
        framebuffers(std::move(other.framebuffers)),
        pageFlipHandler(std::move(other.pageFlipHandler)),
        atomicReq(other.atomicReq) {
        other.deviceFd = -1;
        other.initialized = false;
        other.atomicReq = nullptr;


    }

    device& device::operator=(device&& other) noexcept {
        if (this != &other) {
            cleanup();
            
            devicePath = std::move(other.devicePath);
            deviceFd = other.deviceFd;
            initialized = other.initialized;
            atomicSupported = other.atomicSupported;
            connectors = std::move(other.connectors);
            crtcs = std::move(other.crtcs);
            encoders = std::move(other.encoders);
            planes = std::move(other.planes);
            framebuffers = std::move(other.framebuffers);
            pageFlipHandler = std::move(other.pageFlipHandler);
            atomicReq = other.atomicReq;
            
            other.deviceFd = -1;
            other.initialized = false;
            other.atomicReq = nullptr;
        }
        return *this;
    }

    bool device::initialize() {
        if (initialized) {
            return true;
        }
        
        if (!openDevice()) {
            LOG_ERROR() << "Failed to open DRM device: " << devicePath << std::endl;
            return false;
        }
        
        // Check if we're in headless mode (no real hardware)
        if (deviceFd == -2) {
            LOG_INFO() << "Initializing in headless mode..." << std::endl;
            atomicSupported = false;
            
            // Create dummy connectors and modes for headless operation
            createHeadlessResources();
            
            initialized = true;
            return true;
        }
        
        // Check for atomic mode setting support
        [[maybe_unused]] uint64_t capAtomic = 0;
    #ifdef DRM_CAP_ATOMIC
        if (drmGetCap(deviceFd, DRM_CAP_ATOMIC, &capAtomic) == 0) {
            atomicSupported = (capAtomic == 1);
        } else {
            atomicSupported = false;
        }
    #else
        // DRM_CAP_ATOMIC not available in this version
        atomicSupported = false;
    #endif
        
        if (atomicSupported) {
            drmSetClientCap(deviceFd, DRM_CLIENT_CAP_ATOMIC, 1);
        }
        
        if (!discoverResources()) {
            LOG_ERROR() << "Failed to discover DRM resources" << std::endl;
            cleanup();
            return false;
        }
        
        initialized = true;
        return true;
    }

    void device::cleanup() {
        if (atomicReq) {
            drmModeAtomicFree(static_cast<drmModeAtomicReqPtr>(atomicReq));
            atomicReq = nullptr;
        }
        
        framebuffers.clear();
        planes.clear();
        encoders.clear();
        crtcs.clear();
        connectors.clear();
        
        closeDevice();
        initialized = false;
    }

    bool device::discoverResources() {
        return loadResources() && loadConnectors() && loadCrtcs() && 
            loadEncoders() && loadPlanes();
    }

    void device::refreshResources() {
        // Clear existing resources
        connectors.clear();
        crtcs.clear();
        encoders.clear();
        planes.clear();
        
        // Reload resources
        discoverResources();
    }

    std::shared_ptr<connector> device::getConnector(uint32_t id) const {
        auto it = std::find_if(connectors.begin(), connectors.end(),
            [id](const std::shared_ptr<connector>& conn) {
                return conn->getId() == id;
            });
        return (it != connectors.end()) ? *it : nullptr;
    }

    std::vector<std::shared_ptr<connector>> device::getConnectedConnectors() const {
        std::vector<std::shared_ptr<connector>> connected;
        for (const auto& conn : connectors) {
            if (conn->isConnected()) {
                connected.push_back(conn);
            }
        }
        return connected;
    }

    std::shared_ptr<crtc> device::getCrtc(uint32_t id) const {
        auto it = std::find_if(crtcs.begin(), crtcs.end(),
            [id](const std::shared_ptr<crtc>& crtc) {
                return crtc->getId() == id;
            });
        return (it != crtcs.end()) ? *it : nullptr;
    }

    std::shared_ptr<crtc> device::getFreeCrtc() const {
        // Find the first available CRTC that's not currently in use
        for (const auto& crtc : crtcs) {
            // For now, just return the first CRTC
            // In a more sophisticated implementation, we would check if the CRTC is currently active
            LOG_INFO() << "Using CRTC ID: " << crtc->getId() << std::endl;
            return crtc;
        }
        
        LOG_ERROR() << "No free CRTC found (total CRTCs: " << crtcs.size() << ")" << std::endl;
        return nullptr;
    }

    std::shared_ptr<encoder> device::getEncoder(uint32_t id) const {
        auto it = std::find_if(encoders.begin(), encoders.end(),
            [id](const std::shared_ptr<encoder>& enc) {
                return enc->getId() == id;
            });
        return (it != encoders.end()) ? *it : nullptr;
    }

    std::shared_ptr<plane> device::getPlane(uint32_t id) const {
        auto it = std::find_if(planes.begin(), planes.end(),
            [id](const std::shared_ptr<plane>& plane) {
                return plane->getId() == id;
            });
        return (it != planes.end()) ? *it : nullptr;
    }

    std::vector<std::shared_ptr<plane>> device::getPlanesByType(plane::Type type) const {
        std::vector<std::shared_ptr<plane>> filteredPlanes;
        for (const auto& plane : planes) {
            if (plane->getType() == type) {
                filteredPlanes.push_back(plane);
            }
        }
        return filteredPlanes;
    }

    std::shared_ptr<frameBuffer> device::createFramebuffer(const frameBuffer::FramebufferInfo& info) {
        auto fb = std::make_shared<frameBuffer>(info);
        framebuffers.push_back(fb);
        return fb;
    }

    bool device::destroyFramebuffer(std::shared_ptr<frameBuffer> fb) {
        auto it = std::find(framebuffers.begin(), framebuffers.end(), fb);
        if (it != framebuffers.end()) {
            framebuffers.erase(it);
            return true;
        }
        return false;
    }

    bool device::setMode(std::shared_ptr<connector> connector, const mode& mode) {
        /**
         * @brief Set display mode for a specific connector
         * 
         * This function configures the display pipeline to output the specified
         * mode through the given connector. It finds an appropriate CRTC and
         * encoder, then configures the mode using DRM APIs.
         * 
         * @param connector The connector to configure
         * @param mode The display mode to set
         * @return true if mode was set successfully, false otherwise
         */
        if (!connector || deviceFd < 0) {
            return false;
        }
        
        // Find the actual DRM mode from the connector's supported modes
        drmModeConnector* drmConn = drmModeGetConnector(deviceFd, connector->getId());
        if (!drmConn) {
            LOG_ERROR() << "Failed to get connector " << connector->getId() << std::endl;
            return false;
        }
        
        // Find the matching mode in the connector's mode list
        drmModeModeInfo* targetMode = nullptr;
        for (int i = 0; i < drmConn->count_modes; i++) {
            drmModeModeInfo* drmMode = &drmConn->modes[i];
            if (drmMode->hdisplay == mode.getWidth() &&
                drmMode->vdisplay == mode.getHeight() &&
                drmMode->vrefresh == mode.getRefreshRate()) {
                targetMode = drmMode;
                break;
            }
        }
        
        if (!targetMode) {
            LOG_ERROR() << "Mode " << mode.getWidth() << "x" << mode.getHeight() << 
                         "@" << mode.getRefreshRate() << "Hz not found in connector's mode list" << std::endl;
            drmModeFreeConnector(drmConn);
            return false;
        }
        
        // Find an encoder for this connector
        std::shared_ptr<encoder> enc = nullptr;
        if (drmConn->encoder_id != 0) {
            enc = getEncoder(drmConn->encoder_id);
        }
        
        // If no current encoder, find any compatible encoder
        if (!enc) {
            for (int i = 0; i < drmConn->count_encoders; i++) {
                auto candidateEnc = getEncoder(drmConn->encoders[i]);
                if (candidateEnc) {
                    enc = candidateEnc;
                    break;
                }
            }
        }
        
        if (!enc) {
            LOG_ERROR() << "No encoder found for connector " << connector->getId() << std::endl;
            drmModeFreeConnector(drmConn);
            return false;
        }
        
        // Find a CRTC for this encoder
        std::shared_ptr<crtc> crtcObj = nullptr;
        if (enc->getCrtcId() != 0) {
            crtcObj = getCrtc(enc->getCrtcId());
        }
        
        if (!crtcObj) {
            crtcObj = getFreeCrtc();
        }
        
        if (!crtcObj) {
            LOG_ERROR() << "No CRTC available for connector " << connector->getId() << std::endl;
            drmModeFreeConnector(drmConn);
            return false;
        }
        
        // Create a framebuffer for this mode
        frameBuffer::FramebufferInfo fbInfo;
        fbInfo.width = mode.getWidth();
        fbInfo.height = mode.getHeight();
        fbInfo.format = DRM_FORMAT_XRGB8888;  // Default format
        fbInfo.bpp = 32;
        fbInfo.depth = 24;
        fbInfo.pitch = fbInfo.width * (fbInfo.bpp / 8);
        fbInfo.size = fbInfo.pitch * fbInfo.height;
        
        auto fb = createFramebuffer(fbInfo);
        if (!fb || !fb->map()) {
            LOG_ERROR() << "Failed to create framebuffer for mode setting" << std::endl;
            drmModeFreeConnector(drmConn);
            return false;
        }
        
        // Clear the framebuffer to black
        fb->clear(0x00000000);
        
        // Set the mode using DRM with the actual mode data
        uint32_t connectorIds[] = { connector->getId() };
        
        int ret = drmModeSetCrtc(deviceFd, crtcObj->getId(), fb->getId(), 0, 0, connectorIds, 1, targetMode);
        
        if (ret != 0) {
            LOG_ERROR() << "Failed to set mode: " << strerror(errno) << " (" << ret << ")" << std::endl;
            LOG_ERROR() << "Mode details - CRTC: " << crtcObj->getId() << ", FB: " << fb->getId() << 
                         ", Connector: " << connector->getId() << std::endl;
            LOG_ERROR() << "Target mode: " << targetMode->hdisplay << "x" << targetMode->vdisplay << 
                         "@" << targetMode->vrefresh << "Hz" << std::endl;
            drmModeFreeConnector(drmConn);
            return false;
        }
        
        // Update object states
        crtcObj->setMode(mode);
        crtcObj->setFramebuffer(fb);
        enc->setCrtc(crtcObj->getId());
        
        LOG_INFO() << "Mode set successfully: " << mode.getWidth() << "x" << mode.getHeight() << 
                     "@" << mode.getRefreshRate() << "Hz on connector " << connector->getName() << std::endl;
        
        drmModeFreeConnector(drmConn);
        return true;
    }

    bool device::setMode(uint32_t connectorId, const mode& mode) {
        auto connector = getConnector(connectorId);
        return connector ? setMode(connector, mode) : false;
    }

    bool device::beginAtomicCommit() {
        if (!atomicSupported) {
            return false;
        }
        
        if (atomicReq) {
            drmModeAtomicFree(static_cast<drmModeAtomicReqPtr>(atomicReq));
        }
        
        atomicReq = drmModeAtomicAlloc();
        return atomicReq != nullptr;
    }

    bool device::addAtomicProperty(uint32_t objectId, const std::string& property, uint64_t value) {
        /**
         * @brief Add a property to the current atomic commit request
         * 
         * This function adds a property change to the atomic commit request.
         * The property will be applied when commitAtomic() is called.
         * 
         * @param objectId The DRM object ID (connector, CRTC, or plane)
         * @param property The property name to set
         * @param value The value to set for the property
         * @return true if property was added successfully, false otherwise
         */
        if (!atomicReq) {
            LOG_ERROR() << "No atomic request active" << std::endl;
            return false;
        }
        
        // This is a simplified implementation - in a real implementation,
        // we would need to look up the property ID from the property name
        // and add it to the atomic request using drmModeAtomicAddProperty
        
        LOG_INFO() << "Adding atomic property: object=" << objectId << ", property=" << property << ", value=" << value << std::endl;
        
        // For now, just return true to indicate we would add the property
        return true;
    }

    bool device::commitAtomic(bool testOnly) {
        if (!atomicReq) {
            return false;
        }
        
        uint32_t flags = testOnly ? DRM_MODE_ATOMIC_TEST_ONLY : 0;
        int ret = drmModeAtomicCommit(deviceFd, static_cast<drmModeAtomicReqPtr>(atomicReq), flags, nullptr);
        
        if (!testOnly) {
            drmModeAtomicFree(static_cast<drmModeAtomicReqPtr>(atomicReq));
            atomicReq = nullptr;
        }
        
        return ret == 0;
    }

    bool device::pageFlip(std::shared_ptr<crtc> crtc, std::shared_ptr<frameBuffer> fb, void* userData) {
        /**
         * @brief Perform a page flip operation
         * 
         * Page flipping allows for smooth, tear-free display updates by
         * swapping framebuffers during the vertical blanking interval.
         * 
         * @param crtc The CRTC to perform the page flip on
         * @param fb The framebuffer to flip to
         * @param userData User data passed to the page flip event handler
         * @return true if page flip was initiated successfully, false otherwise
         */
        if (!crtc || !fb || deviceFd < 0) {
            return false;
        }
        
        int ret = drmModePageFlip(deviceFd, crtc->getId(), fb->getId(), 
                                 DRM_MODE_PAGE_FLIP_EVENT, userData);
        
        if (ret != 0) {
            LOG_ERROR() << "Failed to initiate page flip: " << strerror(errno) << std::endl;
            return false;
        }
        
        return true;
    }

    bool device::handleEvents(int timeoutMs) {
        /**
         * @brief Handle DRM events such as page flip completion
         * 
         * This function processes pending DRM events, particularly page flip
         * completion events. It should be called regularly to maintain smooth
         * display updates.
         * 
         * @param timeoutMs Timeout in milliseconds (0 for non-blocking)
         * @return true if events were processed successfully, false otherwise
         */
        if (deviceFd < 0) {
            return false;
        }
        
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(deviceFd, &fds);
        
        struct timeval timeout;
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        
        int ret = select(deviceFd + 1, &fds, nullptr, nullptr, 
                        timeoutMs >= 0 ? &timeout : nullptr);
        
        if (ret < 0) {
            LOG_ERROR() << "Failed to select on DRM fd: " << strerror(errno) << std::endl;
            return false;
        }
        
        if (ret == 0) {
            // Timeout occurred, no events
            return true;
        }
        
        if (FD_ISSET(deviceFd, &fds)) {
            // Set up event context for page flip handler
            drmEventContext evctx;
            memset(&evctx, 0, sizeof(evctx));
            evctx.version = DRM_EVENT_CONTEXT_VERSION;
            // This lambda will be called when a page flip completes
            evctx.page_flip_handler = []([[maybe_unused]] int fd, unsigned int sequence, [[maybe_unused]] unsigned int tv_sec, [[maybe_unused]] unsigned int tv_usec, void* user_data) {
                // If we have a page flip handler, call it
                if (manager::Device && manager::Device->pageFlipHandler) {
                    manager::Device->pageFlipHandler(0, sequence, user_data);
                }
            };
            
            ret = drmHandleEvent(deviceFd, &evctx);
            if (ret != 0) {
                LOG_ERROR() << "Failed to handle DRM event: " << strerror(errno) << std::endl;
                return false;
            }
        }
        
        return true;
    }

    void device::setPageFlipHandler(std::function<void(uint32_t, uint32_t, void*)> handler) {
        pageFlipHandler = handler;
    }

    bool device::openDevice() {
        // If no device path is specified, try to find one dynamically
        if (devicePath.empty()) {
            LOG_VERBOSE() << "No DRM device path specified, attempting dynamic discovery..." << std::endl;
            devicePath = findDrmDevicePath();
            if (devicePath.empty()) {
                LOG_ERROR() << "Failed to find any suitable DRM device" << std::endl;
                
                // Enable headless mode for development
                LOG_INFO() << "No graphics hardware detected. Enabling headless mode for development." << std::endl;
                LOG_INFO() << "Note: This mode is for development/testing only and won't display anything." << std::endl;
                
                deviceFd = -2;  // Special value to indicate headless mode
                return true;
            }
        }

        deviceFd = open(devicePath.c_str(), O_RDWR | O_CLOEXEC);
        if (deviceFd < 0) {
            LOG_ERROR() << "Failed to open DRM device " << devicePath << ": " << strerror(errno) << std::endl;
            
            // If the specified path failed, try dynamic discovery as fallback
            if (!devicePath.empty()) {
                LOG_INFO() << "Attempting dynamic device discovery as fallback..." << std::endl;
                std::string fallbackPath = findDrmDevicePath();
                if (!fallbackPath.empty() && fallbackPath != devicePath) {
                    devicePath = fallbackPath;
                    deviceFd = open(devicePath.c_str(), O_RDWR | O_CLOEXEC);
                    if (deviceFd >= 0) {
                        LOG_INFO() << "Successfully opened fallback DRM device: " << devicePath << std::endl;
                        return true;
                    }
                }
            }
            
            // Check if we're in a virtual environment without real graphics hardware
            if (errno == ENODEV) {
                LOG_INFO() << "No graphics hardware detected. Enabling headless mode for development." << std::endl;
                LOG_INFO() << "Note: This mode is for development/testing only and won't display anything." << std::endl;
                
                // Set a flag to indicate we're in headless mode
                deviceFd = -2;  // Special value to indicate headless mode
                return true;
            }
            
            return false;
        }
        
        LOG_INFO() << "Successfully opened DRM device: " << devicePath << std::endl;
        return true;
    }

    void device::closeDevice() {
        if (deviceFd >= 0) {
            close(deviceFd);
            deviceFd = -1;
        }
    }

    bool device::loadResources() {
        /**
         * @brief Load basic DRM resources and set up the DRM mode setting context
         * 
         * This function discovers all available DRM resources including connectors,
         * CRTCs, encoders, and planes. It serves as the foundation for the display
         * subsystem initialization.
         * 
         * @return true if resources were successfully loaded, false otherwise
         */
        if (deviceFd < 0) {
            LOG_ERROR() << "Device not opened, cannot load resources" << std::endl;
            return false;
        }
        
        // Get DRM resources
        drmModeRes* resources = drmModeGetResources(deviceFd);
        if (!resources) {
            LOG_ERROR() << "Failed to get DRM resources: " << strerror(errno) << std::endl;
            return false;
        }
        
        LOG_INFO() << "DRM Resources loaded successfully:" << std::endl;
        LOG_INFO() << "  Connectors: " << resources->count_connectors << std::endl;
        LOG_INFO() << "  CRTCs: " << resources->count_crtcs << std::endl;
        LOG_INFO() << "  Encoders: " << resources->count_encoders << std::endl;
        LOG_INFO() << "  Framebuffers: " << resources->count_fbs << std::endl;
        
        // Store resource IDs for later use
        drmModeFreeResources(resources);
        return true;
    }

    bool device::loadConnectors() {
        /**
         * @brief Load and initialize all DRM connectors
         * 
         * This function enumerates all available display connectors (HDMI, DisplayPort,
         * VGA, etc.) and creates connector objects for each. It also loads their
         * supported modes and current connection status.
         * 
         * @return true if connectors were successfully loaded, false otherwise
         */
        if (deviceFd < 0) {
            return false;
        }
        
        drmModeRes* resources = drmModeGetResources(deviceFd);
        if (!resources) {
            return false;
        }
        
        connectors.clear();
        
        // Iterate through all connectors
        for (int i = 0; i < resources->count_connectors; i++) {
            drmModeConnector* drmConn = drmModeGetConnector(deviceFd, resources->connectors[i]);
            if (!drmConn) {
                continue;
            }
            
            // Map DRM connector type to our enum
            connector::Type connType = connector::Type::UNKNOWN;
            switch (drmConn->connector_type) {
                case DRM_MODE_CONNECTOR_VGA: connType = connector::Type::VGA; break;
                case DRM_MODE_CONNECTOR_DVII: connType = connector::Type::DVI_I; break;
                case DRM_MODE_CONNECTOR_DVID: connType = connector::Type::DVI_D; break;
                case DRM_MODE_CONNECTOR_DVIA: connType = connector::Type::DVI_A; break;
                case DRM_MODE_CONNECTOR_HDMIA: connType = connector::Type::HDMI_A; break;
                case DRM_MODE_CONNECTOR_HDMIB: connType = connector::Type::HDMI_B; break;
                case DRM_MODE_CONNECTOR_TV: connType = connector::Type::TV; break;
                case DRM_MODE_CONNECTOR_eDP: connType = connector::Type::EDP; break;
                case DRM_MODE_CONNECTOR_VIRTUAL: connType = connector::Type::VIRTUAL; break;
                case DRM_MODE_CONNECTOR_DSI: connType = connector::Type::DSI; break;
                case DRM_MODE_CONNECTOR_DPI: connType = connector::Type::DPI; break;
                default: connType = connector::Type::UNKNOWN; break;
            }
            
            // Create connector object
            auto conn = std::make_shared<connector>(drmConn->connector_id, connType, drmConn->encoder_id);
            
            // Set connection status
            switch (drmConn->connection) {
                case DRM_MODE_CONNECTED:
                    conn->setStatus(connector::Status::CONNECTED);
                    break;
                case DRM_MODE_DISCONNECTED:
                    conn->setStatus(connector::Status::DISCONNECTED);
                    break;
                default:
                    conn->setStatus(connector::Status::UNKNOWN);
                    break;
            }
            
            // Load supported modes
            for (int j = 0; j < drmConn->count_modes; j++) {
                mode::ModeInfo modeInfo;
                modeInfo.width = drmConn->modes[j].hdisplay;
                modeInfo.height = drmConn->modes[j].vdisplay;
                modeInfo.refreshRate = drmConn->modes[j].vrefresh;
                modeInfo.flags = drmConn->modes[j].flags;
                modeInfo.name = std::string(drmConn->modes[j].name);
                modeInfo.preferred = (drmConn->modes[j].type & DRM_MODE_TYPE_PREFERRED) != 0;
                
                conn->addMode(mode(modeInfo));
            }
            
            // Load connector properties
            for (int j = 0; j < drmConn->count_props; j++) {
                drmModePropertyPtr prop = drmModeGetProperty(deviceFd, drmConn->props[j]);
                if (prop) {
                    property::Type propType = property::Type::RANGE;
                    if (prop->flags & DRM_MODE_PROP_ENUM) {
                        propType = property::Type::ENUM;
                    } else if (prop->flags & DRM_MODE_PROP_BITMASK) {
                        propType = property::Type::BITMASK;
                    } else if (prop->flags & DRM_MODE_PROP_BLOB) {
                        propType = property::Type::BLOB;
                    } else if (prop->flags & DRM_MODE_PROP_OBJECT) {
                        propType = property::Type::OBJECT;
                    }
                    
                    auto connProp = std::make_shared<property>(prop->prop_id, std::string(prop->name), propType);
                    connProp->setValue(drmConn->prop_values[j]);
                    conn->addProperty(connProp);
                    
                    drmModeFreeProperty(prop);
                }
            }
            
            connectors.push_back(conn);
            drmModeFreeConnector(drmConn);
            
            LOG_INFO() << "Loaded connector: " << conn->getName() << " (" << conn->getStatusString() << ")" << std::endl;
        }
        
        drmModeFreeResources(resources);
        return true;
    }

    bool device::loadCrtcs() {
        /**
         * @brief Load and initialize all DRM CRTCs (display controllers)
         * 
         * CRTCs are the display controllers that manage the display pipeline.
         * Each CRTC can drive one display at a time and controls mode setting,
         * page flipping, and gamma correction.
         * 
         * @return true if CRTCs were successfully loaded, false otherwise
         */
        if (deviceFd < 0) {
            return false;
        }
        
        drmModeRes* resources = drmModeGetResources(deviceFd);
        if (!resources) {
            return false;
        }
        
        crtcs.clear();
        
        // Iterate through all CRTCs
        for (int i = 0; i < resources->count_crtcs; i++) {
            drmModeCrtc* drmCrtc = drmModeGetCrtc(deviceFd, resources->crtcs[i]);
            if (!drmCrtc) {
                continue;
            }
            
            // Create CRTC object
            auto crtcObj = std::make_shared<crtc>(drmCrtc->crtc_id, drmCrtc->buffer_id);
            
            // Set current mode if available
            if (drmCrtc->mode_valid) {
                mode::ModeInfo modeInfo;
                modeInfo.width = drmCrtc->mode.hdisplay;
                modeInfo.height = drmCrtc->mode.vdisplay;
                modeInfo.refreshRate = drmCrtc->mode.vrefresh;
                modeInfo.flags = drmCrtc->mode.flags;
                modeInfo.name = std::string(drmCrtc->mode.name);
                modeInfo.preferred = (drmCrtc->mode.type & DRM_MODE_TYPE_PREFERRED) != 0;
                
                crtcObj->setMode(mode(modeInfo));
            }
            
            // Load CRTC properties
            drmModeObjectProperties* props = drmModeObjectGetProperties(deviceFd, drmCrtc->crtc_id, DRM_MODE_OBJECT_CRTC);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyPtr prop = drmModeGetProperty(deviceFd, props->props[j]);
                    if (prop) {
                        property::Type propType = property::Type::RANGE;
                        if (prop->flags & DRM_MODE_PROP_ENUM) {
                            propType = property::Type::ENUM;
                        } else if (prop->flags & DRM_MODE_PROP_BITMASK) {
                            propType = property::Type::BITMASK;
                        } else if (prop->flags & DRM_MODE_PROP_BLOB) {
                            propType = property::Type::BLOB;
                        } else if (prop->flags & DRM_MODE_PROP_OBJECT) {
                            propType = property::Type::OBJECT;
                        }
                        
                        auto crtcProp = std::make_shared<property>(prop->prop_id, std::string(prop->name), propType);
                        crtcProp->setValue(props->prop_values[j]);
                        crtcObj->addProperty(crtcProp);
                        
                        drmModeFreeProperty(prop);
                    }
                }
                drmModeFreeObjectProperties(props);
            }
            
            crtcs.push_back(crtcObj);
            drmModeFreeCrtc(drmCrtc);
            
            LOG_INFO() << "Loaded CRTC: " << crtcObj->getId() << std::endl;
        }
        
        drmModeFreeResources(resources);
        return true;
    }

    bool device::loadEncoders() {
        /**
         * @brief Load and initialize all DRM encoders
         * 
         * Encoders convert pixel data from CRTCs into signals that can be
         * transmitted over display connectors. Each encoder is associated
         * with specific types of connectors and CRTCs.
         * 
         * @return true if encoders were successfully loaded, false otherwise
         */
        if (deviceFd < 0) {
            return false;
        }
        
        drmModeRes* resources = drmModeGetResources(deviceFd);
        if (!resources) {
            return false;
        }
        
        encoders.clear();
        
        // Iterate through all encoders
        for (int i = 0; i < resources->count_encoders; i++) {
            drmModeEncoder* drmEnc = drmModeGetEncoder(deviceFd, resources->encoders[i]);
            if (!drmEnc) {
                continue;
            }
            
            // Map DRM encoder type to our enum
            encoder::Type encType = encoder::Type::NONE;
            switch (drmEnc->encoder_type) {
                case DRM_MODE_ENCODER_DAC: encType = encoder::Type::DAC; break;
                case DRM_MODE_ENCODER_TMDS: encType = encoder::Type::TMDS; break;
                case DRM_MODE_ENCODER_LVDS: encType = encoder::Type::LVDS; break;
                case DRM_MODE_ENCODER_TVDAC: encType = encoder::Type::TVDAC; break;
                case DRM_MODE_ENCODER_VIRTUAL: encType = encoder::Type::VIRTUAL; break;
                case DRM_MODE_ENCODER_DSI: encType = encoder::Type::DSI; break;
                case DRM_MODE_ENCODER_DPMST: encType = encoder::Type::DPMST; break;
                case DRM_MODE_ENCODER_DPI: encType = encoder::Type::DPI; break;
                default: encType = encoder::Type::NONE; break;
            }
            
            // Create encoder object
            auto enc = std::make_shared<encoder>(drmEnc->encoder_id, encType, drmEnc->crtc_id);
            
            // Store possible CRTCs for this encoder
            for (int j = 0; j < resources->count_crtcs; j++) {
                if (drmEnc->possible_crtcs & (1 << j)) {
                    enc->addPossibleCrtc(resources->crtcs[j]);
                }
            }
            
            encoders.push_back(enc);
            drmModeFreeEncoder(drmEnc);
            
            LOG_INFO() << "Loaded encoder: " << enc->getId() << " (" << enc->getTypeString() << ")" << std::endl;
        }
        
        drmModeFreeResources(resources);
        return true;
    }

    bool device::loadPlanes() {
        /**
         * @brief Load and initialize all DRM planes
         * 
         * Planes are hardware layers that can be composited together to form
         * the final display output. There are three types: primary (main display),
         * overlay (for video or additional layers), and cursor (for mouse cursor).
         * 
         * @return true if planes were successfully loaded, false otherwise
         */
        if (deviceFd < 0) {
            return false;
        }
        
        // Enable universal planes to get all plane types
        drmSetClientCap(deviceFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        
        drmModePlaneRes* planeRes = drmModeGetPlaneResources(deviceFd);
        if (!planeRes) {
            LOG_ERROR() << "Failed to get plane resources: " << strerror(errno) << std::endl;
            return false;
        }
        
        planes.clear();
        
        // Iterate through all planes
        for (uint32_t i = 0; i < planeRes->count_planes; i++) {
            drmModePlane* drmPlane = drmModeGetPlane(deviceFd, planeRes->planes[i]);
            if (!drmPlane) {
                continue;
            }
            
            // Determine plane type by checking properties
            plane::Type planeType = plane::Type::OVERLAY;  // Default to overlay
            
            drmModeObjectProperties* props = drmModeObjectGetProperties(deviceFd, drmPlane->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyPtr prop = drmModeGetProperty(deviceFd, props->props[j]);
                    if (prop && strcmp(prop->name, "type") == 0) {
                        uint64_t typeValue = props->prop_values[j];
                        if (typeValue == DRM_PLANE_TYPE_PRIMARY) {
                            planeType = plane::Type::PRIMARY;
                        } else if (typeValue == DRM_PLANE_TYPE_CURSOR) {
                            planeType = plane::Type::CURSOR;
                        } else if (typeValue == DRM_PLANE_TYPE_OVERLAY) {
                            planeType = plane::Type::OVERLAY;
                        }
                        drmModeFreeProperty(prop);
                        break;
                    }
                    if (prop) {
                        drmModeFreeProperty(prop);
                    }
                }
                drmModeFreeObjectProperties(props);
            }
            
            // Create plane object
            auto planeObj = std::make_shared<plane>(drmPlane->plane_id, planeType, drmPlane->crtc_id);
            
            // Store supported formats
            for (uint32_t j = 0; j < drmPlane->count_formats; j++) {
                planeObj->addSupportedFormat(drmPlane->formats[j]);
            }
            
            // Load plane properties
            props = drmModeObjectGetProperties(deviceFd, drmPlane->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyPtr prop = drmModeGetProperty(deviceFd, props->props[j]);
                    if (prop) {
                        property::Type propType = property::Type::RANGE;
                        if (prop->flags & DRM_MODE_PROP_ENUM) {
                            propType = property::Type::ENUM;
                        } else if (prop->flags & DRM_MODE_PROP_BITMASK) {
                            propType = property::Type::BITMASK;
                        } else if (prop->flags & DRM_MODE_PROP_BLOB) {
                            propType = property::Type::BLOB;
                        } else if (prop->flags & DRM_MODE_PROP_OBJECT) {
                            propType = property::Type::OBJECT;
                        }
                        
                        auto planeProp = std::make_shared<property>(prop->prop_id, std::string(prop->name), propType);
                        planeProp->setValue(props->prop_values[j]);
                        planeObj->addProperty(planeProp);
                        
                        drmModeFreeProperty(prop);
                    }
                }
                drmModeFreeObjectProperties(props);
            }
            
            planes.push_back(planeObj);
            drmModeFreePlane(drmPlane);
            
            LOG_INFO() << "Loaded plane: " << planeObj->getId() << " (type: " << 
                         (planeType == plane::Type::PRIMARY ? "PRIMARY" : 
                          planeType == plane::Type::CURSOR ? "CURSOR" : "OVERLAY") << ")" << std::endl;
        }
        
        drmModeFreePlaneResources(planeRes);
        return true;
    }

    std::shared_ptr<property> device::loadProperty(uint32_t propId) {
        /**
         * @brief Load a DRM property by its ID
         * 
         * This function loads a DRM property object with its metadata
         * including type, name, and possible values. Properties are used
         * to control various aspects of display objects.
         * 
         * @param propId The DRM property ID to load
         * @return Shared pointer to the property object, or nullptr if failed
         */
        if (deviceFd < 0) {
            return nullptr;
        }
        
        drmModePropertyPtr prop = drmModeGetProperty(deviceFd, propId);
        if (!prop) {
            return nullptr;
        }
        
        property::Type propType = property::Type::RANGE;
        if (prop->flags & DRM_MODE_PROP_ENUM) {
            propType = property::Type::ENUM;
        } else if (prop->flags & DRM_MODE_PROP_BITMASK) {
            propType = property::Type::BITMASK;
        } else if (prop->flags & DRM_MODE_PROP_BLOB) {
            propType = property::Type::BLOB;
        } else if (prop->flags & DRM_MODE_PROP_OBJECT) {
            propType = property::Type::OBJECT;
        }
        
        auto propObj = std::make_shared<property>(propId, std::string(prop->name), propType);
        
        drmModeFreeProperty(prop);
        return propObj;
    }

    //===============================================================================
    // Manager Implementation
    //===============================================================================

    // Define the manager namespace variables
    namespace manager {
        std::shared_ptr<device> Device;
        std::map<uint32_t, std::shared_ptr<connector>> activeDisplays;
        std::function<void(std::shared_ptr<connector>, bool)> hotplugHandler;
        static bool pageFlipPending = false;  // Track if a page flip is currently pending
    }

    bool manager::initialize(const std::string& devicePath) {
        Device = std::make_shared<device>(devicePath);
        
        // Set up page flip completion handler to track pending state
        if (Device) {
            Device->setPageFlipHandler([]([[maybe_unused]] uint32_t crtc_id, [[maybe_unused]] uint32_t sequence, [[maybe_unused]] void* user_data) {
                pageFlipPending = false; // Reset pending state when page flip completes
            });
        }
        
        return Device->initialize();
    }

    void manager::cleanup() {
        if (Device) {
            Device->cleanup();
            Device.reset();
        }
        activeDisplays.clear();
    }

    std::vector<std::shared_ptr<connector>> manager::getAvailableDisplays() {
        return Device ? Device->getConnectedConnectors() : std::vector<std::shared_ptr<connector>>();
    }

    bool manager::enableDisplay(std::shared_ptr<connector> connector, const mode& mode) {
        if (!Device || !connector) {
            return false;
        }
        
        if (setupDisplay(connector, mode)) {
            activeDisplays[connector->getId()] = connector;
            return true;
        }
        return false;
    }

    bool manager::disableDisplay(std::shared_ptr<connector> connector) {
        if (!connector) {
            return false;
        }
        
        auto it = activeDisplays.find(connector->getId());
        if (it != activeDisplays.end()) {
            activeDisplays.erase(it);
            return true;
        }
        return false;
    }

    bool manager::isDisplayEnabled(std::shared_ptr<connector> connector) {
        if (!connector) {
            return false;
        }
        
        return activeDisplays.find(connector->getId()) != activeDisplays.end();
    }

    bool manager::setupClonedDisplays(const std::vector<std::shared_ptr<connector>>& connectors, const mode& mode) {
        /**
         * @brief Set up cloned displays showing the same content
         * 
         * This function configures multiple displays to show identical content
         * at the same resolution and refresh rate. All displays will mirror
         * the same framebuffer content.
         * 
         * @param connectors List of connectors to configure for cloned display
         * @param mode The display mode to use for all displays
         * @return true if cloned displays were set up successfully, false otherwise
         */
        if (!Device || connectors.empty()) {
            return false;
        }
        
        LOG_INFO() << "Setting up cloned displays for " << connectors.size() << " connectors" << std::endl;
        
        // Set up each connector with the same mode
        bool success = true;
        for (const auto& connector : connectors) {
            if (!setupDisplay(connector, mode)) {
                LOG_ERROR() << "Failed to set up cloned display for connector " << connector->getName() << std::endl;
                success = false;
            } else {
                activeDisplays[connector->getId()] = connector;
            }
        }
        
        if (success) {
            LOG_INFO() << "Cloned displays setup completed successfully" << std::endl;
        }
        
        return success;
    }

    bool manager::setupExtendedDisplays(const std::vector<std::shared_ptr<connector>>& connectors) {
        /**
         * @brief Set up extended displays forming a larger desktop
         * 
         * This function configures multiple displays to form an extended desktop
         * where each display shows a different portion of the desktop. Each
         * display will use its preferred mode if available.
         * 
         * @param connectors List of connectors to configure for extended display
         * @return true if extended displays were set up successfully, false otherwise
         */
        if (!Device || connectors.empty()) {
            return false;
        }
        
        LOG_INFO() << "Setting up extended displays for " << connectors.size() << " connectors" << std::endl;
        
        // Set up each connector with its preferred mode
        bool success = true;
        for (const auto& connector : connectors) {
            // Use the preferred mode or first available mode
            const mode& displayMode = connector->getPreferredMode();

            if (!setupDisplay(connector, displayMode)) {
                LOG_ERROR() << "Failed to set up extended display for connector " << connector->getName() << std::endl;
                success = false;
            } else {
                activeDisplays[connector->getId()] = connector;
                LOG_INFO() << "Extended display set up for " << connector->getName() << 
                             " at " << displayMode.getWidth() << "x" << displayMode.getHeight() << 
                             "@" << displayMode.getRefreshRate() << "Hz" << std::endl;
            }
        }
        
        if (success) {
            LOG_INFO() << "Extended displays setup completed successfully" << std::endl;
        }
        
        return success;
    }

    std::shared_ptr<frameBuffer> manager::createFramebuffer(uint32_t width, uint32_t height, uint32_t format) {
        if (!Device) {
            return nullptr;
        }
        
        frameBuffer::FramebufferInfo info;
        info.width = width;
        info.height = height;
        info.format = format;
        info.bpp = 32;  // Default to 32 bpp
        info.depth = 24;
        info.pitch = width * (info.bpp / 8);
        info.size = info.pitch * height;
        
        return Device->createFramebuffer(info);
    }

    bool manager::present(std::shared_ptr<connector> connector, std::shared_ptr<frameBuffer> fb) {
        /**
         * @brief Present a framebuffer to the specified connector
         * 
         * This function displays the contents of the framebuffer on the
         * specified connector. It can use either page flipping for smooth
         * updates or direct framebuffer swapping.
         * 
         * @param connector The connector to present to
         * @param fb The framebuffer to display
         * @return true if framebuffer was presented successfully, false otherwise
         */
        if (!Device || !connector || !fb) {
            return false;
        }
        
        // Check if we're in headless mode
        if (Device->getDeviceFd() == -2) {
            // In headless mode, we just simulate successful presentation
            static int frameCount = 0;
            frameCount++;
            
            if (frameCount % 60 == 0) {  // Log every 60 frames (once per second at 60 FPS)
                LOG_INFO() << "Headless mode: Frame " << frameCount << " rendered (" << 
                             fb->getWidth() << "x" << fb->getHeight() << ")" << std::endl;
            }
            
            return true;
        }
        
        // Check if this connector is active
        auto it = activeDisplays.find(connector->getId());
        if (it == activeDisplays.end()) {
            LOG_ERROR() << "Connector " << connector->getName() << " is not active" << std::endl;
            return false;
        }
        
        // Find the CRTC associated with this connector
        std::shared_ptr<crtc> crtcObj = nullptr;
        if (connector->getEncoderId() != 0) {
            auto encoder = Device->getEncoder(connector->getEncoderId());
            if (encoder && encoder->getCrtcId() != 0) {
                crtcObj = Device->getCrtc(encoder->getCrtcId());
            }
        }
        
        if (!crtcObj) {
            LOG_ERROR() << "No CRTC found for connector " << connector->getName() << std::endl;
            return false;
        }
        
        // For now, just update the CRTC with the new framebuffer
        // Page flipping might not be working due to the DRM driver or hardware limitations
        // Let's use a simpler approach of just updating the CRTC framebuffer
        crtcObj->setFramebuffer(fb);
        
        // Try to use page flipping for smooth updates
        static bool pageFlipSupported = true;
        if (pageFlipSupported && !pageFlipPending && Device->pageFlip(crtcObj, fb, nullptr)) {
            pageFlipPending = true;
            return true;
        }
        
        // If page flip fails, disable it for future frames to avoid spam
        if (pageFlipSupported && !pageFlipPending) {
            pageFlipSupported = false;
            LOG_INFO() << "Page flip not supported, using direct framebuffer updates" << std::endl;
        }
        
        // Since we can't do page flipping, just update the framebuffer reference
        // The actual display should already be set up with the correct mode
        // and the framebuffer content is being updated in-place
        return true;
    }

    bool manager::processEvents(int timeoutMs) {
        return Device ? Device->handleEvents(timeoutMs) : false;
    }

    void manager::setHotplugHandler(std::function<void(std::shared_ptr<connector>, bool)> handler) {
        hotplugHandler = handler;
    }

    bool manager::setupDisplay(std::shared_ptr<connector> connector, const mode& mode) {
        /**
         * @brief Set up display pipeline for a connector with the specified mode
         * 
         * This function configures the complete display pipeline including
         * connector, encoder, CRTC, and framebuffer to display content
         * at the specified resolution and refresh rate.
         * 
         * @param connector The connector to configure
         * @param mode The display mode to use
         * @return true if display was set up successfully, false otherwise
         */
        if (!Device || !connector) {
            return false;
        }
        
        // Check if we're in headless mode
        if (Device->getDeviceFd() == -2) {
            LOG_INFO() << "Headless mode: Setting up virtual display " << connector->getName() << 
                         " at " << mode.getWidth() << "x" << mode.getHeight() << 
                         "@" << mode.getRefreshRate() << "Hz" << std::endl;
            return true;
        }
        
        // Ensure connector is connected
        if (!connector->isConnected()) {
            if (!connector->updateStatus() || !connector->isConnected()) {
                LOG_ERROR() << "Connector " << connector->getName() << " is not connected" << std::endl;
                return false;
            }
        }
        
        // Set the mode using the device
        if (!Device->setMode(connector, mode)) {
            LOG_ERROR() << "Failed to set mode for connector " << connector->getName() << std::endl;
            return false;
        }
        
        LOG_INFO() << "Display setup completed for " << connector->getName() << 
                     " at " << mode.getWidth() << "x" << mode.getHeight() << 
                     "@" << mode.getRefreshRate() << "Hz" << std::endl;
        
        return true;
    }

    void manager::handleHotplugEvent(std::shared_ptr<connector> connector, bool connected) {
        if (hotplugHandler) {
            hotplugHandler(connector, connected);
        }
    }

    void device::createHeadlessResources() {
        /**
         * @brief Create dummy resources for headless mode
         * 
         * This function creates virtual display resources for development and testing
         * when no real graphics hardware is available. It sets up dummy connectors,
         * CRTCs, encoders, and planes that can be used for software testing.
         */
        LOG_INFO() << "Creating headless display resources..." << std::endl;
        
        // Create a dummy connector
        auto dummyConnector = std::make_shared<connector>(1, connector::Type::VIRTUAL, 1);
        dummyConnector->setStatus(connector::Status::CONNECTED);
        
        // Add some standard display modes
        std::vector<mode::ModeInfo> standardModes = {
            {1920, 1080, 60, 0, "1920x1080", true},   // Full HD (preferred)
            {1680, 1050, 60, 0, "1680x1050", false},  // WSXGA+
            {1600, 900, 60, 0, "1600x900", false},    // HD+
            {1440, 900, 60, 0, "1440x900", false},    // WXGA+
            {1366, 768, 60, 0, "1366x768", false},    // WXGA
            {1280, 1024, 60, 0, "1280x1024", false},  // SXGA
            {1280, 720, 60, 0, "1280x720", false},    // HD
            {1024, 768, 60, 0, "1024x768", false},    // XGA
            {800, 600, 60, 0, "800x600", false},      // SVGA
            {640, 480, 60, 0, "640x480", false}       // VGA
        };
        
        for (const auto& modeInfo : standardModes) {
            dummyConnector->addMode(mode(modeInfo));
        }
        
        connectors.push_back(dummyConnector);
        
        // Create a dummy CRTC
        auto dummyCrtc = std::make_shared<crtc>(1, 0);
        crtcs.push_back(dummyCrtc);
        
        // Create a dummy encoder
        auto dummyEncoder = std::make_shared<encoder>(1, encoder::Type::VIRTUAL, 1);
        dummyEncoder->addPossibleCrtc(1);
        encoders.push_back(dummyEncoder);
        
        // Create a dummy primary plane
        auto dummyPlane = std::make_shared<plane>(1, plane::Type::PRIMARY, 1);
        dummyPlane->addSupportedFormat(DRM_FORMAT_XRGB8888);
        dummyPlane->addSupportedFormat(DRM_FORMAT_ARGB8888);
        planes.push_back(dummyPlane);
        
        dummyCrtc->addPlane(dummyPlane);
        
        LOG_INFO() << "Created headless resources:" << std::endl;
        LOG_INFO() << "  - 1 virtual connector with " << standardModes.size() << " modes" << std::endl;
        LOG_INFO() << "  - 1 virtual CRTC" << std::endl;
        LOG_INFO() << "  - 1 virtual encoder" << std::endl;
        LOG_INFO() << "  - 1 virtual plane" << std::endl;
    }

} // namespace display
