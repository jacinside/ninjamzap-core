cmake_minimum_required(VERSION 3.18)
project(ninjamzap C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Paths
# ============================================================================
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../core/ninjamclient/libninjamcore)
set(BRIDGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../platforms/ios/NativeAudioModule)
set(JNI_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================================
# Vorbis configuration
# For initial builds, we stub out vorbis to get compilation working.
# Set NINJAM_USE_VORBIS=ON and provide vorbis libs to enable real encoding.
# ============================================================================
option(NINJAM_USE_VORBIS "Build with real vorbis libraries" OFF)

# ============================================================================
# Google Oboe (low-latency audio)
# ============================================================================
include(FetchContent)
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com/google/oboe.git
    GIT_TAG        1.9.0
)
FetchContent_MakeAvailable(oboe)

# ============================================================================
# Source files
# ============================================================================
set(NINJAM_SOURCES
    # --- JNI Bridge (Android-specific) ---
    ${JNI_DIR}/NinjamClientJNI.cpp
    ${JNI_DIR}/AndroidLogger.cpp
    ${JNI_DIR}/OboeCallback.cpp
    ${JNI_DIR}/fileutil_android.cpp

    # --- C Bridge (shared with iOS) ---
    ${BRIDGE_DIR}/NinjamClientBridge.cpp

    # --- abNinjam Adapter Layer ---
    ${CORE_DIR}/abNinjam/ninjamclientAdapter.cpp
    ${CORE_DIR}/abNinjam/ninjamclient.cpp
    ${CORE_DIR}/abNinjam/connectionproperties.cpp
    ${CORE_DIR}/abNinjam/licensedialog.cpp
    ${CORE_DIR}/abNinjam/ninjamutils.cpp
    ${CORE_DIR}/abNinjam/osctransmitter.cpp
    ${CORE_DIR}/abNinjam/remotechannel.cpp
    ${CORE_DIR}/abNinjam/remoteuser.cpp
    ${CORE_DIR}/abNinjam/remoteuserchannel.cpp
    ${CORE_DIR}/abNinjam/stringutil.cpp

    # --- NINJAM Protocol Engine ---
    ${CORE_DIR}/njclient.cpp
    ${CORE_DIR}/mpb.cpp
    ${CORE_DIR}/netmsg.cpp

    # --- IOSLogger (name is historical, works on Android via our LoggerBridge) ---
    ${CORE_DIR}/IOSLogger.cpp

    # --- WDL Networking ---
    ${CORE_DIR}/WDL/jnetlib/asyncdns.cpp
    ${CORE_DIR}/WDL/jnetlib/connection.cpp
    ${CORE_DIR}/WDL/jnetlib/httpget.cpp
    ${CORE_DIR}/WDL/jnetlib/listen.cpp
    ${CORE_DIR}/WDL/jnetlib/util.cpp

    # --- WDL Utilities ---
    ${CORE_DIR}/WDL/rng.cpp
    ${CORE_DIR}/WDL/sha.cpp
)

# Vorbis encoder (C file) — only if using real vorbis
if(NINJAM_USE_VORBIS)
    list(APPEND NINJAM_SOURCES ${CORE_DIR}/vorbis_encoder.c)
endif()

# ============================================================================
# Shared library
# ============================================================================
add_library(ninjamzap SHARED ${NINJAM_SOURCES})

# ============================================================================
# Include directories
# ============================================================================
target_include_directories(ninjamzap PRIVATE
    ${JNI_DIR}                        # Our JNI headers + LoggerBridge.h
    ${BRIDGE_DIR}                     # NinjamClientBridge.h
    ${CORE_DIR}                       # njclient.h, mpb.h, etc.
    ${CORE_DIR}/abNinjam              # ninjamclientAdapter.h
    ${CORE_DIR}/abNinjam/include      # ninjamclient.h, remoteuser.h, etc.
    ${CORE_DIR}/WDL                   # WDL headers
    ${CORE_DIR}/WDL/jnetlib           # jnetlib headers
)

# IOSLogger.h does: #include "../LoggerBridge.h" (relative to libninjamcore/)
# This resolves to core/ninjamclient/LoggerBridge.h — we need our Android version there.
# Solution: copy our LoggerBridge.h to that location at configure time.
file(COPY ${JNI_DIR}/LoggerBridge.h DESTINATION ${CORE_DIR}/..)

# ============================================================================
# Compiler flags
# ============================================================================
target_compile_options(ninjamzap PRIVATE
    -Wall
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-unused-but-set-variable
    -fvisibility=hidden
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
    $<$<CONFIG:Debug>:-O0 -g>
)

# ============================================================================
# Defines
# ============================================================================
target_compile_definitions(ninjamzap PRIVATE
    __ANDROID__
)

if(NOT NINJAM_USE_VORBIS)
    # Stub out vorbis — compiles without libogg/libvorbis
    target_compile_definitions(ninjamzap PRIVATE NINJAM_NO_VORBIS)
    # Override the vorbisencdec.h include to use our stub
    target_compile_definitions(ninjamzap PRIVATE
        "vorbisencdec_h_override"
    )
endif()

# ============================================================================
# Link libraries
# ============================================================================
target_link_libraries(ninjamzap
    oboe
    log         # __android_log_print
    android     # Android native APIs
)

if(NINJAM_USE_VORBIS)
    # TODO: Add ogg, vorbis, vorbisenc libraries when cross-compiled
    # target_link_libraries(ninjamzap ogg vorbis vorbisenc)
endif()
