#ifndef _TCP_H_
#define _TCP_H_

#include <unistd.h>
#include <cstdint>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <fcntl.h>
#include <errno.h>
#include <iostream>
#include <vector>

#include "types.h"


namespace packet {
    enum class type {
        UNKNOWN,
        DRAW_BUFFER,    // For sending/receiving cells
        INPUT,          // Fer sending/receiving input data
        NOTIFY,         // Contains an notify flag sending like empty buffers, for optimized polling.
        RESIZE,         // For sending/receiving GGUI resize
    };

    class base {
    public:
        type packetType = type::UNKNOWN;

        base(type t) : packetType(t) {}
    };

    namespace notify {
        enum class type {
            UNKNOWN         = 0 << 0,
            EMPTY_BUFFER    = 1 << 0,
            CLOSED          = 1 << 1,   // When GGUI client has shutdown
        };

        class base : public packet::base {
        public:
            type notifyType = type::UNKNOWN;

            base(type t) : packet::base(packet::type::NOTIFY), notifyType(t) {}
        };
    }

    namespace input {
        enum class controlKey {
            UNKNOWN         = 0 << 0,
            SHIFT           = 1 << 0,
            CTRL            = 1 << 1,
            SUPER           = 1 << 2,
            ALT             = 1 << 3,
            ALTGR           = 1 << 4,
            FN              = 1 << 5,
            PRESSED_DOWN    = 1 << 6,   // Always on/off to indicate if the key is being pressed down or not.
        };

        enum class additionalKey {
            UNKNOWN,
            F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
            ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT,
            HOME, END, PAGE_UP, PAGE_DOWN,
            INSERT, DELETE,
            LEFT_CLICK, MIDDLE_CLICK, RIGHT_CLICK, SCROLL_UP, SCROLL_DOWN,
        };

        class base : public packet::base {
        public:
            types::sVector2 mouse;      // Mouse position in the terminal   
            controlKey modifiers;       // Control keys pressed
            additionalKey additional;   // Additional keys pressed, which are not declared in ASCII
            unsigned char key;          // ASCII key pressed, if any

            base() : packet::base(packet::type::INPUT), mouse(), modifiers(controlKey::UNKNOWN), additional(additionalKey::UNKNOWN), key(0) {}
        };
    }

    namespace resize {
        class base : public packet::base {
        public:
            types::sVector2 size;

            base(types::sVector2 s) : packet::base(packet::type::RESIZE), size(s) {}
        };
    }

    union maxSizetype {
        notify::base n;
        input::base i;
        resize::base r;
    };

    // Computes at compile time the maximum needed buffer length for a packet.
    constexpr int size = sizeof(maxSizetype);
}

namespace tcp {
    /**
     * @brief Represents a TCP connection for sending and receiving data.
     * 
     * This class wraps a socket file descriptor and provides methods for
     * sending and receiving typed data over a TCP connection.
     */
    class connection {
        int handle;
    public:
        /**
         * @brief Constructs a connection from an existing socket file descriptor.
         * 
         * @param socketFd The socket file descriptor to wrap. Must be a valid TCP socket.
         * @throws std::invalid_argument if socketFd is negative (invalid).
         */
        explicit connection(int socketFd) : handle(socketFd) {
            if (socketFd < 0) {
                throw std::invalid_argument("Invalid socket file descriptor");
            }
        }

        // destructors are called via the handle manager and thus not needed to be called here

        /**
         * @brief Destructor that closes the connection if still open.
         */
        ~connection() {
            close();
        }

        bool isClosed() {
            return handle < 0;
        }

        // Disable copy constructor and assignment operator to prevent double-close
        connection(const connection&) = delete;
        connection& operator=(const connection&) = delete;

        // Enable move constructor and assignment operator
        connection(connection&& other) noexcept : handle(other.handle) {
            other.handle = -1;
        }

        connection& operator=(connection&& other) noexcept {
            if (this != &other) {
                close();
                handle = other.handle;
                other.handle = -1;
            }
            return *this;
        }

