/*
 * Window Display Management System
 * 
 * This module provides professional display management for window handles.
 * 
 * Key improvements over the original hardcoded approach:
 * 
 * 1. Dynamic Display Detection:
 *    - Automatically detects and works with multiple displays
 *    - Handles display hotplug events gracefully
 *    - Fallback to primary display when requested display is unavailable
 * 
 * 2. Professional Architecture:
 *    - Each handle tracks its associated display ID
 *    - Utility functions for display validation and resolution querying
 *    - Multiple display assignment strategies (round-robin, primary-only, etc.)
 * 
 * 3. Enhanced Error Handling:
 *    - Graceful handling of no-display scenarios
 *    - Automatic migration of handles when displays are disconnected
 *    - Comprehensive logging for debugging
 * 
 * 4. Flexible API:
 *    - Backward compatible with existing code
 *    - New handle-based positioning for better context
 *    - Utility functions for display management
 * 
 * Usage Examples:
 * 
 *   // Get coordinates for a handle (uses handle's assigned display)
 *   types::rectangle coords = handle.getCoordinates();
 *   
 *   // Position on specific display
 *   types::rectangle coords = positionToCoordinates(position::LEFT, displayId);
 *   
 *   // Distribute handles across displays
 *   window::manager::assignDisplaysToHandles(window::manager::DisplayAssignmentStrategy::ROUND_ROBIN);
 *   
 *   // Move a handle to a specific display
 *   window::manager::moveHandleToDisplay(&handle, displayId);
 */

#include "window.h"
#include "input.h"
#include "display.h"
#include "font.h"
#include "logger.h"

#include <cstdio>
#include <cstdint>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <atomic>
#include <chrono>
#include <fcntl.h>

namespace window {

    types::rectangle positionToCoordinates(position pos, const handle& windowHandle) {
        // Keep backward compatibility - this function returns cell coordinates
        return positionToCellCoordinates(pos, windowHandle.getDisplayId());
    }

    types::rectangle positionToCoordinates(position pos, uint32_t displayId) {
        // Keep backward compatibility - this function returns cell coordinates
        return positionToCellCoordinates(pos, displayId);
    }

    types::rectangle positionToPixelCoordinates(position pos, const handle& windowHandle) {
        return positionToPixelCoordinates(pos, windowHandle.getDisplayId());
    }

    types::rectangle positionToPixelCoordinates(position pos, uint32_t displayId) {
        // Get the display resolution dynamically based on the provided display ID
        types::iVector2 currentDisplayResolution;
        
        // Check if we have any active displays
        if (display::manager::activeDisplays.empty()) {
            LOG_ERROR() << "No active displays available for window positioning" << std::endl;
            // Return a default small window if no displays are available
            return {{0, 0}, {800, 600}};
        }
        
        // Get the resolution for the requested display
        currentDisplayResolution = getDisplayResolution(displayId);
        
        // Log if we're falling back to a different display
        if (displayId != 0 && !isValidDisplayId(displayId)) {
            LOG_ERROR() << "Display ID " << displayId << " not found, using primary display" << std::endl;
        }

        // Calculate position based on the preset (in pixels)
        if (pos == position::FULLSCREEN) {
            return {{0, 0}, currentDisplayResolution};
        } 
        else if (pos == position::LEFT) {
            return {{0, 0}, {currentDisplayResolution.x / 2, currentDisplayResolution.y}};
        } 
        else if (pos == position::RIGHT) {
            return {{currentDisplayResolution.x / 2, 0}, {currentDisplayResolution.x / 2, currentDisplayResolution.y}};
        } 
        else if (pos == position::TOP) {
            return {{0, 0}, {currentDisplayResolution.x, currentDisplayResolution.y / 2}};
        } 
        else if (pos == position::BOTTOM) {
            return {{0, currentDisplayResolution.y / 2}, {currentDisplayResolution.x, currentDisplayResolution.y / 2}};
        } 
        else {
            throw std::invalid_argument("Invalid position type");
        }
    }

