#ifndef OBOE_ENGINE_H
#define OBOE_ENGINE_H

#include <oboe/Oboe.h>
#include "OboeCallback.h"
#include "NinjamClientBridge.h"
#include "SessionRecorder.h"
#include <memory>
#include <atomic>
#include <mutex>

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

    // Latency metric helpers — return 0 when the stream isn't open.
    int32_t getOutputBurst() const;
    int32_t getOutputBufferSize() const;
    int32_t getInputBurst() const;
    int32_t getInputBufferSize() const;
    // Real measured latency (ms) via Oboe timestamps; -1 if unsupported.
    double getOutputLatencyMillis() const;
    double getInputLatencyMillis() const;

    // Peak levels (thread-safe, called from UI thread)
    void getOutputPeaks(float* left, float* right) const;
    void getInputPeaks(float* left, float* right) const;

    // Audio device configuration
    void setPerformanceMode(oboe::PerformanceMode mode);
    void setSharingMode(oboe::SharingMode mode);

    // Pin streams to a specific AudioDeviceInfo.id. oboe::kUnspecified (0)
    // means "let Oboe pick" (current behavior). When the engine is already
    // running, the streams are closed and reopened on the new device so the
    // change takes effect immediately (Oboe has no AVAudioSession-style
    // setPreferredInput; close-and-reopen is the only path).
    void setInputDeviceId(int32_t deviceId);
    void setOutputDeviceId(int32_t deviceId);

    // Allocated input session ID — used by Kotlin to attach
    // AcousticEchoCanceler / NoiseSuppressor / AutomaticGainControl AudioFX.
    // Returns 0 (kSessionIdNone) if the input stream isn't open.
    int32_t getInputSessionId() const;

    // Forwarders to the callback's atomic flags / gains.
    void setDirectMonitor(bool enabled);
    void setLocalGain(float gain);
    void setMasterGain(float gain);

    // Change input preset (Unprocessed vs VoicePerformance). When the engine
    // is running, the input stream is reopened so the new preset takes effect.
    void setInputPreset(oboe::InputPreset preset);

    // Latency profile: 0=ultra_low (2× burst, LowLatency), 1=low (2× burst,
    // LowLatency), 2=safe (3× burst, None). Maps the iOS-style "latency
    // preset" radio in the connection screen to actual Oboe knobs. Without
    // this, all three presets behave identically on Android because the
    // legacy flags (PreferredIOBufferDuration, ring buffer settings) are
    // no-ops on the Oboe path. Triggers a stream reopen when running.
    void setLatencyProfile(int profile);

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

    // Selected audio device IDs (AudioDeviceInfo.id). kUnspecified = auto.
    std::atomic<int32_t> m_inputDeviceId{oboe::kUnspecified};
    std::atomic<int32_t> m_outputDeviceId{oboe::kUnspecified};

    // Serializes stream lifecycle (start / stop / live reopen on device change).
    std::mutex m_streamMutex;

    // Input preset — written from JS via setInputPreset(), read inside
    // openInputStream(). Defaults to VoicePerformance (low-latency mic path);
    // switched to Unprocessed when camera starts to disable HAL AGC/AEC/NS.
    std::atomic<oboe::InputPreset> m_inputPreset{oboe::InputPreset::VoicePerformance};

    // Buffer-size multiplier applied to stream burst after open. Lower =
    // less queue (lower latency, more underrun risk). Default 3× matches
    // Oboe's "safe" default; ultra_low/low presets bring it down to 2×.
    std::atomic<int32_t> m_bufferMultiplier{3};

    // Helpers
    bool openOutputStream();
    bool openInputStream();
    void closeStreams();
    // Closes and reopens both streams using the current device IDs.
    // Caller must hold m_streamMutex. Returns true on success.
    bool reopenStreamsLocked();
};

#endif // OBOE_ENGINE_H
