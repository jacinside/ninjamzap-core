// ninjamclientAdapter.cpp
#include "ninjamclientAdapter.h"
// include SumpleProfiler
//#include "SimpleProfiler.h"


//#ifdef __cplusplus
#include <cstring>
#include <memory>
#include <mutex>
//#endif


// Static C callback that routes raw data events to the adapter's std::function
static void rawDataCCallback(void *userData, int eventType,
                              const unsigned char *guid, unsigned int fourcc,
                              const char *username, int chidx,
                              const void *data, int dataLen)
{
    NinjamClientAdapter *adapter = static_cast<NinjamClientAdapter *>(userData);
    if (adapter && adapter->onRawDataCallback) {
        adapter->onRawDataCallback(eventType, guid, fourcc, username, chidx, data, dataLen);
    }
}

NinjamClientAdapter::NinjamClientAdapter()
    : client(new AbNinjam::Common::NinjamClient())
    //, nextChannelIndex(0)
    , connected(false)
    , sampleRate(48000)
    , numChannels(2)
    , inputBuffer(nullptr)
    , outputBuffer(nullptr)
    , metronomeEnabled(true)
    , metronomeVolume(0.5f)
    , metronomePan(0.0f)
    , masterVolume(1.0f)
    , masterPan(0.0f)
    , masterMute(false)
    , onIntervalCallback(nullptr)
    , onRawDataCallback(nullptr)
    , lastIntervalPosition(0.0) {
    
    setupInitialState();
}

NinjamClientAdapter::~NinjamClientAdapter() {
    if (connected) {
        disconnect();
    }
    
    // Clean up audio buffers
    if (inputBuffer) {
        for (int i = 0; i < numChannels; i++) {
            delete[] inputBuffer[i];
        }
        delete[] inputBuffer;
    }
    
    if (outputBuffer) {
        for (int i = 0; i < numChannels; i++) {
            delete[] outputBuffer[i];
        }
        delete[] outputBuffer;
    }
    
    delete client;
}

static void njClientChatCallback(void* obj, NJClient* client, const char** parms, int nparms) {
  NinjamClientAdapter* adapter = static_cast<NinjamClientAdapter*>(obj);

  if (nparms < 1 || !adapter || !adapter->onChatMessageCallback || !parms[0]) return;

  const char* type = parms[0];

  if (!strcmp(type, "MSG")) {
    // Regular/server message: parms[1] = sender (empty = server), parms[2] = text
    const char* user = parms[1] ? parms[1] : "";
    const char* text = parms[2] ? parms[2] : "";
    adapter->onChatMessageCallback(user, text);
  }
  else if (!strcmp(type, "PRIVMSG")) {
    // Private message: parms[1] = sender, parms[2] = text
    const char* user = parms[1] ? parms[1] : "";
    const char* text = parms[2] ? parms[2] : "";
    adapter->onChatMessageCallback(user, text);
  }
  else if (!strcmp(type, "JOIN")) {
    // User joined: parms[1] = username
    const char* user = parms[1] ? parms[1] : "unknown";
    std::string joinMsg = std::string(user) + " has joined the server";
    adapter->onChatMessageCallback("", joinMsg.c_str());
  }
  else if (!strcmp(type, "PART")) {
    // User left: parms[1] = username
    const char* user = parms[1] ? parms[1] : "unknown";
    std::string partMsg = std::string(user) + " has left the server";
    adapter->onChatMessageCallback("", partMsg.c_str());
  }
  else if (!strcmp(type, "TOPIC")) {
    // Topic: parms[1] = who set it, parms[2] = topic text
    const char* text = parms[2] ? parms[2] : "";
    if (text[0]) {
      std::string topicMsg = std::string("Topic is: ") + text;
      adapter->onChatMessageCallback("", topicMsg.c_str());
    }
  }
  // Ignore USERCOUNT, SESSION, and other internal types
}

