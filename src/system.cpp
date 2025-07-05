#include "system.h"
#include "renderer.h"
#include "window.h"

#include <signal.h>
#include <initializer_list>
#include <cstdlib>
#include <iostream>

namespace DRM {
    namespace system {

        void init() {
            std::cout << "Starting GGDirect window manager..." << std::endl;
            
            try {
                struct sigaction normal_exit = {};
                normal_exit.sa_handler = []([[maybe_unused]] int dummy){
                    exit(0);  // Ensures `atexit()` is triggered
                };
                sigemptyset(&normal_exit.sa_mask);
                normal_exit.sa_flags = 0;

                for (
                    auto i : {
                        SIGINT,
                        SIGILL,
                        SIGABRT,
                        SIGFPE,
                        SIGSEGV,
                        SIGTERM
                    }){
                    sigaction(i, &normal_exit, NULL);
                }

                if (atexit([](){cleanup();})){
                    std::cerr << "Failed to register exit handler." << std::endl;
                }
                
                // Initialize the window manager
                window::manager::init();
                std::cout << "Window manager initialized successfully." << std::endl;
                
                // Initialize the renderer
                renderer::init();
                std::cout << "Renderer initialized successfully." << std::endl;
                
                std::cout << "GGDirect is ready. Press Ctrl+C to exit." << std::endl;

            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }

        void cleanup() {
            // Perform any necessary cleanup operations here
            std::cout << "Cleaning up system resources..." << std::endl;
            
            // This is for when we start using conditional variables and mutex instead of while true loops on other threads.
            // We may need to notify other threads to wake up and check for shutdown conditions.
            
            renderer::exit();
            window::manager::close();

            std::cout << "System cleanup completed." << std::endl;

            std::cout << "Shutdown..." << std::endl;
        }

        uint64_t getCurrentTimeMillis() {
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
                std::cerr << "Failed to get current time." << std::endl;
                return 0;
            }
            return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        }

        void sleep(uint32_t milliseconds) {
            struct timespec ts;
            ts.tv_sec = milliseconds / 1000;
            ts.tv_nsec = (milliseconds % 1000) * 1000000;

            if (nanosleep(&ts, NULL) == -1) {
                std::cerr << "Sleep interrupted or failed." << std::endl;
            }
        }

    }

}