        /**
         * @brief Sends typed data over the TCP connection.
         * 
         * This template method sends count elements of type T over the connection.
         * It ensures all data is sent by checking the return value against the expected size.
         * 
         * @tparam T The type of data to send
         * @param data Pointer to the data buffer to send
         * @param count Number of elements of type T to send
         * @return true if all data was successfully sent, false otherwise
         * @throws std::runtime_error if the socket is invalid or closed
         */
        template<typename T>
        bool Send(const T* data, size_t count = 1) {
            if (handle < 0) {
                throw std::runtime_error("Cannot send on closed socket");
            }
            if (!data && count > 0) {
                return false;
            }
            
            size_t totalBytes = count * sizeof(T);
            ssize_t sent = send(handle, data, totalBytes, 0);
            
            if (sent < 0) {
                // Send failed due to error
                return false;
            }
            
            return sent == static_cast<ssize_t>(totalBytes);
        }

        /**
         * @brief Receives typed data from the TCP connection.
         * 
         * This template method receives count elements of type T from the connection.
         * It ensures all expected data is received by checking the return value.
         * 
         * @tparam T The type of data to receive
         * @param data Pointer to the buffer where received data will be stored
         * @param count Number of elements of type T to receive
         * @return true if all expected data was successfully received, false otherwise
         * @throws std::runtime_error if the socket is invalid or closed
         */
        template<typename T>
        bool Receive(T* data, size_t count = 1) {
            if (handle < 0) {
                throw std::runtime_error("Cannot receive on closed socket");
            }
            if (!data && count > 0) {
                return false;
            }
            
            size_t totalBytes = count * sizeof(T);
            size_t bytesReceived = 0;
            char* buffer = reinterpret_cast<char*>(data);
            
            // Keep receiving until we have all the data or an error occurs
            while (bytesReceived < totalBytes) {
                ssize_t recvd = recv(handle, buffer + bytesReceived, totalBytes - bytesReceived, 0);
                
                if (recvd < 0) {
                    // Receive failed due to error
                    return false;
                }
                if (recvd == 0) {
                    // Connection closed by peer
                    return false;
                }
                
                bytesReceived += recvd;
            }
            
            return bytesReceived == totalBytes;
        }

        /**
         * @brief Gets the underlying socket file descriptor.
         * 
         * @return The socket file descriptor, or -1 if the connection is closed
         */
        int getHandle() const { return handle; }

        /**
         * @brief Makes the socket non-blocking.
         * 
         * @return true if successful, false otherwise
         */
        bool setNonBlocking() {
            if (handle < 0) {
                return false;
            }
            
            int flags = fcntl(handle, F_GETFL, 0);
            if (flags == -1) {
                return false;
            }
            
            return fcntl(handle, F_SETFL, flags | O_NONBLOCK) != -1;
        }

        /**
         * @brief Checks if data is available for reading without blocking.
         * 
         * @return true if data is available, false otherwise
         */
        bool hasDataAvailable() {
            if (handle < 0) {
                return false;
            }
            
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(handle, &readfds);
            
            struct timeval timeout = {0, 0};  // No timeout, immediate return
            
            int result = select(handle + 1, &readfds, nullptr, nullptr, &timeout);
            return result > 0 && FD_ISSET(handle, &readfds);
        }

        /**
         * @brief Non-blocking version of Receive that returns immediately if no data is available.
         * 
         * @tparam T The type of data to receive
         * @param data Pointer to the buffer where received data will be stored
         * @param count Number of elements of type T to receive
         * @return true if all expected data was successfully received, false otherwise
         */
        template<typename T>
        bool ReceiveNonBlocking(T* data, size_t count = 1) {
            if (handle < 0) {
                return false;
            }
            if (!data && count > 0) {
                return false;
            }
            
            size_t totalBytes = count * sizeof(T);
            size_t bytesReceived = 0;
            char* buffer = reinterpret_cast<char*>(data);
            
            // Keep receiving until we have all the data or would block
            while (bytesReceived < totalBytes) {
                // Check if more data is available before each read
                if (!hasDataAvailable()) {
                    // No more data available right now
                    return false;
                }
                
                ssize_t recvd = recv(handle, buffer + bytesReceived, totalBytes - bytesReceived, MSG_DONTWAIT);
                
                if (recvd < 0) {
                    if (errno == EWOULDBLOCK) {
                        // Would block, return false to indicate no complete data available
                        return false;
                    }
                    // Other error
                    return false;
                }
                if (recvd == 0) {
                    // Connection closed by peer
                    return false;
                }
                
                bytesReceived += recvd;
            }
            
            return bytesReceived == totalBytes;
        }