    types::rectangle positionToCellCoordinates(position pos, const handle& windowHandle) {
        return positionToCellCoordinates(pos, windowHandle.getDisplayId());
    }

    types::rectangle positionToCellCoordinates(position pos, uint32_t displayId) {
        // Get pixel coordinates first
        types::rectangle pixelRect = positionToPixelCoordinates(pos, displayId);
        
        // Convert to cell coordinates
        int cellWidth = font::manager::getDefaultCellWidth();
        int cellHeight = font::manager::getDefaultCellHeight();
        
        if (cellWidth <= 0 || cellHeight <= 0) {
            LOG_ERROR() << "Invalid cell dimensions: " << cellWidth << "x" << cellHeight << std::endl;
            // Return a reasonable default in cells
            return {{0, 0}, {80, 24}};
        }
        
        types::rectangle cellRect;
        cellRect.position.x = pixelRect.position.x / cellWidth;
        cellRect.position.y = pixelRect.position.y / cellHeight;
        cellRect.position.z = pixelRect.position.z;
        cellRect.size.x = pixelRect.size.x / cellWidth;
        cellRect.size.y = pixelRect.size.y / cellHeight;
        
        LOG_VERBOSE() << "Position " << static_cast<int>(pos) 
                      << " -> Pixel: " << pixelRect.size.x << "x" << pixelRect.size.y 
                      << " -> Cell: " << cellRect.size.x << "x" << cellRect.size.y 
                      << " (cell size: " << cellWidth << "x" << cellHeight << ")" << std::endl;
        
        return cellRect;
    }

    void handle::poll() {
        // Make sure the connection is non-blocking
        if (!connection.hasDataAvailable()) {
            // No data available, return immediately to avoid blocking
            return;
        }

        // Now we can send the first packet to GGUI client, and it is the size of it at fullscreen.
        types::rectangle windowRectangle = positionToCellCoordinates(preset, displayId);

        types::iVector2 dimensionsInCells = {
            windowRectangle.size.x,
            windowRectangle.size.y
        };

        unsigned int requiredSize = dimensionsInCells.x * dimensionsInCells.y;

        if (requiredSize != cellBuffer->size()) {
            // Resize the buffer to match the required size
            cellBuffer->resize(requiredSize);
            LOG_VERBOSE() << "Resized cell buffer to " << requiredSize << " cells (" 
                          << dimensionsInCells.x << "x" << dimensionsInCells.y << ")" << std::endl;
        }

        // Debug: Show current buffer state
        LOG_VERBOSE() << "Poll: Buffer size=" << cellBuffer->size() 
                      << ", Required=" << requiredSize 
                      << ", Dimensions=" << dimensionsInCells.x << "x" << dimensionsInCells.y << std::endl;

        // Now we'll get the buffer data from the GGUI client.
        if (!cellBuffer || cellBuffer->empty()) {
            LOG_ERROR() << "Cell buffer is invalid or empty" << std::endl;
            errorCount++;
            return;
        }

        // Try to receive a packet header first
        char packetBuffer[packet::size];
        
        if (!connection.ReceivePacketNonBlocking(packetBuffer, packet::size)) {
            // No complete packet available, return without error
            return;
        }

        // Cast to base packet to check type
        packet::base* basePacket = reinterpret_cast<packet::base*>(packetBuffer);
        
        LOG_VERBOSE() << "Received packet type: " << static_cast<int>(basePacket->packetType) << std::endl;
        
        if (basePacket->packetType == packet::type::NOTIFY) {
            // Cast to notify packet
            packet::notify::base* notifyPacket = reinterpret_cast<packet::notify::base*>(packetBuffer);

            if (notifyPacket->notifyType == packet::notify::type::EMPTY_BUFFER) {
                // If the buffer is empty, we can skip receiving the cell data
                LOG_VERBOSE() << "Received empty buffer notification, skipping frame" << std::endl;
                return;  // Skip to the next handle
            } 
            else if (notifyPacket->notifyType == packet::notify::type::CLOSED) {
                LOG_VERBOSE() << "Received closed notification, shutting down connection" << std::endl;
                connection.close();
                return;  // Skip to the next handle
            } else {
                LOG_ERROR() << "Unknown notify flag received: " << static_cast<int>(notifyPacket->notifyType) << std::endl;
                errorCount++;
                return;  // Skip to the next handle
            }
        }
        else if (basePacket->packetType == packet::type::DRAW_BUFFER) {
            // This is a draw buffer packet, the cell data follows immediately after the packet header
            // We need to receive the cell buffer data as a separate packet to avoid buffer mixing issues
            if (!connection.ReceivePacketNonBlocking(cellBuffer->data(), cellBuffer->size())) {
                // Cell buffer data not ready yet, return without error
                LOG_VERBOSE() << "Draw buffer packet header received, but cell data not ready yet" << std::endl;
                return;
            }
            LOG_VERBOSE() << "Successfully received draw buffer with " << cellBuffer->size() << " cells" << std::endl;
        }
        else if (basePacket->packetType == packet::type::INPUT) {
            // Input packets are handled elsewhere, ignore them here
            LOG_VERBOSE() << "Received INPUT packet in handle poll (should be handled by input system)" << std::endl;
            return;
        }
        else if (basePacket->packetType == packet::type::RESIZE) {
            // Resize packets should be handled elsewhere, ignore them here  
            LOG_VERBOSE() << "Received RESIZE packet in handle poll (should be handled separately)" << std::endl;
            return;
        }
        else {
            LOG_ERROR() << "Unknown packet type received: " << static_cast<int>(basePacket->packetType) << " (raw bytes: " << std::hex;
            for (int i = 0; i < 8 && i < packet::size; i++) {
                LOG_ERROR() << " 0x" << static_cast<unsigned char>(packetBuffer[i]);
            }
            LOG_ERROR() << std::dec << ")" << std::endl;
            errorCount++;
            return;
        }

        // Set the errorCount to zero if everything worked.
        errorCount = 0;
    }

