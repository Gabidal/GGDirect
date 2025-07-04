#include "window.h"
#include "renderer.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <signal.h>

volatile sig_atomic_t keep_running = 1;

void signal_handler(int sig) {
    (void)sig;  // Suppress unused parameter warning
    keep_running = 0;
}

int main() {
    try {
        // Set up signal handler
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        
        std::cout << "Starting GGDirect window manager..." << std::endl;
        
        // Initialize the window manager
        window::manager::init();
        std::cout << "Window manager initialized successfully." << std::endl;
        
        // Initialize the renderer
        renderer::init();
        std::cout << "Renderer initialized successfully." << std::endl;
        
        std::cout << "GGDirect is ready. Press Ctrl+C to exit." << std::endl;
        
        // Keep the main thread alive
        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "Shutting down..." << std::endl;
        
        // Cleanup
        renderer::exit();
        window::manager::close();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
