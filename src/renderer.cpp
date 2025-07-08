#include "renderer.h"
#include "types.h"
#include "display.h"
#include "font.h"
#include "logger.h"

#include <thread>
#include <iostream>
#include <memory>
#include <algorithm>
#include <chrono>

namespace renderer {
    
    // Renderer state
    static std::shared_ptr<display::frameBuffer> currentFramebuffer;
    static std::shared_ptr<display::connector> primaryConnector;
    static std::shared_ptr<display::crtc> primaryCrtc;
    static std::shared_ptr<display::plane> primaryPlane;
    static std::unique_ptr<display::mode> currentMode;
    static bool rendererInitialized = false;
    static bool shouldExit = false;  // Flag to control renderer thread exit
    
    // Forward declaration
    void renderCellToFramebuffer(uint32_t* fbBuffer, int fbWidth, int fbHeight,
                                int startX, int startY, const font::cellRenderData& cellData);
    
    // Initialize display and font systems
    void init() {
        // Initialize display system
        if (!display::manager::initialize()) {
            std::cerr << "Failed to initialize display system" << std::endl;
            return;
        }
        
        // Initialize font system
        if (!font::manager::initialize()) {
            std::cerr << "Failed to initialize font system" << std::endl;
            return;
        }
        
        // Set up display resources
        auto availableDisplays = display::manager::getAvailableDisplays();
        if (availableDisplays.empty()) {
            std::cerr << "No available displays found" << std::endl;
            return;
        }
        
        // Use the first available display
        primaryConnector = availableDisplays[0];
        if (!primaryConnector->isConnected()) {
            std::cerr << "Primary display is not connected" << std::endl;
            return;
        }
        
        // Get preferred mode
        auto modes = primaryConnector->getAvailableModes();
        if (modes.empty()) {
            std::cerr << "No available modes for primary display" << std::endl;
            return;
        }
        
        currentMode = std::make_unique<display::mode>(primaryConnector->getPreferredMode());
        
        // Enable the display
        if (!display::manager::enableDisplay(primaryConnector, *currentMode)) {
            std::cerr << "Failed to enable primary display" << std::endl;
            return;
        }
        
        // Create framebuffer
        currentFramebuffer = display::manager::createFramebuffer(
            currentMode->getWidth(), 
            currentMode->getHeight(), 
            0x34325258  // DRM_FORMAT_XRGB8888
        );
        
        if (!currentFramebuffer) {
            std::cerr << "Failed to create framebuffer" << std::endl;
            return;
        }
        
        // Map the framebuffer for CPU access
        if (!currentFramebuffer->map()) {
            std::cerr << "Failed to map framebuffer" << std::endl;
            return;
        }
        
        rendererInitialized = true;
        
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

                    // Receive cell buffers and remove problematic handles
                    for (int i = static_cast<int>(self.size()) - 1; i >= 0; i--) {
                        if (self[i].connection.isClosed()) {
                            // If the connection is closed, remove the handle
                            std::cerr << "Removing handle " << i << " due to closed connection" << std::endl;
                            self.erase(self.begin() + i);
                            continue;
                        }

                        if (self[i].errorCount > window::handle::maxAllowedErrorCount) {
                            // This handle has given us way too many problems, remove it
                            std::cerr << "Removing handle " << i << " due to excessive errors (" << 
                                         self[i].errorCount << " > " << window::handle::maxAllowedErrorCount << ")" << std::endl;
                            self.erase(self.begin() + i);
                            continue;
                        }

                        // Safely poll the handle (now non-blocking)
                        self[i].poll();
                    }

                    // Render gotten cell buffers.
                    for (auto& handle : self) {
                        if (renderHandle(&handle)) {
                            needsPresent = true;
                        }
                    }

                });

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
                    const auto targetFrameTime = std::chrono::milliseconds(8);
                    if (frameTime < targetFrameTime) {
                        std::this_thread::sleep_for(targetFrameTime - frameTime);
                    }
                } else {
                    // If nothing was rendered, sleep longer to reduce CPU usage (30 FPS = ~33ms)
                    const auto idleFrameTime = std::chrono::milliseconds(33);
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
        
        // Give the thread a moment to exit
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (currentFramebuffer) {
            currentFramebuffer->unmap();
        }
        font::manager::cleanup();
        display::manager::cleanup();
        rendererInitialized = false;
    }

    bool renderHandle(const window::handle* handle) {
        if (!rendererInitialized || !handle || !handle->cellBuffer || !currentFramebuffer) {
            return false;
        }

        if (handle->cellBuffer->empty()) {
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
            std::cerr << "Invalid cell rectangle size: " << windowCellRect.size.x << "x" << windowCellRect.size.y << std::endl;
            return false;
        }

        // Verify the buffer size matches the expected cell dimensions
        size_t expectedSize = static_cast<size_t>(windowCellRect.size.x) * static_cast<size_t>(windowCellRect.size.y);
        if (handle->cellBuffer->size() != expectedSize) {
            std::cerr << "Buffer size mismatch: expected " << expectedSize << " cells (" 
                      << windowCellRect.size.x << "x" << windowCellRect.size.y << "), got " 
                      << handle->cellBuffer->size() << std::endl;
            std::cerr << "Pixel coords: " << windowPixelRect.size.x << "x" << windowPixelRect.size.y << std::endl;
            std::cerr << "Cell coords: " << windowCellRect.size.x << "x" << windowCellRect.size.y << std::endl;
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
        
        // Render each cell using cell coordinates for iteration
        for (int cellY = 0; cellY < windowCellRect.size.y; cellY++) {
            for (int cellX = 0; cellX < windowCellRect.size.x; cellX++) {
                int cellIndex = cellY * windowCellRect.size.x + cellX;
                
                // Bounds check for cell buffer access
                if (cellIndex < 0 || static_cast<size_t>(cellIndex) >= handle->cellBuffer->size()) {
                    std::cerr << "Cell index out of bounds: " << cellIndex << " (buffer size: " << handle->cellBuffer->size() << ")" << std::endl;
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
                
                // Render the cell using font system
                font::cellRenderData cellData = font::manager::renderCell(cell, baseCellWidth, baseCellHeight, handle->zoom);
                
                // Copy cell pixels to framebuffer
                renderCellToFramebuffer(fbBuffer, currentFramebuffer->getPitch() / sizeof(uint32_t), currentFramebuffer->getHeight(), pixelX, pixelY, cellData);
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
    
    void renderCellToFramebuffer(uint32_t* fbBuffer, int fbWidth, int fbHeight,
                                int startX, int startY, const font::cellRenderData& cellData) {
        
        for (int y = 0; y < cellData.height; y++) {
            for (int x = 0; x < cellData.width; x++) {
                int fbX = startX + x;
                int fbY = startY + y;
                
                // Bounds check
                if (fbX >= fbWidth || fbY >= fbHeight) {
                    continue;
                }
                
                int cellPixelIndex = y * cellData.width + x;
                int fbPixelIndex = fbY * fbWidth + fbX;
                
                if (static_cast<size_t>(cellPixelIndex) < cellData.pixels.size()) {
                    const types::RGB& pixel = cellData.pixels[cellPixelIndex];
                    
                    // Convert RGB to XRGB8888 format
                    uint32_t color = (pixel.r << 16) | (pixel.g << 8) | pixel.b;
                    fbBuffer[fbPixelIndex] = color;
                }
            }
        }
    }

}