    font::font* handle::getFont() const {
        if (customFont)
            return customFont.get();
        
        auto font = font::manager::getFont(font::manager::configurableFontName);

        if (!font) {
            font = font::manager::getDefaultFont();
        }

        return font.get();
    }


    namespace manager {
        // Define the global variables
        atomic::guard<std::vector<handle>> handles;
        atomic::guard<tcp::listener> listener;
        
        // Shutdown control
        std::atomic<bool> shouldShutdown{false};
        
        // Focus management for input system
        static handle* currentFocusedHandle = nullptr;
        
        const char* handshakeInitializedFileName = "/tmp/GGDirect.gateway";  // This file will contain the port this manager is listening at

        // Sets up the secondary thread which is responsible for the handshakes
        void init() {
            uint32_t uniquePort;

            try {
                // init global listener and get a unique port number
                listener([&uniquePort](tcp::listener& self){
                    self = tcp::listener(0); // Create a listener on port 0 to get a unique port number

                    uniquePort = self.getPort();
                });

                // Now we will need to write the port number into the file
                FILE* file = fopen(handshakeInitializedFileName, "w");
                if (file) {
                    fprintf(file, "%u", uniquePort);
                    fclose(file);
                } else {
                    perror("Failed to open handshake file for writing");
                    return;
                }

                // Now we will start listening for connections
                std::thread reception = std::thread([]() {
                    while (!shouldShutdown.load()) {
                        try {
                            // Use the atomic guard to safely access the listener and get a connection
                            listener([](tcp::listener& listenerRef){
                                // Set listener socket to non-blocking mode to allow checking shutdown flag
                                int listenerFd = listenerRef.getHandle();
                                if (listenerFd >= 0) {
                                    int flags = fcntl(listenerFd, F_GETFL, 0);
                                    if (flags >= 0) {
                                        fcntl(listenerFd, F_SETFL, flags | O_NONBLOCK);
                                    }
                                }
                                
                                LOG_VERBOSE() << "Waiting for GGUI client connection..." << std::endl;
                                
                                try {
                                    tcp::connection conn = listenerRef.Accept();
                                    LOG_VERBOSE() << "Accepted connection from GGUI client" << std::endl;
                                    
                                    // Process the connection normally...
                                    // We will first listen for GGUI to give its own port to establish an personal connection to this specific GGUI client
                                uint16_t gguiPort;
                                if (!conn.Receive(&gguiPort)) {
                                    LOG_ERROR() << "Failed to receive GGUI port" << std::endl;
                                    return;
                                }

                                LOG_VERBOSE() << "Received GGUI port: " << gguiPort << std::endl;

                                // Now we can create a new connection with this new port
                                tcp::connection gguiConnection = tcp::sender::getConnection(gguiPort);
                                
                                LOG_VERBOSE() << "Established connection to GGUI on port " << gguiPort << std::endl;
                                
                                // Set the connection to non-blocking mode for better performance
                                if (!gguiConnection.setNonBlocking()) {
                                    LOG_ERROR() << "Warning: Failed to set connection to non-blocking mode" << std::endl;
                                }

                                // Before going to the next connection, we need to send confirmation back to the GGUI that we have accepted the connection
                                if (!gguiConnection.Send(&gguiPort)) {
                                    LOG_ERROR() << "Failed to send confirmation to GGUI" << std::endl;
                                    return;
                                }

                                LOG_VERBOSE() << "Sent confirmation to GGUI" << std::endl;

                                // Now we can send the first packet to GGUI client, and it is the size of it at fullscreen.
                                types::rectangle windowRectangle = positionToCellCoordinates(position::FULLSCREEN, getPrimaryDisplayId());

                                types::iVector2 dimensionsInCells = {
                                    windowRectangle.size.x,
                                    windowRectangle.size.y
                                };

                                LOG_VERBOSE() << "Sending initial dimensions to GGUI client: " 
                                          << dimensionsInCells.x << "x" << dimensionsInCells.y << " cells" << std::endl;

                                // Create resize packet with data
                                char packetBuffer[packet::size];
                                packet::resize::base newsize{types::sVector2{dimensionsInCells}};
                                // we write the inform into the packet buffer
                                memcpy(packetBuffer, &newsize, sizeof(newsize));

                                // Send the resize packet
                                if (!gguiConnection.Send(packetBuffer, packet::size)) {
                                    LOG_ERROR() << "Failed to send resize packet to GGUI" << std::endl;
                                    return;
                                }

                                // Create a new handle for this connection
                                handles([&gguiConnection](std::vector<handle>& self){
                                    self.emplace_back(std::move(gguiConnection));
                                    
                                    // Set the display ID for the new handle to the primary display
                                    auto& newHandle = self.back();
                                    newHandle.setDisplayId(getPrimaryDisplayId());
                                    
                                    // Verify that the handle has the same dimensions as what we sent
                                    types::rectangle testRect = newHandle.getCellCoordinates();
                                    LOG_VERBOSE() << "New handle cell coordinates: " << testRect.size.x << "x" << testRect.size.y << std::endl;
                                    
                                    // Set the first handle as focused by default
                                    if (self.size() == 1) {
                                        setFocusedHandle(&self[0]);
                                        LOG_VERBOSE() << "Set first handle as focused for input." << std::endl;
                                    }
                                    
                                    logger::info("Created GGUI connection on display " + std::to_string(newHandle.getDisplayId()));
                                });
                                } catch (const std::runtime_error& e) {
                                    // This is expected when no connections are pending (non-blocking mode)
                                    // We'll just continue and check again in the next iteration
                                    std::string error_msg = e.what();
                                    if (error_msg.find("Failed to accept connection") != std::string::npos) {
                                        // No connection pending, this is normal in non-blocking mode
                                        LOG_VERBOSE() << "No pending connections, continuing..." << std::endl;
                                        return;
                                    } else {
                                        // Some other error, log it
                                        LOG_ERROR() << "Unexpected error in connection handling: " << e.what() << std::endl;
                                        return;
                                    }
                                }
                            });
                        } catch (const std::exception& e) {
                            LOG_ERROR() << "Error in reception thread: " << e.what() << std::endl;
                            continue;
                        }
                        
                        // Small sleep to prevent busy waiting when no connections are pending
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    LOG_VERBOSE() << "Reception thread exiting..." << std::endl;
                });

                reception.detach(); // Let reception retain after this local scope has ended.
            } catch (const std::exception& e) {
                LOG_ERROR() << "Failed to initialize window manager: " << e.what() << std::endl;
                throw;
            }
        }

        void close() {
            LOG_VERBOSE() << "Shutting down window manager..." << std::endl;
            
            // Signal all threads to stop
            shouldShutdown.store(true);
            
            // Give threads time to exit gracefully
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Close all handles
            handles([](std::vector<handle>& self){
                for (auto& h : self) {
                    h.close(); // Close each connection
                }
                self.clear(); // Clear the vector
            });
            
            // listener destructor will be called automatically via atomic::guard
            LOG_VERBOSE() << "Window manager shutdown complete." << std::endl;
        }
        
        // Focus management functions
        void setFocusedHandle(handle* focusedHandle) {
            currentFocusedHandle = focusedHandle;
            // Update the input system's focused handle
            input::manager::setFocusedHandle(static_cast<void*>(focusedHandle));
        }
        
        handle* getFocusedHandle() {
            return currentFocusedHandle;
        }
        
        void setFocusedHandleByIndex(size_t index) {
            handles([index](std::vector<handle>& self) {
                if (index < self.size()) {
                    setFocusedHandle(&self[index]);
                }
            });
        }
        
        size_t getActiveHandleCount() {
            size_t count = 0;
            handles([&count](std::vector<handle>& self) {
                count = self.size();
            });
            return count;
        }
        
        // Display management functions
        void distributeHandlesAcrossDisplays() {
            handles([](std::vector<handle>& self) {
                if (self.empty() || display::manager::activeDisplays.empty()) {
                    return;
                }
                
                // Get available display IDs
                std::vector<uint32_t> displayIds;
                for (const auto& display : display::manager::activeDisplays) {
                    displayIds.push_back(display.first);
                }
                
                // Distribute handles across displays
                for (size_t i = 0; i < self.size(); ++i) {
                    uint32_t displayId = displayIds[i % displayIds.size()];
                    self[i].setDisplayId(displayId);
                    LOG_VERBOSE() << "Moved handle " << i << " to display " << displayId << std::endl;
                }
            });
        }
        
        void moveHandleToDisplay(handle* windowHandle, uint32_t displayId) {
            if (!windowHandle) {
                LOG_ERROR() << "Cannot move null handle to display" << std::endl;
                return;
            }
            
            // Check if the display exists
            if (display::manager::activeDisplays.find(displayId) == display::manager::activeDisplays.end()) {
                LOG_ERROR() << "Display ID " << displayId << " not found" << std::endl;
                return;
            }
            
            windowHandle->setDisplayId(displayId);
            LOG_VERBOSE() << "Moved handle to display " << displayId << std::endl;
        }
        
        std::vector<uint32_t> getAvailableDisplayIds() {
            std::vector<uint32_t> displayIds;
            for (const auto& display : display::manager::activeDisplays) {
                displayIds.push_back(display.first);
            }
            return displayIds;
        }

        // Display assignment strategies
        void assignDisplaysToHandles(DisplayAssignmentStrategy strategy) {
            handles([strategy](std::vector<handle>& self) {
                if (self.empty() || display::manager::activeDisplays.empty()) {
                    return;
                }
                
                std::vector<uint32_t> displayIds = getAvailableDisplayIds();
                if (displayIds.empty()) {
                    return;
                }
                
                switch (strategy) {
                    case DisplayAssignmentStrategy::ROUND_ROBIN:
                        for (size_t i = 0; i < self.size(); ++i) {
                            uint32_t displayId = displayIds[i % displayIds.size()];
                            self[i].setDisplayId(displayId);
                        }
                        break;
                        
                    case DisplayAssignmentStrategy::PRIMARY_ONLY:
                        for (auto& handle : self) {
                            handle.setDisplayId(getPrimaryDisplayId());
                        }
                        break;
                        
                    case DisplayAssignmentStrategy::FILL_THEN_NEXT:
                        // This could be expanded to consider screen real estate
                        // For now, just do round-robin
                        for (size_t i = 0; i < self.size(); ++i) {
                            uint32_t displayId = displayIds[i % displayIds.size()];
                            self[i].setDisplayId(displayId);
                        }
                        break;
                }
                
                LOG_VERBOSE() << "Assigned displays to " << self.size() << " handles using strategy " 
                              << static_cast<int>(strategy) << std::endl;
            });
        }
        
        // Display monitoring and updates
        void updateHandleDisplays() {
            handles([](std::vector<handle>& self) {
                for (auto& handle : self) {
                    // Check if the handle's display still exists
                    if (!isValidDisplayId(handle.getDisplayId())) {
                        LOG_VERBOSE() << "Handle's display " << handle.getDisplayId() 
                                      << " is no longer available, moving to primary display" << std::endl;
                        handle.setDisplayId(getPrimaryDisplayId());
                    }
                }
            });
        }
        
        // Get information about handle distribution across displays
        std::map<uint32_t, size_t> getHandleDistribution() {
            std::map<uint32_t, size_t> distribution;
            handles([&distribution](std::vector<handle>& self) {
                for (const auto& handle : self) {
                    distribution[handle.getDisplayId()]++;
                }
            });
            return distribution;
        }
        
        // Debug function to print current handle-display mapping
        void printHandleDisplayMapping() {
            LOG_VERBOSE() << "=== Handle-Display Mapping ===" << std::endl;
            handles([](std::vector<handle>& self) {
                for (size_t i = 0; i < self.size(); ++i) {
                    LOG_VERBOSE() << "Handle " << i << " -> Display " 
                                  << self[i].getDisplayId() << std::endl;
                }
            });
            
            auto distribution = getHandleDistribution();
            LOG_VERBOSE() << "=== Display Distribution ===" << std::endl;
            for (const auto& pair : distribution) {
                LOG_VERBOSE() << "Display " << pair.first << ": " << pair.second << " handles" << std::endl;
            }
        }
    }

    // Helper function to get the primary display ID
    uint32_t getPrimaryDisplayId() {
        if (display::manager::activeDisplays.empty()) {
            return 0;
        }
        return display::manager::activeDisplays.begin()->first;
    }

    // Helper function to validate display ID
    bool isValidDisplayId(uint32_t displayId) {
        return display::manager::activeDisplays.find(displayId) != display::manager::activeDisplays.end();
    }

    // Helper function to get display resolution safely
    types::iVector2 getDisplayResolution(uint32_t displayId) {
        auto displayIt = display::manager::activeDisplays.find(displayId);
        if (displayIt != display::manager::activeDisplays.end()) {
            return displayIt->second->getPreferredMode().getResolution();
        }
        
        // Fallback to first available display
        if (!display::manager::activeDisplays.empty()) {
            return display::manager::activeDisplays.begin()->second->getPreferredMode().getResolution();
        }
        
        // Last resort default resolution
        return {1920, 1080};
    }
}