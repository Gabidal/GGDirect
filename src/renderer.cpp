#include "renderer.h"
#include "types.h"
#include "display.h"
#include "font.h"
#include "logger.h"
#include "config.h"

#include <thread>
#include <iostream>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace renderer {
    
    // Optimized cell cache with pre-converted framebuffer data
    struct OptimizedCellCache {
        types::Cell cellID;                    // Cell fingerprint for O(1) comparison
        std::vector<uint32_t> XRGBPixels;      // Pre-converted XRGB8888 data
        int width;
        int height;
        bool isValid;
        
        OptimizedCellCache() : width(0), height(0), isValid(false) {}
        
        // Fast initialization with pre-allocated size
        void initialize(int w, int h) {
            width = w;
            height = h;
            XRGBPixels.resize(w * h);
            isValid = false;
        }
        
        // Convert RGB cell data to XRGB8888 format for direct framebuffer copying
        void convertFromRGB(const font::cellRenderData& cellData) {
            if (XRGBPixels.size() != static_cast<size_t>(cellData.width * cellData.height)) {
                XRGBPixels.resize(cellData.width * cellData.height);
            }
            
            const size_t pixelCount = cellData.pixels.size();
            for (size_t i = 0; i < pixelCount; i++) {
                XRGBPixels[i] = types::toXRGB8888(cellData.pixels[i]);
            }
            
            width = cellData.width;
            height = cellData.height;
            isValid = true;
        }
    };
    
    // Fast framebuffer blitting using memcpy for row-based operations
    inline void blitCellToFramebuffer(uint32_t* fbBuffer, int fbWidth, int fbHeight, int startX, int startY, const OptimizedCellCache& cache) {
        // Bounds checking
        if (startX >= fbWidth || startY >= fbHeight || !cache.isValid) {
            return;
        }
        
        const int maxCopyWidth = std::min(cache.width, fbWidth - startX);
        const int maxCopyHeight = std::min(cache.height, fbHeight - startY);
        
        // Memory operation batching - copy entire rows with memcpy
        for (int y = 0; y < maxCopyHeight; y++) {
            const int srcOffset = y * cache.width;
            const int dstOffset = (startY + y) * fbWidth + startX;
            
            // Fast row-based copy using memcpy instead of pixel-by-pixel
            std::memcpy(&fbBuffer[dstOffset], &cache.XRGBPixels[srcOffset], 
                       maxCopyWidth * sizeof(uint32_t));
        }
    }
    
    // Clear a rectangular area in the framebuffer with a specific color or wallpaper
    inline void clearFramebufferRect(uint32_t* fbBuffer, int fbWidth, int fbHeight, const types::rectangle& rect, uint32_t defaultColor) {
        // Bounds checking
        if (rect.position.x >= fbWidth || rect.position.y >= fbHeight || 
            rect.size.x <= 0 || rect.size.y <= 0) {
            return;
        }
        
        // Calculate actual clearing bounds
        int startX = std::max(0, rect.position.x);
        int startY = std::max(0, rect.position.y);
        int endX = std::min(fbWidth, rect.position.x + rect.size.x);
        int endY = std::min(fbHeight, rect.position.y + rect.size.y);
        
        // Try to get wallpaper path
        std::string wallpaperPath = config::manager::getWallpaperPath();
        
        // Clear line by line
        for (int y = startY; y < endY; y++) {
            uint32_t* lineStart = &fbBuffer[y * fbWidth + startX];
            
            if (!wallpaperPath.empty()) {
                // Use wallpaper if available
                for (int x = startX; x < endX; x++) {
                    uint32_t wallpaperPixel;
                    if (config::manager::getWallpaperPixel(x, y, wallpaperPixel)) {
                        fbBuffer[y * fbWidth + x] = wallpaperPixel;
                    } else {
                        fbBuffer[y * fbWidth + x] = defaultColor;
                    }
                }
            } else {
                // Use solid color
                std::fill(lineStart, lineStart + (endX - startX), defaultColor);
            }
        }
    }
    
    // Renderer state
    static std::shared_ptr<display::frameBuffer> currentFramebuffer;
    static std::shared_ptr<display::connector> primaryConnector;
    static std::shared_ptr<display::crtc> primaryCrtc;
    static std::shared_ptr<display::plane> primaryPlane;
    static std::unique_ptr<display::mode> currentMode;
    static bool rendererInitialized = false;
    static bool shouldExit = false;  // Flag to control renderer thread exit
    
    // Forward declaration - kept for backward compatibility
    void renderCellToFramebuffer(uint32_t* fbBuffer, int fbWidth, int fbHeight, int startX, int startY, const font::cellRenderData& cellData);
    
    // Initialize display and font systems
    void init() {
        // Initialize display system
        if (!display::manager::initialize()) {
            LOG_ERROR() << "Failed to initialize display system" << std::endl;
            return;
        }
        
        // Initialize font system
        if (!font::manager::initialize()) {
            LOG_ERROR() << "Failed to initialize font system" << std::endl;
            return;
        }
        
        // Set up display resources
        auto availableDisplays = display::manager::getAvailableDisplays();
        if (availableDisplays.empty()) {
            LOG_ERROR() << "No available displays found" << std::endl;
            return;
        }
        
        // Use the first available display
        primaryConnector = availableDisplays[0];
        if (!primaryConnector->isConnected()) {
            LOG_ERROR() << "Primary display is not connected" << std::endl;
            return;
        }
        
        // Get preferred mode
        auto modes = primaryConnector->getAvailableModes();
        if (modes.empty()) {
            LOG_ERROR() << "No available modes for primary display" << std::endl;
            return;
        }
        
        currentMode = std::make_unique<display::mode>(primaryConnector->getPreferredMode());
        
        // Enable the display
        if (!display::manager::enableDisplay(primaryConnector, *currentMode)) {
            LOG_ERROR() << "Failed to enable primary display" << std::endl;
            return;
        }
        
        // Create framebuffer
        currentFramebuffer = display::manager::createFramebuffer(
            currentMode->getWidth(), 
            currentMode->getHeight(), 
            0x34325258  // DRM_FORMAT_XRGB8888
        );
        
        if (!currentFramebuffer) {
            LOG_ERROR() << "Failed to create framebuffer" << std::endl;
            return;
        }
        
        // Map the framebuffer for CPU access
        if (!currentFramebuffer->map()) {
            LOG_ERROR() << "Failed to map framebuffer" << std::endl;
            return;
        }
        
        rendererInitialized = true;
        
        // Load wallpaper if configured
        std::string wallpaperPath = config::manager::getWallpaperPath();
        if (!wallpaperPath.empty()) {
            config::manager::loadWallpaper(wallpaperPath);
        }
        
        // Start rendering thread
        std::thread renderingThread([](){
            size_t frameCounter = 0;
            auto lastLogTime = std::chrono::high_resolution_clock::now();
            int framesRendered = 0;
            int totalFrames = 0;
            
            while (!shouldExit) {
                bool needsPresent = false;
                auto frameStart = std::chrono::high_resolution_clock::now();
                
                window::manager::handles([&needsPresent](std::vector<window::handle>& self){
                    // First we'll need to order the handles, so that rendering order is correct, where lower z's get drawn first to be overdrawn.
                    std::sort(self.begin(), self.end(), [](const window::handle& a, const window::handle& b) {
                        types::rectangle aRectangle = a.getCellCoordinates();
                        types::rectangle bRectangle = b.getCellCoordinates();
                        return aRectangle.position.z < bRectangle.position.z;  // Sort by z position
                    });

                    // Receive cell buffers and check for disconnected handles
                    for (int i = static_cast<int>(self.size()) - 1; i >= 0; i--) {
                        if (self[i].connection.isClosed()) {
                            // Connection was closed by client or network failure
                            LOG_INFO() << "Handle " << i << " disconnected, marking for removal" << std::endl;
                            continue;
                        }

                        if (self[i].errorCount > window::handle::maxAllowedErrorCount) {
                            // Handle has too many communication errors - likely disconnected
                            LOG_ERROR() << "Handle " << i << " has excessive errors (" << 
                                         self[i].errorCount << " > " << window::handle::maxAllowedErrorCount << "), marking for removal" << std::endl;
                            self[i].close();
                            continue;
                        }

                        // Poll active handles for new data
                        self[i].poll();
                    }

                    // Handle resize stains - clear areas that need to be cleared due to window resizes
                    if (currentFramebuffer) {
                        uint32_t* fbBuffer = static_cast<uint32_t*>(currentFramebuffer->getBuffer());
                        if (fbBuffer) {
                            for (auto& handle : self) {
                                if (window::stain::has(handle.dirty, window::stain::type::resize)) {
                                    // Get the area that needs to be cleared
                                    types::rectangle clearArea = handle.getResizeClearArea();
                                    
                                    // Clear the area with configured background color
                                    uint32_t backgroundColor = config::manager::getBackgroundColor();
                                    clearFramebufferRect(fbBuffer, 
                                                       currentFramebuffer->getPitch() / sizeof(uint32_t), 
                                                       currentFramebuffer->getHeight(), 
                                                       clearArea, 
                                                       backgroundColor);
                                    
                                    // Clear the resize stain flag
                                    handle.set(window::stain::type::resize, false);
                                    
                                    LOG_VERBOSE() << "Cleared resize area: " << clearArea.position.x << "," << clearArea.position.y 
                                                  << " size: " << clearArea.size.x << "x" << clearArea.size.y << std::endl;
                                    
                                    needsPresent = true;
                                }
                            }
                        }
                    }

                    // Render gotten cell buffers.
                    for (auto& handle : self) {
                        if (renderHandle(&handle)) {
                            needsPresent = true;
                        }
                    }

                });

                // Clean up dead handles after polling
                window::manager::cleanupDeadHandles();

                // Process DRM events to handle page flip completions
                // This prevents memory accumulation in the DRM page flip queue
                display::manager::processEvents(0);  // Non-blocking call

                // Present the framebuffer only once per frame if anything was rendered
                if (needsPresent && currentFramebuffer) {
                    display::manager::present(primaryConnector, currentFramebuffer);
                    framesRendered++;
                }
                
                totalFrames++;
                frameCounter++;

                // Log performance statistics every 5 seconds
                auto now = std::chrono::high_resolution_clock::now();
                auto timeSinceLastLog = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime);
                if (timeSinceLastLog.count() >= 5) {
                    double avgFPS = static_cast<double>(totalFrames) / timeSinceLastLog.count();
                    double renderRate = static_cast<double>(framesRendered) / timeSinceLastLog.count();
                    LOG_VERBOSE() << "Renderer stats: " << avgFPS << " FPS, " 
                                  << renderRate << " rendered FPS, " 
                                  << ((renderRate / avgFPS) * 100.0) << "% utilization" << std::endl;
                    
                    lastLogTime = now;
                    framesRendered = 0;
                    totalFrames = 0;
                }

                // Adaptive timing: only sleep if we didn't do much work
                auto frameEnd = std::chrono::high_resolution_clock::now();
                auto frameTime = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart);
                
                if (needsPresent) {
                    // If we rendered something, aim for higher FPS (120 FPS = ~8ms)
                    const auto targetFrameTime = std::chrono::milliseconds(32);
                    if (frameTime < targetFrameTime) {
                        std::this_thread::sleep_for(targetFrameTime - frameTime);
                    }
                } else {
                    // If nothing was rendered, sleep longer to reduce CPU usage (30 FPS = ~33ms)
                    const auto idleFrameTime = std::chrono::milliseconds(128);
                    if (frameTime < idleFrameTime) {
                        std::this_thread::sleep_for(idleFrameTime - frameTime);
                    }
                }
            }
            LOG_VERBOSE() << "Renderer thread exiting..." << std::endl;
        });

        renderingThread.detach();
    }

    void exit() {
        LOG_VERBOSE() << "Shutting down renderer..." << std::endl;
        shouldExit = true;  // Signal renderer thread to exit
        
        // Give the thread more time to exit gracefully
        // The renderer thread sleeps for up to 33ms per iteration, so we need at least that much time
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        if (currentFramebuffer) {
            currentFramebuffer->unmap();
            currentFramebuffer.reset();
        }
        
        font::manager::cleanup();
        display::manager::cleanup();
        rendererInitialized = false;
        
        LOG_VERBOSE() << "Renderer shutdown complete." << std::endl;
    }

    bool renderHandle(const window::handle* handle) {
        if (!rendererInitialized || !handle || !currentFramebuffer) {
            return false;
        }

        // Protect cellBuffer access with mutex
        std::lock_guard<std::mutex> lock(handle->cellBufferMutex);
        
        if (!handle->cellBuffer || handle->cellBuffer->empty()) {
            // Since we use non-blocking tcp's the buffer can still be in transit, so we can skip this turn.
            return false;
        }

        // Get cell coordinates for buffer size validation
        types::rectangle windowCellRect = handle->getCellCoordinates();
        // Get pixel coordinates for rendering position
        types::rectangle windowPixelRect = handle->getPixelCoordinates();

        LOG_VERBOSE() << "Rendering handle: Cell rect=" << windowCellRect.size.x << "x" << windowCellRect.size.y 
                      << ", Pixel rect=" << windowPixelRect.size.x << "x" << windowPixelRect.size.y 
                      << ", Buffer size=" << handle->cellBuffer->size() << std::endl;

        // Additional safety checks
        if (windowCellRect.size.x <= 0 || windowCellRect.size.y <= 0) {
            LOG_ERROR() << "Invalid cell rectangle size: " << windowCellRect.size.x << "x" << windowCellRect.size.y << std::endl;
            return false;
        }

        // Verify the buffer size matches the expected cell dimensions
        size_t expectedSize = static_cast<size_t>(windowCellRect.size.x) * static_cast<size_t>(windowCellRect.size.y);
        if (handle->cellBuffer->size() != expectedSize) {
            LOG_ERROR() << "Buffer size mismatch: expected " << expectedSize << " cells (" 
                        << windowCellRect.size.x << "x" << windowCellRect.size.y << "), got " 
                        << handle->cellBuffer->size() << std::endl;
            LOG_ERROR() << "Pixel coords: " << windowPixelRect.size.x << "x" << windowPixelRect.size.y << std::endl;
            LOG_ERROR() << "Cell coords: " << windowCellRect.size.x << "x" << windowCellRect.size.y << std::endl;
            return false;
        }

        // Clear the framebuffer region for this handle
        uint32_t* fbBuffer = static_cast<uint32_t*>(currentFramebuffer->getBuffer());
        if (!fbBuffer) {
            return false;
        }

        // Calculate cell dimensions based on font and zoom
        int baseCellWidth = font::manager::getDefaultCellWidth();
        int baseCellHeight = font::manager::getDefaultCellHeight();
        
        // Apply zoom factor
        int cellWidth = static_cast<int>(baseCellWidth * handle->zoom);
        int cellHeight = static_cast<int>(baseCellHeight * handle->zoom);
        
        LOG_VERBOSE() << "Cell dimensions: " << cellWidth << "x" << cellHeight 
                      << " (base: " << baseCellWidth << "x" << baseCellHeight << ", zoom: " << handle->zoom << ")" << std::endl;
        
        // Calculate total window dimensions in pixels
        int windowWidth = windowCellRect.size.x * cellWidth;
        int windowHeight = windowCellRect.size.y * cellHeight;
        
        LOG_VERBOSE() << "Window dimensions: " << windowWidth << "x" << windowHeight 
                      << " (from " << windowCellRect.size.x << "x" << windowCellRect.size.y << " cells)" << std::endl;
        
        // Ensure we don't exceed framebuffer boundaries
        int maxX = std::min(windowPixelRect.position.x + windowWidth, static_cast<int>(currentFramebuffer->getWidth()));
        int maxY = std::min(windowPixelRect.position.y + windowHeight, static_cast<int>(currentFramebuffer->getHeight()));
        
        LOG_VERBOSE() << "Framebuffer bounds: " << currentFramebuffer->getWidth() << "x" << currentFramebuffer->getHeight() 
                      << ", Window bounds: " << maxX << "x" << maxY << std::endl;
        
        bool didRender = false;
        int renderedCells = 0;

        auto font = handle->getFont();

        // Optimized cell cache using pre-converted XRGB8888 format (non-static to avoid sharing issues)
        OptimizedCellCache cellCache;
        cellCache.initialize(cellWidth, cellHeight);
        
        // Temporary buffer for font rendering (only used on cache miss)
        font::cellRenderData tempRenderBuffer{
            cellWidth,
            cellHeight,
            {}
        };
        tempRenderBuffer.pixels.resize(tempRenderBuffer.width * tempRenderBuffer.height);
        
        // Render each cell using cell coordinates for iteration
        for (int cellY = 0; cellY < windowCellRect.size.y; cellY++) {
            for (int cellX = 0; cellX < windowCellRect.size.x; cellX++) {
                int cellIndex = cellY * windowCellRect.size.x + cellX;
                
                // Bounds check for cell buffer access
                if (cellIndex < 0 || static_cast<size_t>(cellIndex) >= handle->cellBuffer->size()) {
                    LOG_ERROR() << "Cell index out of bounds: " << cellIndex << " (buffer size: " << handle->cellBuffer->size() << ")" << std::endl;
                    continue;
                }
                
                const types::Cell& cell = (*handle->cellBuffer)[cellIndex];
                
                // Calculate pixel position in framebuffer using pixel coordinates
                int pixelX = windowPixelRect.position.x + cellX * cellWidth;
                int pixelY = windowPixelRect.position.y + cellY * cellHeight;
                
                // Skip if out of bounds
                if (pixelX >= maxX || pixelY >= maxY) {
                    continue;
                }

                // Smart fingerprint-based lookup - O(1) cell comparison
                if (!cellCache.isValid || cellCache.cellID != cell) {
                    // Cache miss - need to render cell
                    std::fill(tempRenderBuffer.pixels.begin(), tempRenderBuffer.pixels.end(), cell.backgroundColor);
                    
                    if (font) {
                        tempRenderBuffer = font->renderCell(cell, tempRenderBuffer, handle->zoom);
                    }
                    
                    // Format pre-conversion - convert RGB to XRGB8888 during caching
                    cellCache.convertFromRGB(tempRenderBuffer);
                    cellCache.cellID = cell;
                }
                
                // Memory operation batching - fast framebuffer blitting
                blitCellToFramebuffer(fbBuffer, currentFramebuffer->getPitch() / sizeof(uint32_t), 
                                    currentFramebuffer->getHeight(), pixelX, pixelY, cellCache);
                didRender = true;
                renderedCells++;
            }
        }
        
        LOG_VERBOSE() << "Rendered " << renderedCells << " cells out of " << handle->cellBuffer->size() << std::endl;
        
        return didRender;
    }
    
    // Legacy function for backward compatibility
    void render(const window::handle* handle) {
        renderHandle(handle);
    }
    
    void renderCellToFramebuffer(uint32_t* fbBuffer, int fbWidth, int fbHeight, int startX, int startY, const font::cellRenderData& cellData) {
        // Legacy function - kept for backward compatibility
        // For better performance, use the optimized caching system in renderHandle()
        
        // Bounds checking
        if (startX >= fbWidth || startY >= fbHeight) {
            return;
        }
        
        const int maxCopyWidth = std::min(cellData.width, fbWidth - startX);
        const int maxCopyHeight = std::min(cellData.height, fbHeight - startY);
        
        // Optimize by using row-based copying when possible
        if (maxCopyWidth == cellData.width) {
            // Can copy entire rows efficiently
            for (int y = 0; y < maxCopyHeight; y++) {
                const int srcOffset = y * cellData.width;
                const int dstOffset = (startY + y) * fbWidth + startX;
                
                // Convert RGB to XRGB8888 for the entire row
                for (int x = 0; x < maxCopyWidth; x++) {
                    const types::RGB& pixel = cellData.pixels[srcOffset + x];
                    fbBuffer[dstOffset + x] = (pixel.r << 16) | (pixel.g << 8) | pixel.b;
                }
            }
        } else {
            // Fallback to pixel-by-pixel for partial rows
            for (int y = 0; y < maxCopyHeight; y++) {
                for (int x = 0; x < maxCopyWidth; x++) {
                    int fbX = startX + x;
                    int fbY = startY + y;
                    
                    int cellPixelIndex = y * cellData.width + x;
                    int fbPixelIndex = fbY * fbWidth + fbX;
                    
                    if (static_cast<size_t>(cellPixelIndex) < cellData.pixels.size()) {
                        const types::RGB& pixel = cellData.pixels[cellPixelIndex];
                        fbBuffer[fbPixelIndex] = (pixel.r << 16) | (pixel.g << 8) | pixel.b;
                    }
                }
            }
        }
    }

}