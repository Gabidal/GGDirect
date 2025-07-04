#include "renderer.h"
#include "types.h"
#include "display.h"
#include "font.h"

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
        std::cout << "Renderer initialized successfully" << std::endl;
        
        // Start rendering thread
        std::thread renderingThread([](){
            while (!shouldExit) {
                window::manager::handles([](std::vector<window::handle>& self){
                    // First we'll need to order the handles, so that rendering order is correct, where lower z's get drawn first to be overdrawn.
                    std::sort(self.begin(), self.end(), [](const window::handle& a, const window::handle& b) {
                        return a.position.z < b.position.z;  // Sort by z position
                    });

                    // Receive cell buffers
                    for (int i = 0; i < self.size(); i++) {
                        if (self[i].errorCount > window::handle::maxAllowedErrorCount) {
                            // This handle has given us way too much problems, remove it
                            self.erase(self.begin() + i);
                            i--;  // Decrement i to account for the removed element
                        }

                        self[i].poll();
                    }

                    // Render gotten cell buffers.
                    for (auto& handle : self) {
                        render(&handle);
                    }

                });

                std::this_thread::sleep_for(std::chrono::milliseconds(16));  // cap at 60fps
            }
            std::cout << "Renderer thread exiting..." << std::endl;
        });

        renderingThread.detach();
    }

    void exit() {
        std::cout << "Shutting down renderer..." << std::endl;
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

    void render(const window::handle* handle) {
        if (!rendererInitialized || !handle || !handle->cellBuffer || !currentFramebuffer) {
            return;
        }

        // Clear the framebuffer region for this handle
        uint32_t* fbBuffer = static_cast<uint32_t*>(currentFramebuffer->getBuffer());
        if (!fbBuffer) {
            return;
        }

        // Calculate cell dimensions based on font and zoom
        int baseCellWidth = font::manager::getDefaultCellWidth();
        int baseCellHeight = font::manager::getDefaultCellHeight();
        
        // Apply zoom factor
        int cellWidth = static_cast<int>(baseCellWidth * handle->zoom);
        int cellHeight = static_cast<int>(baseCellHeight * handle->zoom);
        
        // Calculate total window dimensions
        int windowWidth = handle->size.x * cellWidth;
        int windowHeight = handle->size.y * cellHeight;
        
        // Ensure we don't exceed framebuffer boundaries
        int maxX = std::min(handle->position.x + windowWidth, static_cast<int>(currentFramebuffer->getWidth()));
        int maxY = std::min(handle->position.y + windowHeight, static_cast<int>(currentFramebuffer->getHeight()));
        
        // Render each cell
        for (int cellY = 0; cellY < handle->size.y; cellY++) {
            for (int cellX = 0; cellX < handle->size.x; cellX++) {
                int cellIndex = cellY * handle->size.x + cellX;
                
                if (static_cast<size_t>(cellIndex) >= handle->cellBuffer->size()) {
                    continue;
                }
                
                const types::Cell& cell = (*handle->cellBuffer)[cellIndex];
                
                // Calculate pixel position in framebuffer
                int pixelX = handle->position.x + cellX * cellWidth;
                int pixelY = handle->position.y + cellY * cellHeight;
                
                // Skip if out of bounds
                if (pixelX >= maxX || pixelY >= maxY) {
                    continue;
                }
                
                // Render the cell using font system
                font::cellRenderData cellData = font::manager::renderCell(cell, baseCellWidth, baseCellHeight, handle->zoom);
                
                // Copy cell pixels to framebuffer
                renderCellToFramebuffer(fbBuffer, currentFramebuffer->getWidth(), currentFramebuffer->getHeight(),
                                      pixelX, pixelY, cellData);
            }
        }
        
        // Present the framebuffer
        display::manager::present(primaryConnector, currentFramebuffer);
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