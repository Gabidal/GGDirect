#ifndef _LOGGER_H_
#define _LOGGER_H_

#include <iostream>
#include <string>

namespace logger {
    // Log levels
    enum class Level {
        ERROR,    // Always shown
        INFO,     // Major events (connections, initialization, etc.)
        VERBOSE   // Detailed debug information
    };
    
    // Global verbose flag
    extern bool isVerbose;
    
    // Initialize logger with verbose setting
    void init(bool verbose);
    
    // Logging functions
    void error(const std::string& message);
    void info(const std::string& message);
    void verbose(const std::string& message);
    
    // Stream-style logging
    class LogStream {
    private:
        Level level;
        bool shouldPrint;
        
    public:
        LogStream(Level level);
        ~LogStream();
        
        template<typename T>
        LogStream& operator<<(const T& value) {
            if (shouldPrint) {
                std::cout << value;
            }
            return *this;
        }
        
        // Handle endl specifically
        LogStream& operator<<(std::ostream& (*pf)(std::ostream&)) {
            if (shouldPrint) {
                std::cout << pf;
            }
            return *this;
        }
    };
    
    // Convenience macros for stream-style logging
    #define LOG_ERROR() logger::LogStream(logger::Level::ERROR)
    #define LOG_INFO() logger::LogStream(logger::Level::INFO)
    #define LOG_VERBOSE() logger::LogStream(logger::Level::VERBOSE)
}

#endif