void NinjamClientAdapter::setupInitialState() {
    // Setup initial channel
    client->gsNjClient()->config_play_prebuffer = -1;
    //client->gsNjClient()->SetLocalChannelInfo(0, "channel0", true, 0, false, 0, true, true, false, 0);
    //client->gsNjClient()->SetLocalChannelMonitoring(0, true, 0.0f, true, 0.0f, false, true, false, false); //muted local channel
    client->gsNjClient()->ChatMessage_Callback = njClientChatCallback;
    client->gsNjClient()->ChatMessage_User = this;
  // Setup metronome
    client->gsNjClient()->config_metronome_mute = !metronomeEnabled;
    client->gsNjClient()->config_metronome_pan = metronomePan;
    client->gsNjClient()->config_metronome = metronomeVolume;
}

void NinjamClientAdapter::setCredentials(const std::string& username, const std::string& password) {
    this->username = username;
    this->password = password;
}

void NinjamClientAdapter::connect(const std::string& host, int port) {
    if (connected) {
        disconnect();
    }
  
    std::ostringstream oss;
    oss << host << ":" << port;
    std::string hostPort = oss.str();
  
    AbNinjam::Common::ConnectionProperties properties;
    properties.gsHost() = strdup(hostPort.c_str());
    properties.gsUsername() = strdup(username.c_str());
    properties.gsPassword() = strdup(password.c_str());
    properties.gsAutoLicenseAgree() = true;
    properties.gsAutoRemoteVolume() = true;
    properties.gsAutoSyncBpm() = true;
    
    auto status = client->connect(&properties);
//  char* hostCopy = strdup(host.c_str());
//  char* userCopy = strdup(m_username.c_str());
//  char* passCopy = strdup(m_password.c_str());
//
//  m_client->Connect(hostCopy, userCopy, passCopy);

    free(properties.gsHost());
    free(properties.gsUsername());
    free(properties.gsPassword());

    if (status == AbNinjam::Common::NinjamClientStatus::ok) {
        connected = true;

        // Report BPM/BPI immediately after connection — don't wait for the
        // 1-second polling interval, which causes a stale-defaults window.
        int bpm = getBPM();
        int bpi = getBPI();
        if (onIntervalCallback && (bpm != lastReportedBPM || bpi != lastReportedBPI)) {
            onIntervalCallback(bpm, bpi);
            lastReportedBPM = bpm;
            lastReportedBPI = bpi;
        }

        if (onConnectCallback) {
            onConnectCallback();
        }
    } else {
        connected = false;
      if (onDisconnectCallback) {
        if (status == AbNinjam::Common::NinjamClientStatus::disconnected) {
          onDisconnectCallback(220);
        }
        else if (status == AbNinjam::Common::NinjamClientStatus::serverNotProvided) {
          onDisconnectCallback(221);
        }
        else if (status == AbNinjam::Common::NinjamClientStatus::licenseNotAccepted) {
          onDisconnectCallback(222);
        }
        else if (status == AbNinjam::Common::NinjamClientStatus::connectionError) {
          onDisconnectCallback(223);
        } else{
          onDisconnectCallback(0);
        }
      }
    }
}

void NinjamClientAdapter::disconnect() {
    if (connected) {
        client->disconnect();
        connected = false;
        
        if (onDisconnectCallback) {
            onDisconnectCallback(0);
        }
    }
}

bool NinjamClientAdapter::isConnected() const {
    return connected;
}

void NinjamClientAdapter::process() {
    if (connected) {
        {
            std::lock_guard<std::mutex> lock(client->gsMtx());
            client->gsNjClient()->Run();
        }
      
        int status = client->gsNjClient()->GetStatus();
        
        if (status < 0) {
          // Handle disconnection or error
          
          if (onDisconnectCallback) {
            connected = false;
            onDisconnectCallback(status);
          }
        }

        double now = getTimeInSeconds(); // usar clock_gettime o similar

        // Chequear BPM/BPI cada 1 segundo
        if (now - intervalCheckTimer > 1.0) {
            int bpm = getBPM();
            int bpi = getBPI();
            if ((bpm != lastReportedBPM || bpi != lastReportedBPI) && onIntervalCallback) {
                onIntervalCallback(bpm, bpi);
                lastReportedBPM = bpm;
                lastReportedBPI = bpi;
            }
            intervalCheckTimer = now;
        }
      }

        // Check for interval transitions
        double currentPos = getIntervalPosition();
        
      // Step 2: Interval tracking callback (optional, probably need to implement in swift
//       if (currentPos < lastIntervalPosition && lastIntervalPosition > 0.9) {
//            // Interval transition detected
//            if (onIntervalCallback) {
//                onIntervalCallback(getBPM(), getBPI());
//            }
//        }
        
        lastIntervalPosition = currentPos;
  
}

