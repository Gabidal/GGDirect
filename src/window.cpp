#include "window.h"
#include "input.h"
#include <cstdio>
#include <cstdint>
#include <thread>
#include <stdexcept>
#include <iostream>

namespace window {
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
                                if (!conn.Receive(&gguiPort, 1)) {
                                    std::cerr << "Failed to receive GGUI port" << std::endl;
                                    return;
                                }

                                // Now we can create a new connection with this new port
                                tcp::connection gguiConnection = tcp::sender::getConnection(gguiPort);

                                // Before going to the next connection, we need to send confirmation back to the GGUI that we have accepted the connection
                                if (!gguiConnection.Send(&gguiPort, 1)) {
                                    std::cerr << "Failed to send confirmation to GGUI" << std::endl;
                                    return;
                                }

                                // Create a new handle for this connection
                                handles([&gguiConnection](std::vector<handle>& self){
                                    self.emplace_back(std::move(gguiConnection));
                                    
                                    // Set the first handle as focused by default
                                    if (self.size() == 1) {
                                        setFocusedHandle(&self[0]);
                                        std::cout << "Set first handle as focused for input." << std::endl;
                                    }
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
    }
}