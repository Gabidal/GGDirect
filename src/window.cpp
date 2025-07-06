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

#include <cstdio>
#include <cstdint>
#include <thread>
#include <stdexcept>
#include <iostream>

namespace window {

    types::rectangle positionToCoordinates(position pos, const handle& windowHandle) {
        return positionToCoordinates(pos, windowHandle.getDisplayId());
    }

    types::rectangle positionToCoordinates(position pos, uint32_t displayId) {
        // Get the display resolution dynamically based on the provided display ID
        types::iVector2 currentDisplayResolution;
        
        // Check if we have any active displays
        if (display::manager::activeDisplays.empty()) {
            std::cerr << "No active displays available for window positioning" << std::endl;
            // Return a default small window if no displays are available
            return {{0, 0}, {800, 600}};
        }
        
        // Get the resolution for the requested display
        currentDisplayResolution = getDisplayResolution(displayId);
        
        // Log if we're falling back to a different display
        if (displayId != 0 && !isValidDisplayId(displayId)) {
            std::cerr << "Display ID " << displayId << " not found, using primary display" << std::endl;
        }

        // Calculate position based on the preset
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

    void handle::poll() {
        // Now we'll get the buffer data from the GGUI client.
        if (!cellBuffer || cellBuffer->empty()) {
            std::cerr << "Cell buffer is invalid or empty" << std::endl;
            errorCount++;
            return;
        }

        packet::type incomingType;

        if (!connection.Receive(&incomingType)) {
            std::cerr << "Failed to receive packet type from GGUI client" << std::endl;
            errorCount++;
            return;  // Skip to the next handle
        }

        if (incomingType == packet::type::NOTIFY) {
            // receive the notify packet
            packet::notify flags;

            if (!connection.Receive(&flags)) {
                std::cerr << "Failed to receive notify flags from GGUI client" << std::endl;
                errorCount++;
                return;  // Skip to the next handle
            }

            if (flags == packet::notify::EMPTY_BUFFER) {
                // If the buffer is empty, we can skip receiving the cell data
                std::cout << "Received empty buffer, skipping frame" << std::endl;
                return;  // Skip to the next handle
            } else {
                std::cerr << "Unknown notify flag received: " << static_cast<int>(flags) << std::endl;
                errorCount++;
                return;  // Skip to the next handle
            }
        }
        else {
            if (!connection.Receive(cellBuffer->data(), cellBuffer->size())) {
                std::cerr << "Failed to receive buffer data from GGUI client" << std::endl;
                errorCount++;
                return;  // Skip to the next handle
            }
        }

        // Set the errorCount to zero if everything worked.
        errorCount = 0;
    }

    namespace manager {
        // Define the global variables
        atomic::guard<std::vector<handle>> handles;
        atomic::guard<tcp::listener> listener;
        
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
                    while (true) {
                        try {
                            // Use the atomic guard to safely access the listener and get a connection
                            listener([](tcp::listener& listenerRef){
                                tcp::connection conn = listenerRef.Accept();

                                // We will first listen for GGUI to give its own port to establish an personal connection to this specific GGUI client
                                uint16_t gguiPort;
                                if (!conn.Receive(&gguiPort)) {
                                    std::cerr << "Failed to receive GGUI port" << std::endl;
                                    return;
                                }

                                // Now we can create a new connection with this new port
                                tcp::connection gguiConnection = tcp::sender::getConnection(gguiPort);

                                // Before going to the next connection, we need to send confirmation back to the GGUI that we have accepted the connection
                                if (!gguiConnection.Send(&gguiPort)) {
                                    std::cerr << "Failed to send confirmation to GGUI" << std::endl;
                                    return;
                                }

                                // Now we can send the first packet to GGUI client, and it is the size of it at fullscreen.
                                // font::manager::getDefaultCellWidth()
                                types::rectangle windowRectangle = window::positionToCoordinates(position::FULLSCREEN);

                                types::iVector2 dimensionsInCells = {
                                    windowRectangle.size.x / font::manager::getDefaultCellWidth(),
                                    windowRectangle.size.y / font::manager::getDefaultCellHeight()
                                };

                                packet::type header = packet::type::RESIZE;

                                // Send the dimensions in cells to the GGUI client
                                if (!gguiConnection.Send(&header)) {
                                    std::cerr << "Failed to send resize packet to GGUI" << std::endl;
                                    return;
                                }

                                // Send the dimensions in cells to the GGUI client
                                if (!gguiConnection.Send(&dimensionsInCells)) {
                                    std::cerr << "Failed to send dimensions in cells to GGUI" << std::endl;
                                    return;
                                }

                                // Create a new handle for this connection
                                handles([&gguiConnection](std::vector<handle>& self){
                                    self.emplace_back(std::move(gguiConnection));
                                    
                                    // Set the display ID for the new handle to the primary display
                                    auto& newHandle = self.back();
                                    newHandle.setDisplayId(getPrimaryDisplayId());
                                    
                                    // Set the first handle as focused by default
                                    if (self.size() == 1) {
                                        setFocusedHandle(&self[0]);
                                        std::cout << "Set first handle as focused for input." << std::endl;
                                    }
                                    
                                    std::cout << "Created handle on display " << newHandle.getDisplayId() << std::endl;
                                });
                            });
                        } catch (const std::exception& e) {
                            std::cerr << "Error in reception thread: " << e.what() << std::endl;
                            continue;
                        }
                    }
                });

                reception.detach(); // Let reception retain after this local scope has ended.
            } catch (const std::exception& e) {
                std::cerr << "Failed to initialize window manager: " << e.what() << std::endl;
                throw;
            }
        }

        void close() {
            // listener has its destructor called automatically via atomic::guard.

            // free all handles
            handles([](std::vector<handle>& self){
                for (auto& h : self) {
                    h.close(); // Close each connection
                }
            });
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
                    std::cout << "Moved handle " << i << " to display " << displayId << std::endl;
                }
            });
        }
        
        void moveHandleToDisplay(handle* windowHandle, uint32_t displayId) {
            if (!windowHandle) {
                std::cerr << "Cannot move null handle to display" << std::endl;
                return;
            }
            
            // Check if the display exists
            if (display::manager::activeDisplays.find(displayId) == display::manager::activeDisplays.end()) {
                std::cerr << "Display ID " << displayId << " not found" << std::endl;
                return;
            }
            
            windowHandle->setDisplayId(displayId);
            std::cout << "Moved handle to display " << displayId << std::endl;
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
                
                std::cout << "Assigned displays to " << self.size() << " handles using strategy " 
                          << static_cast<int>(strategy) << std::endl;
            });
        }
        
        // Display monitoring and updates
        void updateHandleDisplays() {
            handles([](std::vector<handle>& self) {
                for (auto& handle : self) {
                    // Check if the handle's display still exists
                    if (!isValidDisplayId(handle.getDisplayId())) {
                        std::cout << "Handle's display " << handle.getDisplayId() 
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
            std::cout << "=== Handle-Display Mapping ===" << std::endl;
            handles([](std::vector<handle>& self) {
                for (size_t i = 0; i < self.size(); ++i) {
                    std::cout << "Handle " << i << " -> Display " 
                              << self[i].getDisplayId() << std::endl;
                }
            });
            
            auto distribution = getHandleDistribution();
            std::cout << "=== Display Distribution ===" << std::endl;
            for (const auto& pair : distribution) {
                std::cout << "Display " << pair.first << ": " << pair.second << " handles" << std::endl;
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