#include "system.h"
#include "logger.h"
#include "renderer.h"
#include "window.h"
#include "input.h"
#include "config.h"
#include "display.h"

#include <signal.h>
#include <initializer_list>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace DRM {
    namespace system {

        void init() {
            logger::info("Starting GGDirect window manager...");
            
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
                    logger::error("Failed to register exit handler.");
                }
                
                // Initialize the configuration system first
                config::manager::init();
                logger::info("Configuration system initialized successfully.");
                
                // Initialize the display subsystem (supports headless mode)
                if (!display::manager::initialize()) {
                    throw std::runtime_error("Failed to initialize display subsystem");
                }

                logger::info("Display subsystem initialized successfully.");

                if (display::manager::Device && display::manager::Device->getDeviceFd() == -2) {
                    LOG_INFO() << "Running without GPU output (headless mode detected)." << std::endl;
                }

                // Initialize the window manager
                window::manager::init();
                logger::info("Window manager initialized successfully.");
                
                // Initialize the input system
                input::manager::init();
                logger::info("Input system initialized successfully.");
                
                // Initialize the renderer
                renderer::init();
                if (display::manager::Device && display::manager::Device->getDeviceFd() == -2) {
                    logger::info("Renderer initialization skipped (headless mode).");
                } else {
                    logger::info("Renderer initialized successfully.");
                }
                
                logger::info("GGDirect is ready. Press Ctrl+C to exit.");

            } catch (const std::exception& e) {
                logger::error(std::string("Initialization failed: ") + e.what());
            }
        }

        void cleanup() {
            // Perform any necessary cleanup operations here
            LOG_VERBOSE() << "Cleaning up system resources..." << std::endl;
            
            // Signal all threads to stop first
            // This is for when we start using conditional variables and mutex instead of while true loops on other threads.
            // We may need to notify other threads to wake up and check for shutdown conditions.
            
            // Clean up input system first (stops polling threads)
            // This should be fast since it properly joins threads
            input::manager::exit();
            
            // Clean up configuration system
            config::manager::cleanup();
            
            // Clean up renderer (stops rendering thread)
            // This may take up to 200ms to complete
            renderer::exit();
            
            // Clean up window manager last (stops reception thread and closes connections)
            // This may take up to 200ms to complete
            window::manager::close();

            // Release display resources
            display::manager::cleanup();

            LOG_VERBOSE() << "System cleanup completed." << std::endl;

            logger::info("Shutdown complete.");
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
