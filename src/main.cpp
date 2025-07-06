#include "system.h"
#include "logger.h"

#include <thread>
#include <chrono>
#include <cstring>

int main(int argc, char* argv[]) {
    bool verbose = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "GGDirect - Direct GPU Terminal Manager\n";
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
            std::cout << "\nOptions:\n";
            std::cout << "  --verbose, -v    Enable verbose logging\n";
            std::cout << "  --help, -h       Show this help message\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            std::cerr << "Use --help for usage information." << std::endl;
            return 1;
        }
    }
    
    // Initialize logger
    logger::init(verbose);
    
    DRM::system::init();

    // Keep the main thread alive
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}