void NinjamClientAdapter::setAudioConfig(int sampleRate, int channels) {
    this->sampleRate = sampleRate;
    this->numChannels = channels;
    
    // Clean up existing buffers
    if (inputBuffer) {
        for (int i = 0; i < this->numChannels; i++) {
            delete[] inputBuffer[i];
        }
        delete[] inputBuffer;
    }
    
    if (outputBuffer) {
        for (int i = 0; i < this->numChannels; i++) {
            delete[] outputBuffer[i];
        }
        delete[] outputBuffer;
    }
    
    // Allocate new buffers
    inputBuffer = new float*[channels];
    outputBuffer = new float*[channels];
    
    for (int i = 0; i < channels; i++) {
        inputBuffer[i] = new float[8192]; // Use a reasonable buffer size
        outputBuffer[i] = new float[8192];
    }
}

void NinjamClientAdapter::processAudio(
    float* inBufferLeft,
    float* inBufferRight,
    float* outBufferLeft,
    float* outBufferRight,
    int numFrames
) {
    this->numFrames = numFrames;
    
    if (!connected || numFrames <= 0) {
        // Clear output buffers if not connected
        if (outBufferLeft) memset(outBufferLeft, 0, sizeof(float) * numFrames);
        if (outBufferRight) memset(outBufferRight, 0, sizeof(float) * numFrames);
        return;
    }
    
    // Copy input data to our channel buffers (if provided)
    if (inBufferLeft) {
        memcpy(inputBuffer[0], inBufferLeft, numFrames * sizeof(float));
    } else {
        memset(inputBuffer[0], 0, numFrames * sizeof(float));
    }
    
    if (numChannels > 1) {
        if (inBufferRight) {
            memcpy(inputBuffer[1], inBufferRight, numFrames * sizeof(float));
        } else {
            memset(inputBuffer[1], 0, numFrames * sizeof(float));
        }
    }
    
    // Process audio through NJClient
    client->audiostreamOnSamples(inputBuffer, numChannels, outputBuffer, numChannels, numFrames, sampleRate);
    
    // Copy output data to separate channel buffers
    if (outBufferLeft) {
        memcpy(outBufferLeft, outputBuffer[0], numFrames * sizeof(float));
    }
    
    if (numChannels > 1 && outBufferRight) {
        memcpy(outBufferRight, outputBuffer[1], numFrames * sizeof(float));
    }
    
    // Debug output
//    if (numFrames > 0 && outputBuffer && outputBuffer[0]) {
//        for (int i = 0; i < numFrames; ++i) {
//            if (fabs(outputBuffer[0][i]) > 0.01f) {
//                printf("🔊 Remote audio present at frame %d: %f\n", i, outputBuffer[0][i]);
//                break;
//            }
//        }
//    }
}

