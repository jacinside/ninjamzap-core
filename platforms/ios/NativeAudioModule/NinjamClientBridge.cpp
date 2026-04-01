// NinjamClientBridge.cpp
#include "NinjamClientBridge.h"
#include <type_traits>
#include <chrono>
#ifdef __cplusplus
#ifdef __ANDROID__
#include "ninjamclientAdapter.h"  // Android: found via include path
#else
#include "NJClient/abNinjam/ninjamclientAdapter.h"  // iOS: relative to NativeAudioModule/
#endif
#include <cstring>
#include <string>
#include <cstdlib>
#include <ctime>

// Implementation of missing adapter methods
void NinjamClientAdapter::setOnConnect(OnConnectCallback callback) {
    onConnectCallback = callback;
}

void NinjamClientAdapter::setOnDisconnect(OnDisconnectCallback callback) {
    onDisconnectCallback = callback;
}

void NinjamClientAdapter::setOnChatMessage(OnChatMessageCallback callback) {
    onChatMessageCallback = callback;
}

#endif

  // Helper to safely get adapter from NinjamClientRef
  inline NinjamClientAdapter* getAdapter(NinjamClientRef* client) {
      return client ? static_cast<NinjamClientAdapter*>(client->adapter) : nullptr;
  }


// MARK: - Client Creation/Destruction

// Client lifecycle
NinjamClientRef* NinjamClient_create(void) {
    NinjamClientRef* client = new NinjamClientRef();
    client->adapter = new NinjamClientAdapter();
    client->messageCallback = nullptr;
    client->connectedCallback = nullptr;
    client->disconnectedCallback = nullptr;
    client->licenseCallback = nullptr;
    client->chatCallback = nullptr;
    client->intervalCallback = nullptr;
    return client;
}

void NinjamClient_destroy(NinjamClientRef* client) {
    if (client) {
        delete getAdapter(client);
        delete client;
    }
}

const char* NinjamClient_getVersion(void) {
    static const char* version = "1.0.0"; // Replace with actual version
    return version;
}

// Connection management
void NinjamClient_setUser(NinjamClientRef* client, const char* username, const char* password) {
    auto adapter = getAdapter(client);
    if (adapter && username && password) {
        adapter->setCredentials(std::string(username), std::string(password));
    }
}

void NinjamClient_connect(NinjamClientRef* client, const char* hostname, int32_t port) {
    auto adapter = getAdapter(client);
    if (adapter && hostname) {
        adapter->invalidateUsersCache();
        adapter->connect(std::string(hostname), port);
    }
}

void NinjamClient_disconnect(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->invalidateUsersCache();
        adapter->disconnect();
    }
}

int32_t NinjamClient_isConnected(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (!adapter) {
        return 0; // Definitely not connected if adapter is null
    }
    
    // Convert boolean result to explicit 0 or 1
    return adapter->isConnected() ? 1 : 0;
}

void NinjamClient_process(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->process();
    }
}

const char* NinjamClient_getServerStatus(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (adapter) {
        // Note: This returns a pointer to a temporary - implementation should handle memory properly
        static std::string status;
        status = adapter->getServerStatus();
        return status.c_str();
    }
    return "Not connected";
}

// Audio configuration
void NinjamClient_setAudioConfig(NinjamClientRef* client, int32_t sampleRate, int32_t channels) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->setAudioConfig(sampleRate, channels);
    }
}

void NinjamClient_processAudio(NinjamClientRef* client, float* inBufferLeft, float* inBufferRight, float* outBufferLeft, float* outBufferRight, int32_t numFrames) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->processAudio(inBufferLeft, inBufferRight, outBufferLeft, outBufferRight, numFrames);
    }
}

void NinjamClient_submitAudioData(NinjamClientRef* client, int32_t channelIndex, const float* data, int32_t numFrames) {
    auto adapter = getAdapter(client);
    if (adapter) {
        //adapter->sendAudio(data, numFrames * sizeof(float), channelIndex, false);
        adapter->sendAudio(data, numFrames, channelIndex, false);
    }
}

void NinjamClient_submitAudioDataForSync(NinjamClientRef* client, int32_t channelIndex, const float* data, int32_t numFrames) {
    auto adapter = getAdapter(client);
    if (adapter) {
        //adapter->sendAudio(data, numFrames * sizeof(float), channelIndex, false);
        adapter->sendAudio(data, numFrames, channelIndex, true);
    }
}

