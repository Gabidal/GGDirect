#ifndef _SYSTEM_H_
#define _SYSTEM_H_

#include <cstdint>  // For uint64_t and uint32_t types

namespace DRM {
    namespace system {
        extern void init();    // Initializes the system, setting up necessary resources.
        extern void cleanup();    // Cleans up resources and exits the system.
        
        // Function to get the current system time in milliseconds
        extern uint64_t getCurrentTimeMillis();
        
        // Function to sleep for a specified number of milliseconds
        extern void sleep(uint32_t milliseconds);
    }
}

#endif