void NinjamClientAdapter::sendAudio(const float* data, int size, int channelIndex, bool forSync) {
    if (!connected || channelIndex < 0 || !data) return;
    
    int numFrames = size;
    if (numFrames <= 0) return;
    
    // Ensure the channel is set up for broadcasting
//    client->gsNjClient()->SetLocalChannelInfo(
//        channelIndex,
//        nullptr,         // Don't change name
//        false, 0,        // Don't change source channel
//        false, 0,        // Don't change bitrate
//        true, true,      // Set broadcasting to true
//        false, 0,        // Don't change output channel
//        false, 0         // Don't change flags
//    );
    
    // Get a pointer to the local channel's buffer queue
    // Note: This is internal NJClient implementation and the adapter would need
    // to provide a method to access the BufferQueue for a channel
    
    // For now, we'll use the AudioProc method which will handle the input properly
    // Create a temporary buffer for input data
    float** tempBuffer = new float*[1];
    tempBuffer[0] = new float[numFrames];
    memcpy(tempBuffer[0], data, numFrames * sizeof(float));
    
    // Create empty output buffer (we don't need output)
    float** emptyOut = new float*[numChannels];
    for (int i = 0; i < numChannels; i++) {
        emptyOut[i] = new float[numFrames];
        memset(emptyOut[i], 0, numFrames * sizeof(float));
    }
    
    // Process audio through NJClient which will handle queuing for broadcast
    //client->gsNjClient()->AudioProc(tempBuffer, 1, emptyOut, numChannels, numFrames, sampleRate);
  if (forSync) {
    // logs
    printf("NinjamClientAdapter::sendAudio: calling audiostreamForSync forSync=%d channel=%d numFrames=%d\n", forSync, channelIndex, numFrames);
    client->audiostreamForSync(tempBuffer, 1, emptyOut, numChannels, numFrames, sampleRate);
  } else {
    printf("NinjamClientAdapter::sendAudio: calling audiostreamOnSamples forSync=%d channel=%d numFrames=%d\n", forSync, channelIndex, numFrames);
    client->audiostreamOnSamples(tempBuffer, 1, emptyOut, numChannels, numFrames, sampleRate);
  }
  //njClient->AudioProc(inbuf, innch, outbuf, outnch, len, srate);
  // Clean up
    for (int i = 0; i < numChannels; i++) {
        delete[] emptyOut[i];
    }
    delete[] emptyOut;
    delete[] tempBuffer[0];
    delete[] tempBuffer;
}

void NinjamClientAdapter::removeLocalChannel(int channelIndex) {
    client->gsNjClient()->DeleteLocalChannel(channelIndex);
}

void NinjamClientAdapter::setLocalChannelMonitoring(int index, float volume, float pan, bool mute, bool solo) {
    client->gsNjClient()->SetLocalChannelMonitoring(index, true, volume, true, pan, true, mute, true, solo);
}


void NinjamClientAdapter::SetLocalChannelInfo(int index, const char* name, bool setsrcch, int srcch, bool setxmit, bool xmit, bool setflags, int flags) {
    client->gsNjClient()->SetLocalChannelInfo(
                                              index,
                                              name, //--> name : Nombre del canal
                                              setsrcch, //--> setsrcch
                                              srcch,  // --> srcchannel : 0 para mono, 1024 para estéreo
                                              false, // --> setbitrate
                                              0,  // --> bitrate : Bitrate de codificación OGG (ej: 64 o 128 kbps)
                                              setxmit,  //--> setbcast
                                              xmit,  // --> broadcast
                                              false, // --> setoutch for monitoring
                                              0,   // --> outchannel
                                              setflags,
                                              flags // --> flags: Modos especiales (ej: 0 = normal, 2 = voz en vivo, 4 = sesión)
                                              );
    // Notify server of the channel info change so other clients see it
    client->gsNjClient()->NotifyServerOfChannelChange();
}

void NinjamClientAdapter::sendUserMask(unsigned int mask) {
    // Implementation depends on the internal structure of NJClient
    // This would typically be used to subscribe/unsubscribe from user channels
    if (connected && client->gsNjClient()->GetNumUsers() > 0) {
        // Apply user mask to all available users
        for (int i = 0; i < client->gsNjClient()->GetNumUsers(); i++) {
            char* username = client->gsNjClient()->GetUserState(i);
            if (username) {
                // Set subscription mask for this user
                client->gsNjClient()->SetUserState(i, false, 0, false, 0, false, false);
                
                // Apply channel subscriptions based on mask
                for (int ch = 0; ch < 32; ch++) {
                    if (mask & (1 << ch)) {
                        client->gsNjClient()->SetUserChannelState(i, ch, true, true, false, 0, false, 0, false, false, false, false);
                    }
                }
            }
        }
    }
}

void NinjamClientAdapter::setMetronome(float volume, bool mute, float pan) {
    metronomeVolume = volume;
    metronomeEnabled = !mute;
    metronomePan = pan;
    
    client->gsNjClient()->config_metronome_mute = !metronomeEnabled;
    client->gsNjClient()->config_metronome_pan = metronomePan;
    client->gsNjClient()->config_metronome = metronomeVolume;
}

