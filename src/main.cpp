#include "window.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

int main() {
    try {
        std::cout << "Starting GGDirect window manager..." << std::endl;
        
        // Initialize the window manager
        window::manager::init();
        
        std::cout << "GGDirect window manager initialized successfully." << std::endl;
        std::cout << "Press Ctrl+C to exit." << std::endl;
        
        // Keep the main thread alive
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
