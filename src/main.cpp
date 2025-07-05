#include "system.h"

#include <thread>
#include <chrono>

int main() {
    DRM::system::init();

    // Keep the main thread alive
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}
