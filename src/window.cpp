#include "window.h"
#include <cstdio>
#include <cstdint>
#include <thread>
#include <stdexcept>
#include <iostream>

namespace window {
    void handle::poll() {
        // GGUI client will always first give us the buffer dimensions first as a iVector2.
        types::iVector2 dimensions;
        if (!connection.Receive(&dimensions, 1)) {
            std::cerr << "Failed to receive dimensions from GGUI client" << std::endl;
            errorCount++;
            return;  // Skip to the next         
        }

        if (dimensions == types::iVector2{0, 0}) {
            // This a STAIN::CLEAN state, nothing changed from the previous buffer.
            return;   // goto next
        }

        if (dimensions != size) {
            cellBuffer = new std::vector<types::Cell>(dimensions.x * dimensions.y);
        }

        // Now we'll get the buffer data from the GGUI client.
        if (!connection.Receive(cellBuffer->data(), cellBuffer->size())) {
            std::cerr << "Failed to receive buffer data from GGUI client" << std::endl;
            errorCount++;
            return;  // Skip to the next handle
        }

        // Set the errorCount to zero if everything worked.
        errorCount = 0;
    }

    namespace manager {
        // Define the global variables
        atomic::guard<std::vector<handle>> handles;
        atomic::guard<tcp::listener> listener;
        
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
    }
}