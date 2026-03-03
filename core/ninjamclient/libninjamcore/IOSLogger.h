// IOSLogger.h
#ifndef IOS_LOGGER_H
#define IOS_LOGGER_H

// Add this include to access the Log template class
#include "abNinjam/include/log.h"
#include "../LoggerBridge.h"
#include <string>

// Enum for log levels that matches both systems
enum LogLevel {
    LogLevelError = 0,    // matches Swift's error = 0 and C++'s lerror = 0
    LogLevelWarning = 1,  // matches Swift's warning = 1 and C++'s lwarning = 1
    LogLevelInfo = 2,     // matches Swift's info = 2 and C++'s linfo = 2
    LogLevelDebug = 3,    // matches Swift's debug = 3 and C++'s ldebug = 3
    LogLevelTrace = 4     // matches Swift's trace = 4 and C++'s ltrace = 4
};

class IOSLogOutput {
public:
    static void Output(const std::string& msg) {
        // Strip timestamp and log level from message as the Swift logger adds its own
        size_t colonPos = msg.find(":");
        if (colonPos != std::string::npos && colonPos + 2 < msg.length()) {
            std::string content = msg.substr(colonPos + 2);
            LogMessage(content.c_str(), currentLogLevel);
        } else {
            LogMessage(msg.c_str(), currentLogLevel);
        }
    }
    
    static int currentLogLevel;
};

// Typedef for compatibility with existing log.h
typedef Log<IOSLogOutput> IOSLog;

// Convert between TLogLevel and LogLevel
inline int ConvertLogLevel(TLogLevel level) {
    return static_cast<int>(level);
}

// Set the log level
inline void SetIOSLogLevel(TLogLevel level) {
    IOSLogOutput::currentLogLevel = ConvertLogLevel(level);
    SetLogLevel(IOSLogOutput::currentLogLevel);
}

// Macros for iOS-specific logging
#define IOS_LOG(level) \
    IOSLogOutput::currentLogLevel = ConvertLogLevel(level); \
    if (level > FILELOG_MAX_LEVEL) ; \
    else IOSLog().Get(level)

#endif // IOS_LOGGER_H

inline void InitializeIOSLogger(TLogLevel level = linfo) {
    SetIOSLogLevel(level);
    IOS_LOG(linfo) << "IOS Logger initialized at level " << Log<IOSLogOutput>::ToString(level);
}
