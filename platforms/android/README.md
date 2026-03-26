# Android Support for NinjamZap Core

## Status: Scaffolding In Progress

The C++ core is cross-platform and ready. The Android JNI bridge and Oboe audio callback are scaffolded.

## Directory Structure

```
platforms/android/
├── jni/
│   ├── CMakeLists.txt          # NDK build configuration (Oboe + C++ core + Vorbis)
│   ├── NinjamClientJNI.h       # JNI method declarations
│   ├── NinjamClientJNI.cpp     # JNI bindings for all 40+ C bridge functions
│   ├── AndroidLogger.cpp       # Android logging (replaces IOSLogger)
│   ├── OboeCallback.h          # Oboe AudioStreamCallback header
│   └── OboeCallback.cpp        # Full-duplex audio render loop
└── README.md                   # This file
```

## Dependencies

- **Android NDK** r21+ (CMake 3.18+)
- **Google Oboe** 1.8+ (fetched via CMake FetchContent)
- **libogg / libvorbis / libvorbisenc** (cross-compiled or pre-built for Android ABIs)

## Build

The JNI sources are compiled as part of the React Native Android build via `externalNativeBuild` in `build.gradle`. The CMakeLists.txt references the shared C++ core from `../../core/ninjamclient/libninjamcore/`.

## Architecture

```
Kotlin (NinjamClientBridge.kt)
    |  external fun declarations
    v
JNI (NinjamClientJNI.cpp)
    |  maps to NinjamClient_* C functions
    v
C Bridge (NinjamClientBridge.h/cpp)  <-- shared with iOS
    |
    v
C++ Core (njclient, adapter, WDL, Vorbis)  <-- shared with iOS
```

## Callback Flow

C++ network threads fire callbacks (connect, disconnect, chat, license, interval).
These are routed via JNI `AttachCurrentThread` to Kotlin, then posted to the
main thread for React Native event emission.

## Next Steps

See `ninjam-mobile/docs/ANDROID_MIGRATION_PLAN.md` for the full phased plan.

## Resources

- [Android NDK](https://developer.android.com/ndk)
- [Google Oboe](https://github.com/google/oboe)
- [JNI Best Practices](https://developer.android.com/training/articles/perf-jni)
- [Vorbis for Android](https://xiph.org/vorbis/)