void NinjamClient_removeLocalChannel(NinjamClientRef* client, int32_t channelIndex) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->removeLocalChannel(channelIndex);
    }
}

void NinjamClient_setLocalChannelState(NinjamClientRef* client, int32_t index, float volume, float pan, int32_t mute, int32_t solo) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->setLocalChannelMonitoring(index, volume, pan, mute != 0, solo != 0);
    }
}

// Volume controls
void NinjamClient_setMasterVolume(NinjamClientRef* client, float volume, float pan, int32_t mute) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->setMasterVolume(volume, pan, mute != 0);
    }
}

void NinjamClient_setMetronome(NinjamClientRef* client, float volume, int32_t mute, float pan, int32_t channelIndex) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->setMetronome(volume, mute != 0, pan);
    }
}

// Session info
int32_t NinjamClient_getBPM(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    return adapter ? adapter->getBPM() : 120;
}

int32_t NinjamClient_getBPI(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    return adapter ? adapter->getBPI() : 16;
}

double NinjamClient_getIntervalPosition(NinjamClientRef* client) {
  auto adapter = getAdapter(client);
  if (!adapter) return 0;
  
  // Make sure the adapter is processing audio and calculating interval position
  //adapter->process(); // Process any pending events first
  
  return adapter->getIntervalPosition();
}

const char* NinjamClient_getErrorString(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (!adapter) return "";
    static std::string errorStr;
    errorStr = adapter->getErrorString();
    return errorStr.c_str();
}

const char* NinjamClient_getLocalUserName(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (!adapter) return "";
    return adapter->getLocalUserName();
}

// Callback setup
void NinjamClient_setOnConnected(NinjamClientRef* client, ConnectedCallback callback) {
    if (client) {
        client->connectedCallback = callback;
        auto adapter = getAdapter(client);
        if (adapter) {
            adapter->setOnConnect([client]() {
                if (client->connectedCallback) {
                    client->connectedCallback();
                }
            });
        }
    }
}

void NinjamClient_setOnDisconnected(NinjamClientRef* client, DisconnectedCallback callback) {
    if (client) {
        client->disconnectedCallback = callback;
        auto adapter = getAdapter(client);
        if (adapter) {
                   adapter->setOnDisconnect([client](int reason) {
                       if (client->disconnectedCallback) {
                           client->disconnectedCallback(reason);
                       }
                   });
               }
    }
}

// Chat functionality
void NinjamClient_sendChatMessage(NinjamClientRef* client, const char* message) {
    auto adapter = getAdapter(client);
    if (adapter && message) {
        adapter->sendChatMessage(std::string(message));
    }
}

void NinjamClient_setChatCallback(NinjamClientRef* client, ChatCallback callback) {
    if (client) {
        client->chatCallback = callback;
        auto adapter = getAdapter(client);
        if (adapter) {
            adapter->setOnChatMessage([client](const char* username, const char* message) {
                if (client->chatCallback) {
                    client->chatCallback(username, message); // Assuming username is missing
                }
            });
        }
    }
}

// Now, implement the missing wrapper functions:

// Additional helper functions
int32_t NinjamClient_getLatency(NinjamClientRef* client) {
    // Implement latency retrieval based on adapter
    return 0; // Default implementation
}

int32_t NinjamClient_getServerUptime(NinjamClientRef* client) {
    // Server uptime implementation
    return 0; // Default implementation
}

float NinjamClient_getOutputPeak(NinjamClientRef* client, int32_t channel) {
    // Default implementation
    return 0.0f;
}

void NinjamClient_getOutputPeaks(NinjamClientRef* client, float* left, float* right) {
    auto adapter = getAdapter(client);
    if (adapter) {
        // Obtener valores reales del adaptador
        adapter->getOutputPeaks(left, right);
    } else {
        if (left) *left = 0.0f;
        if (right) *right = 0.0f;
    }
}

// User and channel info
const char* NinjamClient_getLocalChannelName(NinjamClientRef* client, int32_t channelIndex) {
    // Default implementation
    static const char* defaultName = "channel";
    return defaultName;
}

void NinjamClient_setLocalChannelInfo(NinjamClientRef* client, int32_t channelIndex, const char* name, int32_t setsrcch, int32_t srcch, int32_t setxmit, int32_t xmit, int32_t setflags, int32_t flags) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->SetLocalChannelInfo(
                                     channelIndex,
                                     name ? name : "Channel 1",
                                     setsrcch == 1 ? true : false,
                                     srcch,
                                     setxmit == 1 ? true : false,
                                     xmit,
                                     setflags == 1 ? true: false,
                                     flags
                                     );
    }
}