void NinjamClientAdapter::setMasterVolume(float volume, float pan, bool mute) {
    masterVolume = volume;
    masterPan = pan;
    masterMute = mute;
    
    // Set master volume - implementation would depend on NJClient's interface
    // This is a common implementation pattern:
    client->gsNjClient()->config_mastermute = masterMute;
    client->gsNjClient()->config_mastervolume = masterVolume;
    client->gsNjClient()->config_masterpan = masterPan;
//    client->gsNjClient()->config_metronome = 1.0f;
    //client->gsNjClient()->config_play_prebuffer = 4096;
  
    
}


void NinjamClientAdapter::getOutputPeaks(float* left, float* right) {
    if (!connected || !client) {
        if (left) *left = 0.0f;
        if (right) *right = 0.0f;
        return;
    }
    
    // Obtener valores reales del cliente NJClient
    float l = 0.0f, r = 0.0f;
    
    // Valores de ejemplo - reemplazar con llamadas reales al cliente
    if (outputBuffer && numChannels > 0) {
        // Calcular niveles de pico de los últimos datos procesados
        for (int i = 0; i < numFrames && i < 8192; i++) {
            l = std::max(l, std::abs(outputBuffer[0][i]));
            if (numChannels > 1) {
                r = std::max(r, std::abs(outputBuffer[1][i]));
            }
        }
    }
    
    if (left) *left = l;
    if (right) *right = (numChannels > 1) ? r : l;
}

float NinjamClientAdapter::getUserChannelPeak(int useridx, int channelidx, int whichch) {
  if (!connected || !client) {
    return 0.0f; // Return 0 if not connected
  }
  return client->gsNjClient()->GetUserChannelPeak(useridx, channelidx, whichch);
}

void NinjamClientAdapter::sendChatMessage(const std::string& message) {
    if (connected) {
        client->sendChatMessage(message);
    }
}

void NinjamClientAdapter::playMetronomeTick(bool isDownbeat) {
    if (!connected || !client) return;
    
    // Inicializar el sonido del metrónomo si no está inicializado
    static MetronomeSound metronome;
    if (!metronome.initialized) {
        metronome.initialize();
    }
    
    // Obtener muestras del sonido apropiadas
    const float* samples = metronome.getSamples(isDownbeat);
    int sampleCount = metronome.getSampleCount();
    
    // Crear buffers temporales de entrada y salida
    float** tempInBuffer = new float*[1];
    float** tempOutBuffer = new float*[numChannels];
    
    tempInBuffer[0] = new float[sampleCount];
    
    // Aplicar volumen a las muestras
    float tickVolume = metronomeEnabled ?
                     (isDownbeat ? metronomeVolume : metronomeVolume * 0.8f) : 0.0f;
                     
    for (int i = 0; i < sampleCount; i++) {
        tempInBuffer[0][i] = samples[i] * tickVolume;
    }
    
    // Preparar buffer de salida
    for (int i = 0; i < numChannels; i++) {
        tempOutBuffer[i] = new float[sampleCount];
        memset(tempOutBuffer[i], 0, sizeof(float) * sampleCount);
    }
    
    // Usar AudioProc en vez de OnSamples
    //client->gsNjClient()->AudioProc(tempInBuffer, 1, tempOutBuffer, numChannels, sampleCount, sampleRate);
    
  
    // Liberar los buffers
    for (int i = 0; i < numChannels; i++) {
        delete[] tempOutBuffer[i];
    }
    delete[] tempOutBuffer;
    delete[] tempInBuffer[0];
    delete[] tempInBuffer;
}

int NinjamClientAdapter::getBPM() {
    if (connected) {
        return static_cast<int>(client->gsNjClient()->GetActualBPM());
    }
    return 120; // Default BPM
}

int NinjamClientAdapter::getBPI() {
    if (connected) {
        return client->gsNjClient()->GetBPI();
    }
    return 16; // Default BPI
}

double NinjamClientAdapter::getIntervalPosition() {
    if (connected) {
      int pos, samplesInInterval;
      client->gsNjClient()->GetPosition(&pos, &samplesInInterval);
      return static_cast<double>(pos) / static_cast<double>(samplesInInterval);
    }
    return 0.0;
}

