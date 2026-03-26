cmake_minimum_required(VERSION 3.18)
project(ninjamzap C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Path to the NINJAM C++ core (relative to this CMakeLists.txt)
# ============================================================================
set(NINJAM_CORE_DIR ${CMAKE_SOURCE_DIR}/../../../core/ninjamclient/libninjamcore)
set(NINJAM_BRIDGE_DIR ${CMAKE_SOURCE_DIR}/../../../platforms/ios/NativeAudioModule)

# ============================================================================
# Google Oboe (low-latency audio for Android)
# ============================================================================
include(FetchContent)
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com/google/oboe.git
    GIT_TAG        1.8.0
)
FetchContent_MakeAvailable(oboe)

# ============================================================================
# libogg + libvorbis
# Pre-built libraries or build from source
# ============================================================================
# Option A: Pre-built .so files in libs_compiled/android/{abi}/
# set(OGG_LIB_DIR ${CMAKE_SOURCE_DIR}/../libs_compiled/android/${ANDROID_ABI})
# add_library(ogg SHARED IMPORTED)
# set_target_properties(ogg PROPERTIES IMPORTED_LOCATION ${OGG_LIB_DIR}/libogg.so)
# add_library(vorbis SHARED IMPORTED)
# set_target_properties(vorbis PROPERTIES IMPORTED_LOCATION ${OGG_LIB_DIR}/libvorbis.so)
# add_library(vorbisenc SHARED IMPORTED)
# set_target_properties(vorbisenc PROPERTIES IMPORTED_LOCATION ${OGG_LIB_DIR}/libvorbisenc.so)

# Option B: Build from source (uncomment and add submodules)
# add_subdirectory(third_party/ogg)
# add_subdirectory(third_party/vorbis)

# TODO: Choose Option A or B above and uncomment the appropriate section.
# For now, define placeholder targets so CMake doesn't fail on first setup.
# Remove these once real libraries are configured.

# ============================================================================
# NINJAM Core shared library
# ============================================================================
add_library(ninjamzap SHARED
    # --- JNI Bridge (Android-specific) ---
    NinjamClientJNI.cpp
    AndroidLogger.cpp
    OboeCallback.cpp

    # --- C Bridge (shared, from ninjamzap-core) ---
    ${NINJAM_BRIDGE_DIR}/NinjamClientBridge.cpp

    # --- NinjamClientAdapter (shared) ---
    ${NINJAM_CORE_DIR}/abNinjam/ninjamclientAdapter.cpp
    ${NINJAM_CORE_DIR}/abNinjam/ninjamclient.cpp

    # --- NINJAM Protocol Engine (shared) ---
    ${NINJAM_CORE_DIR}/njclient.cpp
    ${NINJAM_CORE_DIR}/mpb.cpp
    ${NINJAM_CORE_DIR}/netmsg.cpp

    # --- WDL Networking (shared) ---
    ${NINJAM_CORE_DIR}/WDL/jnetlib/asyncdns.cpp
    ${NINJAM_CORE_DIR}/WDL/jnetlib/connection.cpp
    ${NINJAM_CORE_DIR}/WDL/jnetlib/httpget.cpp
    ${NINJAM_CORE_DIR}/WDL/jnetlib/listen.cpp
    ${NINJAM_CORE_DIR}/WDL/jnetlib/util.cpp

    # --- WDL Utilities (shared) ---
    ${NINJAM_CORE_DIR}/WDL/rng.cpp
    ${NINJAM_CORE_DIR}/WDL/sha.cpp

    # --- Vorbis Encoder (shared) ---
    ${NINJAM_CORE_DIR}/vorbis_encoder.c
)

# Include directories
target_include_directories(ninjamzap PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${NINJAM_BRIDGE_DIR}
    ${NINJAM_CORE_DIR}
    ${NINJAM_CORE_DIR}/abNinjam
    ${NINJAM_CORE_DIR}/WDL
    ${NINJAM_CORE_DIR}/WDL/jnetlib
    # Add ogg/vorbis include dirs when configured:
    # ${OGG_INCLUDE_DIR}
    # ${VORBIS_INCLUDE_DIR}
)

# Compiler flags
target_compile_options(ninjamzap PRIVATE
    -Wall
    -Wno-unused-variable
    -Wno-unused-function
    -fvisibility=hidden
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
    $<$<CONFIG:Debug>:-O0 -g>
)

# Defines
target_compile_definitions(ninjamzap PRIVATE
    __ANDROID__
    # Disable Apple-specific code paths in the bridge
)

# Link libraries
target_link_libraries(ninjamzap
    oboe
    # ogg vorbis vorbisenc  # Uncomment when Vorbis libs are configured
    log         # Android logging (__android_log_print)
    android     # Android native APIs
)
