#ifndef OBOE_CALLBACK_H
#define OBOE_CALLBACK_H

#include <oboe/Oboe.h>
#include "NinjamClientBridge.h"
#include "AudioFXProcessor.h"
#include "SessionRecorder.h"

/**
 * Oboe AudioStreamCallback for full-duplex audio processing.
 *
 * This callback runs on a high-priority audio thread and calls
 * NinjamClient_processAudioSIMD to mix local input with remote streams.
 *
 * Architecture:
 *   Input Stream (mic) --> inputRingBuffer --> [render callback] --> Output Stream (speakers)
 *                                                   |
 *                                          NinjamClient_processAudioSIMD
 *                                          (mixes remote + local audio)
 */
class NinjamOboeCallback : public oboe::AudioStreamDataCallback,
                           public oboe::AudioStreamErrorCallback {
public:
    NinjamOboeCallback();
    ~NinjamOboeCallback();

    // Set the NINJAM client to process audio through
    void setClient(NinjamClientRef* client);

    // Set the FX processor for local input effects
    void setFXProcessor(AudioFXProcessor* fxProcessor);

    // Set the session recorder for recording tap
    void setRecorder(SessionRecorder* recorder);

    // Called by Oboe on the audio thread when output needs data
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* outputStream,
        void* audioData,
        int32_t numFrames) override;

    // Called when the audio stream encounters an error (e.g., device disconnected)
    void onErrorBeforeClose(oboe::AudioStream* stream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

    // Input stream management for full-duplex
    void setInputStream(oboe::AudioStream* inputStream);

    // Peak level access (called from Kotlin on UI thread)
    void getOutputPeaks(float* left, float* right) const;
    void getInputPeaks(float* left, float* right) const;

    // Direct monitor — see m_directMonitor field doc.
    void setDirectMonitor(bool enabled) { m_directMonitor.store(enabled, std::memory_order_relaxed); }
    void setLocalGain(float gain) { m_localGain.store(gain, std::memory_order_relaxed); }
    void setMasterGain(float gain) { m_masterGain.store(gain, std::memory_order_relaxed); }

private:
    NinjamClientRef* m_client = nullptr;
    AudioFXProcessor* m_fxProcessor = nullptr;
    SessionRecorder* m_recorder = nullptr;
    oboe::AudioStream* m_inputStream = nullptr;

    // Intermediate buffers for deinterleaving stereo to L/R
    static constexpr int MAX_FRAMES = 2048;
    float m_inLeft[MAX_FRAMES] = {};
    float m_inRight[MAX_FRAMES] = {};
    float m_outLeft[MAX_FRAMES] = {};
    float m_outRight[MAX_FRAMES] = {};
    // Metronome rendered separately by processAudio3 so we can mix it into
    // speakers but skip it for the recording tap (metronome-free recording,
    // iOS parity per AudioSessionManager.swift:682 / develop commit 32d259c).
    float m_outMetro[MAX_FRAMES] = {};

    // Direct monitor: bypass NJClient buffering by mixing input -> output
    // inside this audio callback. NJClient's local channel is muted by the
    // session manager so we don't double-mix. iOS does the equivalent by
    // connecting `inputNode -> mixer` directly in AVAudioEngine
    // (AppleAudioEngine.swift:1073-1077).
    std::atomic<bool>  m_directMonitor{false};

    // m_localGain: applied to m_inLeft IN PLACE before processAudio3 →
    // scales the mic both in the direct-monitor mix AND in what NJClient
    // sends to the server. Matches iOS built-in mic behavior where
    // setInputGain(localVolume) scales at the hardware preamp (affects
    // both directions equally). For USB on Android we apply the same
    // software scaling.
    std::atomic<float> m_localGain{1.0f};

    // m_masterGain: applied to the FINAL m_outLeft / m_outRight, AFTER all
    // mixes (NJClient music + direct monitor mic + metronome). Mirrors iOS
    // AVAudioEngine mixer.outputVolume which catches every node feeding the
    // mixer. NJClient's config_mastervolume already scales its own outbuf
    // independently, so the effective scaling on remote audio is master²
    // (same on iOS — accepted asymmetry).
    std::atomic<float> m_masterGain{1.0f};

    // Peak tracking (atomic for cross-thread access)
    std::atomic<float> m_outputPeakL{0.0f};
    std::atomic<float> m_outputPeakR{0.0f};
    std::atomic<float> m_inputPeakL{0.0f};
    std::atomic<float> m_inputPeakR{0.0f};

    // Helpers
    void deinterleave(const float* interleaved, float* left, float* right, int32_t numFrames);
    void interleave(const float* left, const float* right, float* interleaved, int32_t numFrames);
    float calculatePeak(const float* buffer, int32_t numFrames);
};

#endif // OBOE_CALLBACK_H