double NinjamClientAdapter::getTimeInSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}


// Implementación de getRemoteUsers()
std::vector<AbNinjam::Common::RemoteUser> NinjamClientAdapter::getRemoteUsers() {
    if (!connected || !client) {
        return std::vector<AbNinjam::Common::RemoteUser>();
    }
    return client->getRemoteUsers();
}

void NinjamClientAdapter::setUserChannelVolume(int userId, int channelId, float volume) {
    if (connected && client) {
        client->setUserChannelVolume(userId, channelId, volume);
    }
}

void NinjamClientAdapter::setUserChannelState(int userId, int channelId,bool setsub, bool sub, bool setvol, float vol, bool setpan, float pan, bool setmute, bool mute, bool setsolo, bool solo, bool setoutch, int outchannel) {
    if (connected && client) {
        client->setUserChannelState(userId, channelId, setsub, sub, setvol, vol, setpan, pan, setmute,  mute, setsolo, solo, setoutch, outchannel);
    }
}

void NinjamClientAdapter::setLocalChannelVolume(int channelId, float volume) {
    if (connected && client) {
        client->setLocalChannelVolume(channelId, volume);
    }
}

void NinjamClientAdapter::setOnInterval(OnIntervalCallback callback) {
    onIntervalCallback = callback;
    lastIntervalPosition = 0.0;
}

std::string NinjamClientAdapter::getServerStatus() {
    if (connected) {
        std::string status = "Connected to ";
        status += client->gsNjClient()->GetHostName();
        status += " as ";
        status += client->gsNjClient()->GetUser();
        status += " - ";
        status += std::to_string(client->gsNjClient()->GetNumUsers());
        status += " users online - BPM: ";
        status += std::to_string(static_cast<int>(client->gsNjClient()->GetActualBPM()));
        status += " BPI: ";
        status += std::to_string(client->gsNjClient()->GetBPI());
        return status;
    }
    return "Not connected";
}

std::string NinjamClientAdapter::getErrorString() {
    if (client && client->gsNjClient()) {
        char* err = client->gsNjClient()->GetErrorStr();
        if (err && err[0]) {
            return std::string(err);
        }
    }
    return "";
}

const char* NinjamClientAdapter::getLocalUserName() {
    if (connected && client) {
        return client->gsNjClient()->GetUser();
    }
    return "";
}

// Implementation of syncWithServerClock
void NinjamClientAdapter::syncWithServerClock() {
    if (!connected || !client) return;
    
    // Get current time with high precision
    double now = 0.0;
    #ifdef _WIN32
        LARGE_INTEGER frequency, currentTime;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&currentTime);
        now = static_cast<double>(currentTime.QuadPart) / frequency.QuadPart;
    #else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = ts.tv_sec + ts.tv_nsec / 1000000000.0;
    #endif
    
    // Limit sync operations to once per second to avoid jitter
    if (now - lastSyncTime < 1.0) return;
    
    // Get current server interval position
    double serverPosition = getIntervalPosition();
    
    // Calculate beat and interval durations
    int bpm = getBPM();
    int bpi = getBPI();
    double beatDuration = 60.0 / bpm;
    double intervalDuration = beatDuration * bpi;
    
    // If this is our first sync, initialize the interval start time
    if (intervalStartTime <= 0.0) {
        intervalStartTime = now - (serverPosition * intervalDuration);
    }
    
    // Calculate where we should be based on our internal clock
    double expectedPosition = fmod((now - intervalStartTime), intervalDuration) / intervalDuration;
    
    // Calculate drift (normalize to -0.5 to 0.5 range)
    double drift = serverPosition - expectedPosition;
    if (drift > 0.5) drift -= 1.0;
    if (drift < -0.5) drift += 1.0;
    
    // Store drift in history (circular buffer)
    clockDriftHistory[clockDriftIndex] = drift;
    clockDriftIndex = (clockDriftIndex + 1) % clockDriftHistory.size();
    
    // Calculate average drift using our history buffer
    double avgDrift = 0.0;
    for (double d : clockDriftHistory) {
        avgDrift += d;
    }
    avgDrift /= clockDriftHistory.size();
    
    // Apply graduated correction based on drift magnitude
    double correctionFactor;
    if (fabs(avgDrift) > 0.1) {
        correctionFactor = 0.25; // Larger correction for significant drift
    } else if (fabs(avgDrift) > 0.05) {
        correctionFactor = 0.15; // Medium correction
    } else if (fabs(avgDrift) > 0.01) {
        correctionFactor = 0.1;  // Small correction
    } else {
        correctionFactor = 0.0;  // No correction for minimal drift
    }
    
    if (correctionFactor > 0.0) {
        double correction = avgDrift * correctionFactor;
        intervalStartTime += correction * intervalDuration;
        
        // Force immediate position update
        {
            std::lock_guard<std::mutex> lock(client->gsMtx());
            client->gsNjClient()->Run();
        }
    }
    
    lastSyncTime = now;
}

