#include <android/log.h>

#define LOG_TAG "NinjamCore"

// Replacement for IOSLogger.cpp on Android
// Called from C++ core code for logging

extern "C" {

void ninjam_log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, format, args);
    va_end(args);
}

void ninjam_log_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, LOG_TAG, format, args);
    va_end(args);
}

void ninjam_log_debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_DEBUG, LOG_TAG, format, args);
    va_end(args);
}

} // extern "C"
