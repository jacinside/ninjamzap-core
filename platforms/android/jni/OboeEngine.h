#ifndef OBOE_ENGINE_H
#define OBOE_ENGINE_H

#include <oboe/Oboe.h>
#include "OboeCallback.h"
#include "NinjamClientBridge.h"
#include "SessionRecorder.h"
#include <memory>
#include <string>
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

    // AEC toggle from the connection screen. Changing it while running
    // reopens the streams (the input needs / drops its session ID).
    void setAecRequested(bool enabled);

    // Output route is Bluetooth. Store-only: callers follow up with
    // setOutputDeviceId(), which performs the reopen that applies it.
    void setOutputBluetooth(bool bluetooth) { m_outputBluetooth.store(bluetooth); }

    // Stream health counters for the metrics panel / periodic stats log.
    // Read from a non-RT thread; the stream getters are cheap local reads.
    int32_t getOutputXRuns() const;
    int32_t getInputXRuns() const;
    int32_t getInputShortReads() const;
    int32_t getCallbackMaxMicros() const;
    int32_t getTunerGrowCount() const;

    // What the HAL actually GRANTED for the open streams, as JSON. These are
    // the facts that decide whether our latency work pays off on a given
    // device (AAudio vs OpenSL ES, MMAP, Exclusive vs Shared, LowLatency vs
    // None, whether the input carries a session id). They used to exist only
    // as a log line, which no test could assert on. Empty object when the
    // engine is not running.
    std::string getStreamCapabilitiesJson() const;
    int getLatencyProfile() const { return m_latencyProfile.load(); }

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

    /**
     * How many times the HAL has reported the output stream disconnected,
     * which is what losing the device it was opened on looks like. Exposed
     * because a lost stream is otherwise indistinguishable from silence: with
     * a counter, the condition can be raised and asserted on.
     */
    int32_t getOutputDisconnects() const {
        return m_callback ? m_callback->getOutputDisconnects() : 0;
    }

    /**
     * How many audio callbacks have run, and how long since the last one.
     *
     * The decisive pair. A stream can stop being driven while still reporting
     * itself open and running, and with nothing raised on the error path — so
     * "is audio alive?" cannot be answered by asking the stream. It is
     * answered by whether callbacks are still arriving. A gap far larger than
     * one buffer period, while the engine believes it is running, means the
     * audio is gone.
     */
    int64_t getCallbackCount() const { return m_callback ? m_callback->getCallbackCount() : 0; }
    int64_t getMillisSinceLastCallback() const;

#ifndef NDEBUG
    /**
     * Test seam, debug builds only: the order the streams were last closed in.
     *
     * One nibble per close, shifted in as it happens: 1 = output, 2 = input.
     * So 0x12 is output-then-input, which is the only correct order, and 0x21
     * is the inverted one. 0 means nothing has been closed yet.
     *
     * Recorded rather than inferred from logs: the order is a real invariant
     * (the callback runs on the output's thread and reads the input, so only
     * closing the output joins it), and an invariant deserves something
     * sturdier to check than the wording of a log line.
     */
    int32_t getCloseSequenceForTest() const {
        return m_closeSequence.load(std::memory_order_relaxed);
    }

    /**
     * Test seam, debug builds only: raises the HAL's disconnect path without a
     * real device loss.
     *
     * It exists because the condition is hard to produce on demand. An
     * emulator will not give it: restarting the audio server there drops the
     * stream onto a legacy path that restores itself, with callbacks never
     * missing a beat. Only real hardware losing a real device raises it, and
     * waiting for that is not a test.
     */
    void simulateDisconnectForTest() {
        if (m_callback) m_callback->simulateDisconnectForTest(oboe::Result::ErrorDisconnected);
    }
#endif


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

#ifndef NDEBUG
    // See getCloseSequenceForTest. Debug only; nothing reads it in release.
    std::atomic<int32_t> m_closeSequence{0};
#endif

    // Input preset — written from JS via setInputPreset(), read inside
    // openInputStream(). Defaults to VoicePerformance (low-latency mic path);
    // switched to Unprocessed when camera starts to disable HAL AGC/AEC/NS.
    // Unprocessed = flat input, the lowest-DSP capture path. This is the
    // Android equivalent of the iOS session running in .measurement mode, and
    // it is chosen for the same reason: VoicePerformance asks the HAL for voice
    // processing, which colours the signal and, on iOS, was what made the audio
    // swell and feed back once the camera opened (develop 28012e2). We record
    // music, not voice calls.
    std::atomic<oboe::InputPreset> m_inputPreset{oboe::InputPreset::Unprocessed};

    // Latency profile (connection-screen preset): 0=ultra_low, 1=low, 2=safe.
    // Single source of truth for buffer sizing — see profileParams(). Every
    // profile uses PerformanceMode::LowLatency: `None` is a different (slow,
    // ~200 ms round-trip) HAL path, not "a bit more buffer"
    // (docs/ANDROID_AUDIO_ENGINE_AUDIT.md §2.1).
    std::atomic<int> m_latencyProfile{1};
    struct ProfileParams {
        int32_t outMinBursts;   // LatencyTuner start / minimum (output)
        int32_t outMaxBursts;   // LatencyTuner ceiling (output); 0 = stream capacity
        int32_t inBursts;       // fixed input buffer (bursts)
    };
    static ProfileParams profileParams(int profile);
    static int profileFromFramesPerBuffer(int32_t framesPerBuffer);

    // AEC requested from the connection screen. Only then do we ask AAudio
    // for a session ID on the input: a session ID disables MMAP capture on
    // every device (effects can't attach to MMAP streams), so requesting it
    // unconditionally silently pushed the mic to the legacy path.
    std::atomic<bool> m_aecRequested{false};

    // Output currently routed to Bluetooth (set by Kotlin from AudioManager).
    // BT sinks need ~2× the queue (iOS parity: larger buffer on BT routes).
    std::atomic<bool> m_outputBluetooth{false};

    // Dynamic output buffer (Oboe auto-tuning pattern, docs/ANDROID_AUDIO_LATENCY.md §6):
    // the output queue starts at profile.outMinBursts × burst and grows by
    // one burst each time the stream reports a new xrun, capped at
    // profile.outMaxBursts × burst. tune() runs on the audio thread from
    // NinjamOboeCallback; the pointer is cleared before the stream is stopped.
    std::unique_ptr<oboe::LatencyTuner> m_outputTuner;

    // Helpers
    bool openOutputStream();
    bool openInputStream();
    void closeStreams();
    // Closes and reopens both streams using the current device IDs.
    // Caller must hold m_streamMutex. Returns true on success.
    bool reopenStreamsLocked();
};

#endif // OBOE_ENGINE_H
