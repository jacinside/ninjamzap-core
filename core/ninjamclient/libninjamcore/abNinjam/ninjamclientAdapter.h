// ninjamclientAdapter.h
#ifndef NINJAMCLIENTADAPTER_H
#define NINJAMCLIENTADAPTER_H

#include "include/ninjamclient.h"
#include "include/remoteuser.h"

#ifdef __cplusplus
#include <string>
#include <functional>
#include <vector>
#include <cmath>
#endif

// Simple sine wave generator for metronome sounds
struct MetronomeSound {
    static const int SAMPLE_RATE = 48000;
    static const int TICK_DURATION_MS = 50;
    static const int SAMPLES = (SAMPLE_RATE * TICK_DURATION_MS) / 1000;
  
    float downbeatSamples[SAMPLES];
    float regularSamples[SAMPLES];
    bool initialized = false;
    
    void initialize() {
        if (initialized) return;
        
        // Generate a higher frequency sine wave for downbeat (1000Hz)
        for (int i = 0; i < SAMPLES; i++) {
            float phase = static_cast<float>(i) / SAMPLE_RATE;
            float amplitude = 1.0f - (static_cast<float>(i) / SAMPLES); // Simple fade out
            downbeatSamples[i] = amplitude * std::sin(2.0f * M_PI * 1000.0f * phase);
        }
        
        // Generate a lower frequency sine wave for regular beats (800Hz)
        for (int i = 0; i < SAMPLES; i++) {
            float phase = static_cast<float>(i) / SAMPLE_RATE;
            float amplitude = 0.8f - (static_cast<float>(i) / SAMPLES); // Quieter & fade out
            regularSamples[i] = amplitude * std::sin(2.0f * M_PI * 800.0f * phase);
        }
        
        initialized = true;
    }
    
    const float* getSamples(bool isDownbeat) const {
        return isDownbeat ? downbeatSamples : regularSamples;
    }
    
    int getSampleCount() const {
        return SAMPLES;
    }
};


// Callback type definitions
using OnConnectCallback = std::function<void()>;
using OnDisconnectCallback = std::function<void(int reason)>;
using OnChatMessageCallback = std::function<void(const char* username, const char* message)>;
using OnIntervalCallback = std::function<void(int bpm, int bpi)>;
using OnRawDataCallback = std::function<void(int eventType, const unsigned char *guid,
                                              unsigned int fourcc, const char *username,
                                              int chidx, const void *data, int dataLen)>;

class NinjamClientAdapter {
public:
    NinjamClientAdapter();
    ~NinjamClientAdapter();
    
    // Connection management
    void setCredentials(const std::string& username, const std::string& password);
    void connect(const std::string& host, int port);
    void disconnect();
    bool isConnected() const;
    void process();
    
    // Audio configuration and processing
    void setAudioConfig(int sampleRate, int channels);
    void processAudio(float* inBufferLeft,float* inBufferRight,float* outBufferLeft,float* outBufferRight,int numFrames);
//    void processAudio(float* inBuffer, float* outBuffer, int numFrames);
    void sendAudio(const float* data, int size, int channelIndex, bool forSync = false);
    
    // Callbacks
    void setOnConnect(OnConnectCallback callback);
    void setOnDisconnect(OnDisconnectCallback callback);
    void setOnChatMessage(OnChatMessageCallback callback);
    void setOnInterval(OnIntervalCallback callback);
    void setOnRawData(OnRawDataCallback callback);
    void setIntervalSwapCallback(std::function<void()> callback);
    void setVideoFrameReadyCallback(NJClient::VideoFrameReadyCallback callback);

    // Raw data send
    void rawDataSendBegin(unsigned char outGuid[16], unsigned int fourcc, int chidx, int estsize);
    void rawDataSendWrite(const unsigned char guid[16], const void *data, int dataLen, bool isEnd);

    // Video channel management (delegates to NJClient)
    void setVideoChannel(int chidx, unsigned int fourcc);
    void stopVideoChannel();
    void queueVideoFrame(const void *data, int len);
    void setVideoSPSPPS(const void *data, int len);
  
    // Channel management
    void removeLocalChannel(int channelIndex);
    void setLocalChannelMonitoring(int index, float volume, float pan, bool mute, bool solo);
    void SetLocalChannelInfo(int index, const char* name, bool setsrcch, int srcch, bool setxmit, bool xmit, bool setflags, int flags);
    void subscribeToAllRemoteChannels();
    void getOutputPeaks(float* left, float* right);
  
    // User management
    void sendUserMask(unsigned int mask);
    
    // Metronome control
    void setMetronome(float volume, bool mute, float pan);
    
    // Master volume controls
    void setMasterVolume(float volume, float pan, bool mute);
    
    // Chat functionality
    void sendChatMessage(const std::string& message);
    
    // Session info
    int getBPM();
    int getBPI();
    double getIntervalPosition();
    void syncWithServerClock();
    
    // User and channel information
    std::vector<AbNinjam::Common::RemoteUser> getRemoteUsers();
    void setUserChannelVolume(int userId, int channelId, float volume);
    void setUserChannelState(int userId, int channelId,bool setsub, bool sub, bool setvol, float vol, bool setpan, float pan, bool setmute, bool mute, bool setsolo, bool solo, bool setoutch, int outchannel);
    void setLocalChannelVolume(int channelId, float volume);
    float getUserChannelPeak(int useridx, int channelidx, int whichch);
    
    // Server information
    std::string getServerStatus();
    std::string getErrorString();
    const char* getLocalUserName();
    bool isServerVideoSupported();

    // Metronome playback
    void playMetronomeTick(bool isDownbeat);
  
    // Método para invalidar el caché cuando sea necesario
    void invalidateUsersCache() {
        usersCacheValid = false;
    }

    // Método para obtener usuarios (usando caché si es posible)
    std::vector<AbNinjam::Common::RemoteUser> getCachedRemoteUsers() {
        if (!usersCacheValid) {
            usersCache = client->getRemoteUsers();
            usersCacheValid = true;
        }
        return usersCache;
    }
  // Callbacks
  OnConnectCallback onConnectCallback;
  OnDisconnectCallback onDisconnectCallback;
  OnChatMessageCallback onChatMessageCallback;
  OnIntervalCallback onIntervalCallback;
  OnRawDataCallback onRawDataCallback;
  std::function<void()> intervalSwapCb;


private:
    // Clock sync variables
    double lastSyncTime = 0.0;
    double intervalStartTime = 0.0;
    std::vector<double> clockDriftHistory = std::vector<double>(10, 0.0);
    int clockDriftIndex = 0;
    int numFrames = 0;
  
    // Interval callback
    int lastReportedBPM = -1;
    int lastReportedBPI = -1;
    double intervalCheckTimer = 0.0;
  
    // Caché para usuarios remotos
    std::vector<AbNinjam::Common::RemoteUser> usersCache;
    bool usersCacheValid = false;
    
  // Private helper methods
    void setupInitialState();
    
    // Member variables
    AbNinjam::Common::NinjamClient* client;
    std::string username;
    std::string password;
    bool connected;
    
    // Audio settings
    int sampleRate;
    int numChannels;
    float** inputBuffer;
    float** outputBuffer;
    
    // Metronome settings
    bool metronomeEnabled;
    float metronomeVolume;
    float metronomePan;
    
    // Master settings
    float masterVolume;
    float masterPan;
    bool masterMute;
    
    // Interval tracking
    double lastIntervalPosition;
    double getTimeInSeconds();
};

#endif // NINJAMCLIENTADAPTER_H