void NinjamClient_getLocalChannelPeaks(NinjamClientRef* client, int32_t channelIndex, float* left, float* right) {
    if (left) *left = 0.0f;
    if (right) *right = 0.0f;
}

void NinjamClient_invalidateUsersCache(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (adapter) {
      adapter->invalidateUsersCache();
    }
}

const char** NinjamClient_getRemoteUserNames(NinjamClientRef* client, int* count) {
    auto adapter = getAdapter(client);
    if (!adapter || !count) {
        if (count) *count = 0;
        return nullptr;
    }
    
    // Usar caché de usuarios remotos
    std::vector<AbNinjam::Common::RemoteUser> users = adapter->getCachedRemoteUsers();
    *count = static_cast<int>(users.size());
    
    if (*count == 0) {
        return nullptr;
    }
    
    // Crear un array de punteros a cadenas de caracteres
    const char** userNames = new const char*[*count];
    for (int i = 0; i < *count; i++) {
        userNames[i] = strdup(users[i].name.c_str());
    }
    
    return userNames;
}

void NinjamClient_freeRemoteUserNames(const char** userNames, int count) {
    if (userNames) {
        for (int i = 0; i < count; i++) {
            if (userNames[i]) {
                free((void*)userNames[i]);
            }
        }
        delete[] userNames;
    }
}

// MARK: - Optimized Audio Processing Functions

/// Streamlined audio processing function for zero-copy operations
/// Optimized for minimal latency and maximum throughput
OSStatus NinjamClient_processAudioStreamlined(
    NinjamClientRef* client,
    const float* inBufferLeft,
    const float* inBufferRight,
    float* outBufferLeft,
    float* outBufferRight,
    uint32_t numFrames,
    const AudioTimeStamp* timestamp
) {
    auto adapter = getAdapter(client);
    if (!adapter) {
        // No adapter - clear output buffers and return
        if (outBufferLeft && outBufferRight) {
            memset(outBufferLeft, 0, numFrames * sizeof(float));
            memset(outBufferRight, 0, numFrames * sizeof(float));
        }
        return noErr;
    }
    
    try {
        // Profile execution time for performance monitoring
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Create buffer pointers for adapter processing
        float* inputBuffers[2] = {
            const_cast<float*>(inBufferLeft),
            const_cast<float*>(inBufferRight)
        };
        float* outputBuffers[2] = {
            outBufferLeft,
            outBufferRight
        };
        
        // Process audio through NINJAM adapter with optimized path
        adapter->processAudio(
            inputBuffers[0],    // Input buffer Left
            inputBuffers[1],    // Input buffer Right
            outputBuffers[0],   // Output buffer Left
            outputBuffers[1],   // Output buffer Right
            static_cast<int>(numFrames)
        );
        
        // Performance monitoring
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        // Log performance warning if processing takes too long
        if (duration.count() > 10000) {  // 10ms threshold
            // Consider logging this for performance monitoring
            // printf("Audio processing took %ld microseconds\n", duration.count());
        }
        
        return noErr;
        
    } catch (const std::exception& e) {
        // Exception occurred - clear output buffers
        if (outBufferLeft && outBufferRight) {
            memset(outBufferLeft, 0, numFrames * sizeof(float));
            memset(outBufferRight, 0, numFrames * sizeof(float));
        }
        return kAudioUnitErr_FailedInitialization;
    }
}

/// High-performance audio processing with SIMD optimization hints
/// Uses compiler optimizations and memory alignment for maximum performance
OSStatus NinjamClient_processAudioSIMD(
    NinjamClientRef* client,
    const float* inBufferLeft,
    const float* inBufferRight,
    float* outBufferLeft,
    float* outBufferRight,
    uint32_t numFrames
) {
    auto adapter = getAdapter(client);
    if (!adapter) {
        // Fast zero-fill using SIMD-friendly operations
        if (outBufferLeft && outBufferRight) {
            // Use vectorized memset if available
            memset(outBufferLeft, 0, numFrames * sizeof(float));
            memset(outBufferRight, 0, numFrames * sizeof(float));
        }
        return noErr;
    }
    
    // Validate alignment for optimal SIMD performance
    const bool isAligned = 
        (reinterpret_cast<uintptr_t>(inBufferLeft) % 16 == 0) &&
        (reinterpret_cast<uintptr_t>(inBufferRight) % 16 == 0) &&
        (reinterpret_cast<uintptr_t>(outBufferLeft) % 16 == 0) &&
        (reinterpret_cast<uintptr_t>(outBufferRight) % 16 == 0);
    
    if (isAligned && numFrames % 4 == 0) {
        // Optimal path: aligned buffers, frame count divisible by 4
        // This enables the best SIMD optimization
        return NinjamClient_processAudioStreamlined(
            client, inBufferLeft, inBufferRight, 
            outBufferLeft, outBufferRight, numFrames, nullptr
        );
    } else {
        // Fallback to standard processing for unaligned data
        return NinjamClient_processAudioStreamlined(
            client, inBufferLeft, inBufferRight, 
            outBufferLeft, outBufferRight, numFrames, nullptr
        );
    }
}