    private:
        // Internal buffer for packet reception to handle partial reads
        std::vector<char> packetBuffer;
        size_t packetBytesReceived = 0;
        
    public:
        /**
         * @brief Attempt to receive a complete packet using internal buffering.
         * 
         * This method handles partial packet reception by maintaining an internal buffer.
         * It will only return true when a complete packet has been received.
         * 
         * @param data Pointer to buffer where the complete packet will be stored
         * @param packetSize Size of the packet to receive
         * @return true if a complete packet was received, false otherwise
         */
        bool ReceivePacketNonBlocking(void* data, size_t packetSize) {
            if (handle < 0 || !data || packetSize == 0) {
                return false;
            }
            
            // Initialize buffer if needed
            if (packetBuffer.size() != packetSize) {
                packetBuffer.resize(packetSize);
                packetBytesReceived = 0;
            }
            
            // Keep reading until we have a complete packet
            while (packetBytesReceived < packetSize) {
                // Check if data is available
                if (!hasDataAvailable()) {
                    return false; // No more data available right now
                }
                
                ssize_t recvd = recv(
                    handle, 
                    packetBuffer.data() + packetBytesReceived, 
                    packetSize - packetBytesReceived, 
                    MSG_DONTWAIT
                );
                
                if (recvd < 0) {
                    if (errno == EWOULDBLOCK) {
                        // Would block, but we might have partial data
                        return false;
                    }
                    // Other error - reset buffer
                    packetBytesReceived = 0;
                    return false;
                }
                if (recvd == 0) {
                    // Connection closed by peer - reset buffer
                    packetBytesReceived = 0;
                    return false;
                }
                
                packetBytesReceived += recvd;
            }
            
            // We have a complete packet, copy it to output buffer
            std::memcpy(data, packetBuffer.data(), packetSize);
            
            // Reset for next packet
            packetBytesReceived = 0;
            
            return true;
        }

        /**
         * @brief Closes the TCP connection.
         * 
         * This method closes the underlying socket and marks the connection as invalid.
         * It's safe to call this method multiple times.
         */
        void close() {
            if (handle >= 0) {
                ::close(handle);
                handle = -1; // Mark as closed
            }
        }
    };

    /**
     * @brief TCP listener for accepting incoming connections.
     * 
     * This class creates a TCP server socket that can listen for and accept
     * incoming connections on a specified port.
     */
    class listener {
        int handle;
    public:
        /**
         * @brief Default constructor that creates an uninitialized listener.
         * 
         * Creates a listener with an invalid handle. Must be assigned or moved
         * from a properly constructed listener before use.
         */
        listener() : handle(-1) {}

