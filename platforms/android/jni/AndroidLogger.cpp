#include <android/log.h>
#include <cstdarg>

#define LOG_TAG "NinjamCore"

// Implementation of LoggerBridge.h interface (same as iOS LoggerBridge.mm)
// These functions are called by IOSLogger.h/cpp via the C-linkage bridge.

extern "C" {

void LogMessage(const char* message, int level) {
    int androidLevel;
    switch (level) {
        case 0: androidLevel = ANDROID_LOG_ERROR; break;   // LogLevelError
        case 1: androidLevel = ANDROID_LOG_WARN; break;    // LogLevelWarning
        case 2: androidLevel = ANDROID_LOG_INFO; break;    // LogLevelInfo
        case 3: androidLevel = ANDROID_LOG_DEBUG; break;   // LogLevelDebug
        case 4: androidLevel = ANDROID_LOG_VERBOSE; break; // LogLevelTrace
        default: androidLevel = ANDROID_LOG_INFO; break;
    }
    __android_log_print(androidLevel, LOG_TAG, "%s", message);
}

void SetLogLevel(int level) {
    // Android logging is always enabled; level filtering happens in IOSLogOutput
    // Nothing to do here.
}

} // extern "C"