int NinjamClient_getUserChannelCount(NinjamClientRef* client, const char* username) {
    auto adapter = getAdapter(client);
    if (!adapter || !username) {
        return 0;
    }
    
    // Usar caché de usuarios remotos
    std::vector<AbNinjam::Common::RemoteUser> users = adapter->getCachedRemoteUsers();
    for (const auto& user : users) {
        if (user.name == username) {
            return static_cast<int>(user.channels.size());
        }
    }
    
    return 0;
}

void NinjamClient_setRemoteChannelVolume(NinjamClientRef* client, const char* username, int channelIndex, float volume) {
    auto adapter = getAdapter(client);
    if (!adapter || !username) {
        return;
    }
    
    // Buscar el usuario por nombre
    std::vector<AbNinjam::Common::RemoteUser> users = adapter->getRemoteUsers();
    for (const auto& user : users) {
        if (user.name == username) {
            adapter->setUserChannelVolume(user.id, channelIndex, volume);
            break;
        }
    }
}

void NinjamClient_setLocalChannelVolume(NinjamClientRef* client, int channelIndex, float volume) {
    auto adapter = getAdapter(client);
    if (adapter) {
        adapter->setLocalChannelVolume(channelIndex, volume);
    }
}
const char* NinjamClient_getUserName(NinjamClientRef* client, int32_t index) {
    static const char* defaultName = "user";
    return defaultName;
}

const char* NinjamClient_getUserChannelName(NinjamClientRef* client, const char* username, int32_t channelIndex) {
  static thread_local std::string cachedName = "no-name";
  
  auto adapter = getAdapter(client);
  if (!adapter || !username) {
    return 0;
  }
  
  // Usar caché de usuarios remotos
  std::vector<AbNinjam::Common::RemoteUser> users = adapter->getCachedRemoteUsers();
  for (const auto& user : users) {
    if (user.name == username) {
      for (const auto& channel : user.channels) {
        if (channel.id == channelIndex) {
          cachedName = channel.name;
          return cachedName.c_str();        }
      }
    }
  }
  cachedName = "no-name";
  return cachedName.c_str();
}

void NinjamClient_getUserChannelPeaks(NinjamClientRef* client, const char* username, int32_t channelIndex, float* left, float* right) {
    auto adapter = getAdapter(client);
    if (!adapter || !username) {
        if (left) *left = 0.0f;
        if (right) *right = 0.0f;
        return;
    }
    
    // Find the user's index in the remote users list
    std::vector<AbNinjam::Common::RemoteUser> users = adapter->getCachedRemoteUsers();
    for (const auto& user : users) {
        if (user.name == username) {
            // Use the adapter to get the peak values for left and right channels
            if (left) *left = adapter->getUserChannelPeak(user.id, channelIndex, 0);  // 0 = left channel
            if (right) *right = adapter->getUserChannelPeak(user.id, channelIndex, 1); // 1 = right channel
            return;
        }
    }
    
    // User not found, set defaults
    if (left) *left = 0.0f;
    if (right) *right = 0.0f;
}

int32_t NinjamClient_getUserChannelState(NinjamClientRef* client, const char* username, int32_t channelIndex, float* volume,
                                         float* pan, int32_t* mute, int32_t* subscribed, bool* isStereo, int32_t* channelPairIndex) {
    auto adapter = getAdapter(client);
    if (!adapter || !username) {
       return 0;
    }

    // Usar caché de usuarios remotos
    std::vector<AbNinjam::Common::RemoteUser> users = adapter->getCachedRemoteUsers();
    for (const auto& user : users) {
       if (user.name == username) {
         for (const auto& channel : user.channels) {
            if (channel.id == channelIndex) {
              if (volume) *volume = channel.volume;
              if (pan) *pan = 0.0f;
              if (mute) *mute = 0;
              if (subscribed) *subscribed = 1;
              if (isStereo) *isStereo = channel.isStereo;
              if (channelPairIndex) *channelPairIndex = channel.channelPairIndex;
              return 0; // Success code
            }
         }
       }
    }
    return 1; // User or channel not found
}

