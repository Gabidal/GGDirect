#include "logger.h"

namespace logger {
    // Global verbose flag
    bool isVerbose = false;
    
    void init(bool verbose) {
        isVerbose = verbose;
    }
    
    void error(const std::string& message) {
        std::cerr << "ERROR: " << message << std::endl;
    }
    
    void info(const std::string& message) {
        std::cout << message << std::endl;
    }
    
    void verbose(const std::string& message) {
        if (isVerbose) {
            std::cout << "[VERBOSE] " << message << std::endl;
        }
    }
    
    LogStream::LogStream(Level Level) : level(Level) {
        switch (level) {
            case Level::ERROR:
                shouldPrint = true;
                std::cout << "ERROR: ";
                break;
            case Level::INFO:
                shouldPrint = true;
                break;
            case Level::VERBOSE:
                shouldPrint = isVerbose;
                if (shouldPrint) {
                    std::cout << "[VERBOSE] ";
                }
                break;
        }
    }
    
    LogStream::~LogStream() {
        if (shouldPrint) {
            std::cout.flush();
        }
    }
}
