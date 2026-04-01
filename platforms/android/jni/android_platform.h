// android_platform.h
// Force-included via CMake before all source files.
// Provides platform overrides for Android builds.

#ifndef ANDROID_PLATFORM_H
#define ANDROID_PLATFORM_H

// On Android, redirect vorbisencdec.h to our stub/wrapper.
// The original vorbisencdec.h has hardcoded iOS xcframework include paths.
// We intercept the include by defining a macro that njclient.cpp's preprocessor
// will pick up before it tries to open the file with iOS paths.

#endif // ANDROID_PLATFORM_H