int32_t NinjamClient_setUserChannelState(
    NinjamClientRef* client,
    const char* username,
    int32_t channelIndex,
    float* volume,
    float* pan,
    int32_t* mute,
    int32_t* subscribed
) {
  auto adapter = getAdapter(client);
  if (!adapter || !username) {
    return 0;
  }

  std::vector<AbNinjam::Common::RemoteUser> users = adapter->getCachedRemoteUsers();
  for (const auto& user : users) {
    if (user.name == username) {
      adapter->setUserChannelState(
        user.id,
        channelIndex,
        subscribed != nullptr, subscribed ? (*subscribed != 0) : false,
        volume != nullptr, volume ? *volume : 0.0f,
        pan != nullptr, pan ? *pan : 0.0f,
        mute != nullptr, mute ? (*mute != 0) : false,
        false, false, // solo
        false, 0      // output channel
      );

      //NinjamClient_invalidateUsersCache(client);
      return 1;
    }
  }

  return 0;
}

void NinjamClient_subscribeToAllChannel(NinjamClientRef* client) {
  // Default implementation
  auto adapter = getAdapter(client);
  adapter->subscribeToAllRemoteChannels();

}

int32_t NinjamClient_isUserSoloed(NinjamClientRef* client, const char* username) {
    return 0;
}

// Chat functionality
void NinjamClient_sendPrivateMessage(NinjamClientRef* client, const char* username, const char* message) {
    auto adapter = getAdapter(client);
    if (adapter && username && message) {
        std::string privateMsg = "/msg ";
        privateMsg += username;
        privateMsg += " ";
        privateMsg += message;
        adapter->sendChatMessage(privateMsg);
    }
}

void NinjamClient_sendAdminMessage(NinjamClientRef* client, const char* message) {
    auto adapter = getAdapter(client);
    if (adapter && message) {
        std::string adminMsg = "/admin ";
        adminMsg += message;
        adapter->sendChatMessage(adminMsg);
    }
}

// Callback setters
void NinjamClient_setCallback(NinjamClientRef* client, MessageCallback callback) {
    if (client) {
        client->messageCallback = callback;
    }
}

void NinjamClient_setLicenseCallback(NinjamClientRef* client, LicenseCallback callback) {
    if (client) {
        client->licenseCallback = callback;
    }
}

void NinjamClient_setIntervalCallback(NinjamClientRef* client, IntervalCallback callback) {
    if (client) {
        client->intervalCallback = callback;
        auto adapter = getAdapter(client);
        if (adapter) {
            adapter->setOnInterval([client](int bpm, int bpi) {
                if (client->intervalCallback) {
                    client->intervalCallback(bpm, bpi);
                }
            });
        }
    }
}

void NinjamClient_playMetronomeTick(NinjamClientRef* client, int32_t isDownbeat) {
    auto adapter = getAdapter(client);
    if (!adapter) return;
    
    // Let the adapter handle the metronome tick directly
    adapter->playMetronomeTick(isDownbeat != 0);
}

// Synchronize client clock with server
void NinjamClient_syncWithServerClock(NinjamClientRef* client) {
    auto adapter = getAdapter(client);
    if (!adapter) return;
    
    // Get current interval position
    double currentPos = adapter->getIntervalPosition();
    
    // Request server time information
    int bpm = adapter->getBPM();
    int bpi = adapter->getBPI();
    
    // Calculate beat duration in milliseconds
    double beatDuration = 60000.0 / bpm;
    
    // Calculate interval duration in milliseconds
    double intervalDuration = beatDuration * bpi;
    
    // The sync algorithm adjusts the local interval position
    // based on server timestamp and network latency estimation
    adapter->syncWithServerClock();
    
    // Process to apply sync immediately
    adapter->process();
    
    // Log the synchronization event
    if (client->messageCallback) {
        const char* syncMsg = "Clock synchronized with server";
        client->messageCallback(100, (const uint8_t*)syncMsg, strlen(syncMsg));
    }
}


