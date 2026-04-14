#ifndef OBOE_ENGINE_H
#define OBOE_ENGINE_H

#include <oboe/Oboe.h>
#include "OboeCallback.h"
#include "NinjamClientBridge.h"
#include "SessionRecorder.h"
#include <memory>
#include <atomic>

/**
 * Manages Oboe audio streams for full-duplex NINJAM audio.
 *
 * Creates and manages an input stream (mic) and output stream (speakers).
 * The NinjamOboeCallback handles the actual audio processing on the audio thread.
 *
 * Lifecycle: create → setClient → start → [running] → stop → destroy
 */
class OboeEngine {
public:
    OboeEngine();
    ~OboeEngine();

    // Set the NINJAM client (must be called before start)
    void setClient(NinjamClientRef* client);

    // Get FX processor (created in constructor, lives for engine lifetime)
    AudioFXProcessor* getFXProcessor() { return m_fxProcessor.get(); }

    // Get session recorder (created in constructor, lives for engine lifetime)
    SessionRecorder* getRecorder() { return m_recorder.get(); }

    // Start audio streams (returns true on success)
    bool start(int32_t sampleRate, int32_t framesPerBuffer);

    // Stop and close audio streams
    void stop();

    // Check if engine is running
    bool isRunning() const;

    // Get the actual sample rate negotiated with the device
    int32_t getSampleRate() const;

    // Get the actual frames per buffer
    int32_t getFramesPerBuffer() const;

    // Peak levels (thread-safe, called from UI thread)
    void getOutputPeaks(float* left, float* right) const;
    void getInputPeaks(float* left, float* right) const;

    // Audio device configuration
    void setPerformanceMode(oboe::PerformanceMode mode);
    void setSharingMode(oboe::SharingMode mode);

    // Audio device routing — thread device selection into Oboe streams
    void setInputDeviceId(int32_t deviceId);
    void setOutputDeviceId(int32_t deviceId);

private:
    // Streams
    std::shared_ptr<oboe::AudioStream> m_outputStream;
    std::shared_ptr<oboe::AudioStream> m_inputStream;

    // Callback (shared between input and output)
    std::shared_ptr<NinjamOboeCallback> m_callback;

    // FX processor (owned by engine, used by callback on audio thread)
    std::unique_ptr<AudioFXProcessor> m_fxProcessor;

    // Session recorder (owned by engine, used by callback on audio thread)
    std::unique_ptr<SessionRecorder> m_recorder;

    // Client reference
    NinjamClientRef* m_client = nullptr;

    // State
    std::atomic<bool> m_running{false};
    int32_t m_sampleRate = 48000;
    int32_t m_framesPerBuffer = 256;
    oboe::PerformanceMode m_performanceMode = oboe::PerformanceMode::LowLatency;
    oboe::SharingMode m_sharingMode = oboe::SharingMode::Exclusive;

    // Device routing (kUnspecified = system default)
    int32_t m_inputDeviceId = oboe::kUnspecified;
    int32_t m_outputDeviceId = oboe::kUnspecified;

    // Helpers
    bool openOutputStream();
    bool openInputStream();
    void closeStreams();
};

#endif // OBOE_ENGINE_H
