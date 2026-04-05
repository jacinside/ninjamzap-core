#ifndef NINJAM_CLIENT_BRIDGE_H
#define NINJAM_CLIENT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

// Include Core Audio types for OSStatus and AudioTimeStamp
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudioTypes.h>
#else
// Define fallback types for non-Apple platforms
typedef int32_t OSStatus;
typedef struct AudioTimeStamp AudioTimeStamp;
#define noErr 0
#define kAudioUnitErr_FailedInitialization -10868
#endif

#ifdef __cplusplus
extern "C" {
#endif



// Define the opaque C type for NINJAM client
//private struct NJClientRefOpaqueType {}
//typealias NinjamClientRef = NJClientRefOpaqueType;

// Opaque reference struct for Swift
typedef struct NinjamClientRef NinjamClientRef;



// Client lifecycle
NinjamClientRef* NinjamClient_create(void);
void NinjamClient_destroy(NinjamClientRef* client);
const char* NinjamClient_getVersion(void);

// Connection management
void NinjamClient_setUser(NinjamClientRef* client, const char* username, const char* password);
void NinjamClient_connect(NinjamClientRef* client, const char* hostname, int32_t port);
void NinjamClient_disconnect(NinjamClientRef* client);
int32_t NinjamClient_isConnected(NinjamClientRef* client);
void NinjamClient_process(NinjamClientRef* client);
const char* NinjamClient_getServerStatus(NinjamClientRef* client);
int32_t NinjamClient_getLatency(NinjamClientRef* client);
int32_t NinjamClient_getServerUptime(NinjamClientRef* client);

// Audio configuration
void NinjamClient_setAudioConfig(NinjamClientRef* client, int32_t sampleRate, int32_t channels);
//void NinjamClient_processAudio(NinjamClientRef* client, float* inBuffer, float* outBuffer, int32_t numFrames);
void NinjamClient_processAudio(NinjamClientRef* client, float* inBufferLeft, float* inBufferRight, float* outBufferLeft, float* outBufferRight, int32_t numFrames);

// Streamlined audio processing function for zero-copy operations
OSStatus NinjamClient_processAudioStreamlined(NinjamClientRef* client, const float* inBufferLeft, const float* inBufferRight, float* outBufferLeft, float* outBufferRight, uint32_t numFrames, const AudioTimeStamp* timestamp);

// High-performance audio processing with SIMD optimization
OSStatus NinjamClient_processAudioSIMD(NinjamClientRef* client, const float* inBufferLeft, const float* inBufferRight, float* outBufferLeft, float* outBufferRight, uint32_t numFrames);

float NinjamClient_getOutputPeak(NinjamClientRef* client, int32_t channel);
void NinjamClient_getOutputPeaks(NinjamClientRef* client, float* left, float* right);

// Metronome control
void NinjamClient_playMetronomeTick(NinjamClientRef* client, int32_t isDownbeat);

// Channel management
void NinjamClient_removeLocalChannel(NinjamClientRef* client, int32_t channelIndex);
void NinjamClient_setLocalChannelState(NinjamClientRef* client, int32_t index, float volume, float pan, int32_t mute, int32_t solo);
const char* NinjamClient_getLocalChannelName(NinjamClientRef* client, int32_t channelIndex);
void NinjamClient_setLocalChannelInfo(NinjamClientRef* client, int32_t channelIndex, const char* name, int32_t setsrcch, int32_t srcch, int32_t setxmit, int32_t xmit, int32_t setflags, int32_t flags);
void NinjamClient_getLocalChannelPeaks(NinjamClientRef* client, int32_t channelIndex, float* left, float* right);
void NinjamClient_syncWithServerClock(NinjamClientRef* client);

void NinjamClient_submitAudioData(NinjamClientRef* client, int32_t channelIndex, const float* data, int32_t numFrames);
void NinjamClient_submitAudioDataForSync(NinjamClientRef* client, int32_t channelIndex, const float* data, int32_t numFrames);


// User channel management
void NinjamClient_subscribeToAllChannel(NinjamClientRef* client);
int32_t NinjamClient_getUserChannelState(NinjamClientRef* client, const char* username, int32_t channelIndex, float* volume,
                                         float* pan, int32_t* mute, int32_t* subscribed, bool* isStereo, int32_t* channelPairIndex);
int32_t NinjamClient_setUserChannelState(NinjamClientRef* client, const char* username, int32_t channelIndex, float* volume, float* pan, int32_t* mute, int32_t* subscribed, int32_t* solo);
void NinjamClient_getUserChannelPeaks(NinjamClientRef* client, const char* username, int32_t channelIndex, float* left, float* right);
int32_t NinjamClient_isUserSoloed(NinjamClientRef* client, const char* username);

// Volume controls
void NinjamClient_setMasterVolume(NinjamClientRef* client, float volume, float pan, int32_t mute);
void NinjamClient_setMetronome(NinjamClientRef* client, float volume, int32_t mute, float pan, int32_t channelIndex);

// Chat functionality
void NinjamClient_sendChatMessage(NinjamClientRef* client, const char* message);
void NinjamClient_sendPrivateMessage(NinjamClientRef* client, const char* username, const char* message);
void NinjamClient_sendAdminMessage(NinjamClientRef* client, const char* message);

// User info
const char* NinjamClient_getLocalUserName(NinjamClientRef* client);
const char* NinjamClient_getUserName(NinjamClientRef* client, int32_t index);
const char* NinjamClient_getUserChannelName(NinjamClientRef* client, const char* username, int32_t channelIndex);

const char** NinjamClient_getRemoteUserNames(NinjamClientRef* client, int* count);
void NinjamClient_freeRemoteUserNames(const char** userNames, int count);
void NinjamClient_invalidateUsersCache(NinjamClientRef* client);
int NinjamClient_getUserChannelCount(NinjamClientRef* client, const char* username);
void NinjamClient_setRemoteChannelVolume(NinjamClientRef* client, const char* username, int channelIndex, float volume);
void NinjamClient_setLocalChannelVolume(NinjamClientRef* client, int channelIndex, float volume);


// Session info
int32_t NinjamClient_getBPM(NinjamClientRef* client);
int32_t NinjamClient_getBPI(NinjamClientRef* client);
double NinjamClient_getIntervalPosition(NinjamClientRef* client);
const char* NinjamClient_getErrorString(NinjamClientRef* client);
const char* NinjamClient_getLocalUserName(NinjamClientRef* client);

// Callbacks
typedef void (*MessageCallback)(uint16_t type, const uint8_t* data, int32_t size);
typedef void (*ConnectedCallback)(void);
typedef void (*DisconnectedCallback)(int32_t reason);
typedef int32_t (*LicenseCallback)(const char* text);
typedef void (*ChatCallback)(const char* username, const char* message);
typedef void (*IntervalCallback)(int32_t bpm, int32_t bpi);
// eventType: 0=begin, 1=data, 2=end
typedef void (*RawDataRecvCallback)(int32_t eventType, const uint8_t* guid,
                                     uint32_t fourcc, const char* username,
                                     int32_t chidx, const void* data, int32_t dataLen);

void NinjamClient_setCallback(NinjamClientRef* client, MessageCallback callback);
void NinjamClient_setOnConnected(NinjamClientRef* client, ConnectedCallback callback);
void NinjamClient_setOnDisconnected(NinjamClientRef* client, DisconnectedCallback callback);
void NinjamClient_setLicenseCallback(NinjamClientRef* client, LicenseCallback callback);
void NinjamClient_respondToLicense(int accepted);
void NinjamClient_cancelPendingLicense(void);
void NinjamClient_setChatCallback(NinjamClientRef* client, ChatCallback callback);
void NinjamClient_setIntervalCallback(NinjamClientRef* client, IntervalCallback callback);

// Raw data channel API
void NinjamClient_setRawDataCallback(NinjamClientRef* client, RawDataRecvCallback callback);
void NinjamClient_rawDataSendBegin(NinjamClientRef* client, uint8_t outGuid[16], uint32_t fourcc, int32_t chidx, int32_t estsize);
void NinjamClient_rawDataSendWrite(NinjamClientRef* client, const uint8_t guid[16], const void* data, int32_t dataLen, int32_t isEnd);

// Audio interval swap notification — fires when audio starts playing a new interval
typedef void (*IntervalSwapCallback)(void);
void NinjamClient_setIntervalSwapCallback(NinjamClientRef* client, IntervalSwapCallback callback);

// Video channel management — interval BEGIN/END driven from C++ on_new_interval()
void NinjamClient_setVideoChannel(NinjamClientRef* client, int32_t chidx, uint32_t fourcc);
void NinjamClient_stopVideoChannel(NinjamClientRef* client);
void NinjamClient_queueVideoFrame(NinjamClientRef* client, const void* data, int32_t len);
void NinjamClient_setVideoSPSPPS(NinjamClientRef* client, const void* data, int32_t len);

// Video frame ready callback — called from AudioProc() to deliver individual frames
// at audio clock rate. frameIndex 0 = SPS/PPS, 1..N = H.264 frames.
typedef void (*VideoFrameReadyCallback)(const char* username, int32_t chidx,
                                         uint32_t fourcc, int32_t frameIndex, int32_t totalFrames,
                                         const void* data, int32_t dataLen);
void NinjamClient_setVideoFrameReadyCallback(NinjamClientRef* client, VideoFrameReadyCallback callback);

struct NinjamClientRef {
    void* adapter; // Changed from NJClientAdapter to NinjamClientAdapter
    MessageCallback messageCallback;
    ConnectedCallback connectedCallback;
    DisconnectedCallback disconnectedCallback;
    LicenseCallback licenseCallback;
    ChatCallback chatCallback;
    IntervalCallback intervalCallback;
    RawDataRecvCallback rawDataCallback;
};


#ifdef __cplusplus
} // extern "C++"
#endif

#endif // NINJAM_CLIENT_BRIDGE_H