        /**
         * @brief Constructs a TCP listener on the specified port.
         * 
         * Creates a TCP socket, binds it to the specified port (or any available port if 0),
         * and starts listening for incoming connections.
         * 
         * @param port The port number to listen on. Use 0 to let the system assign an available port.
         * @throws std::runtime_error if socket creation, binding, or listening fails
         */
        explicit listener(uint16_t port) : handle(-1) {
            // Create socket
            handle = socket(AF_INET, SOCK_STREAM, 0);
            if (handle < 0) {
                throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
            }

            // Set socket option to reuse address
            int opt = 1;
            if (setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
                ::close(handle);
                throw std::runtime_error("Failed to set socket option SO_REUSEADDR: " + std::string(strerror(errno)));
            }

            // Bind socket to address
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            
            if (bind(handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(handle);
                throw std::runtime_error("Failed to bind socket to port " + std::to_string(port) + ": " + std::string(strerror(errno)));
            }

            // Start listening
            if (listen(handle, 5) < 0) {
                ::close(handle);
                throw std::runtime_error("Failed to start listening on socket: " + std::string(strerror(errno)));
            }
        }

        /**
         * @brief Destructor that properly closes the listener socket.
         */
        ~listener() {
            if (handle >= 0) {
                ::close(handle);
            }
        }

        // Disable copy constructor and assignment operator to prevent double-close
        listener(const listener&) = delete;
        listener& operator=(const listener&) = delete;

        // Enable move constructor and assignment operator
        listener(listener&& other) noexcept : handle(other.handle) {
            other.handle = -1;
        }

        listener& operator=(listener&& other) noexcept {
            if (this != &other) {
                if (handle >= 0) {
                    ::close(handle);
                }
                handle = other.handle;
                other.handle = -1;
            }
            return *this;
        }

        /**
         * @brief Accepts an incoming connection.
         * 
         * This method blocks until a client connects to the listener.
         * 
         * @return A connection object representing the accepted client connection
         * @throws std::runtime_error if the listener socket is closed or accept fails
         */
        connection Accept() {
            if (handle < 0) {
                throw std::runtime_error("Cannot accept on closed listener");
            }

            int connFd = accept(handle, nullptr, nullptr);
            if (connFd < 0) {
                throw std::runtime_error("Failed to accept connection: " + std::string(strerror(errno)));
            }
            
            // Enable TCP_NODELAY for lower latency on accepted connections
            int nodelay = 1;
            if (setsockopt(connFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
                std::cerr << "Warning: Failed to enable TCP_NODELAY on accepted connection: " << strerror(errno) << std::endl;
            }
            
            return connection(connFd);
        }

        /**
         * @brief Gets the port number this listener is bound to.
         * 
         * This is particularly useful when the listener was created with port 0,
         * as it returns the actual port assigned by the system.
         * 
         * @return The port number this listener is bound to
         * @throws std::runtime_error if getting the socket name fails
         */
        uint16_t getPort() {
            if (handle < 0) {
                throw std::runtime_error("Cannot get port of closed listener");
            }

            sockaddr_in actual{};
            socklen_t len = sizeof(actual);
            
            if (getsockname(handle, reinterpret_cast<sockaddr*>(&actual), &len) < 0) {
                throw std::runtime_error("Failed to get socket name: " + std::string(strerror(errno)));
            }

            return ntohs(actual.sin_port);
        }
        
        /**
         * @brief Gets the underlying socket file descriptor.
         * 
         * This is useful for advanced socket operations like setting to non-blocking mode.
         * 
         * @return The socket file descriptor, or -1 if the listener is closed
         */
        int getHandle() const { 
            return handle; 
        }
    };

    /**
     * @brief Utility class for creating outgoing TCP connections.
     * 
     * This class provides static methods to establish TCP connections to remote hosts.
     */
    class sender {
    public:
        /**
         * @brief Creates a TCP connection to the specified host and port.
         * 
         * Establishes a TCP connection to the given host and port. This method
         * creates a socket, resolves the host address, and connects to the remote endpoint.
         * 
         * @param port The port number to connect to
         * @param host The hostname or IP address to connect to (defaults to localhost)
         * @return A connection object representing the established connection
         * @throws std::runtime_error if socket creation, address resolution, or connection fails
         */
        static connection getConnection(uint16_t port, const char* host = "127.0.0.1") {
            if (!host) {
                throw std::invalid_argument("Host cannot be null");
            }

            // Create socket
            int sockFd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockFd < 0) {
                throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
            }

            // Enable TCP_NODELAY for lower latency
            int nodelay = 1;
            if (setsockopt(sockFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
                // Non-fatal error, continue but log warning
                std::cerr << "Warning: Failed to enable TCP_NODELAY: " << strerror(errno) << std::endl;
            }

            // Prepare address structure
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            
            // Convert IP address from text to binary form
            int result = inet_pton(AF_INET, host, &addr.sin_addr);
            if (result <= 0) {
                ::close(sockFd);
                if (result == 0) {
                    throw std::runtime_error("Invalid IP address format: " + std::string(host));
                } else {
                    throw std::runtime_error("inet_pton failed: " + std::string(strerror(errno)));
                }
            }

            // Connect to the remote host
            if (connect(sockFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(sockFd);
                throw std::runtime_error("Failed to connect to " + std::string(host) + ":" + 
                                       std::to_string(port) + " - " + std::string(strerror(errno)));
            }
            
            return connection(sockFd);
        }
    };
}


#endif