// Add this to your NinjamClientAdapter class to automatically subscribe to new channels
void NinjamClientAdapter::subscribeToAllRemoteChannels() {
    if (!connected) return;
    
    // Get all remote users
    std::vector<AbNinjam::Common::RemoteUser> users = client->getRemoteUsers();
    
    // For each user, subscribe to all their channels
    for (const auto& user : users) {
        for (const auto& channel : user.channels) {
            // Set channel state to subscribed with audible volume (0.8)
            client->gsNjClient()->SetUserChannelState(
                user.id,             // User index
                channel.id,          // Channel index
                true,                // Set subscribe
                true,                // Subscribe = true
                true,                // Set volume
                1.0f,                // Volume = 1.0 (100%, default)
                false, 0.0f,         // Don't change pan
                false, false,        // Don't change mute
                false, false         // Don't change solo
            );
        }
    }
    
    // Invalidate cache to ensure our changes are reflected
    invalidateUsersCache();
}

void NinjamClientAdapter::setOnRawData(OnRawDataCallback callback) {
    onRawDataCallback = callback;
    if (client) {
        client->setRawDataCallback(rawDataCCallback, this);
    }
}

void NinjamClientAdapter::rawDataSendBegin(unsigned char outGuid[16], unsigned int fourcc, int chidx, int estsize) {
    if (!connected || !client) return;
    client->rawDataSendBegin(outGuid, fourcc, chidx, estsize);
}

void NinjamClientAdapter::rawDataSendWrite(const unsigned char guid[16], const void *data, int dataLen, bool isEnd) {
    if (!connected || !client) return;
    client->rawDataSendWrite(guid, data, dataLen, isEnd);
}

void NinjamClientAdapter::setIntervalSwapCallback(std::function<void()> callback) {
    intervalSwapCb = callback;
    if (client && client->gsNjClient()) {
        client->gsNjClient()->IntervalSwap_Callback = [](void *userData) {
            NinjamClientAdapter *adapter = static_cast<NinjamClientAdapter*>(userData);
            if (adapter && adapter->intervalSwapCb) {
                adapter->intervalSwapCb();
            }
        };
        client->gsNjClient()->IntervalSwap_User = this;
    }
}

void NinjamClientAdapter::setVideoIntervalReadyCallback(NJClient::VideoIntervalReadyCallback callback) {
    if (client && client->gsNjClient()) {
        client->gsNjClient()->VideoIntervalReady_Callback = callback;
        client->gsNjClient()->VideoIntervalReady_User = this;
    }
}

void NinjamClientAdapter::setVideoChannel(int chidx, unsigned int fourcc) {
    if (!client) return;
    client->gsNjClient()->SetVideoChannel(chidx, fourcc);
}

void NinjamClientAdapter::stopVideoChannel() {
    if (!client) return;
    client->gsNjClient()->StopVideoChannel();
}

void NinjamClientAdapter::queueVideoFrame(const void *data, int len) {
    if (!client) return;
    client->gsNjClient()->QueueVideoFrame(data, len);
}

void NinjamClientAdapter::setVideoSPSPPS(const void *data, int len) {
    if (!client) return;
    client->gsNjClient()->SetVideoSPSPPS(data, len);
}
