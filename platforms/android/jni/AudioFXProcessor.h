#ifndef AUDIO_FX_PROCESSOR_H
#define AUDIO_FX_PROCESSOR_H

#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>

/**
 * Real-time audio FX processor for local input channel.
 * Port of ios/NativeAudioModule/AudioFXProcessor.swift.
 *
 * Effects chain (in order):
 *   1. Mic Boost — linear gain multiply
 *   2. Compressor — envelope follower + gain reduction
 *   3. Reverb — Schroeder (4 comb + 2 allpass filters)
 *
 * Thread safety: snapshot pattern — lock once, copy params to locals,
 * unlock, process without locks. Safe for real-time audio thread.
 */
class AudioFXProcessor {
public:
    AudioFXProcessor();
    ~AudioFXProcessor() = default;

    // Process audio buffers in-place (called from Oboe audio thread)
    void process(float* inL, float* inR, int numFrames);

    // Enable/disable effects (called from JNI/main thread)
    void setEnabled(const char* fxName, bool enabled);

    // Set effect parameters (called from JNI/main thread)
    void setParameter(const char* fxName, const char* param, float value);

    // Get state as flat array for JNI:
    // [micBoostEnabled, micBoostGain, reverbEnabled, reverbMix, reverbPreset,
    //  compressorEnabled, compThreshold, compRatio, compAttack, compRelease, compMakeupGain]
    void getState(float* outState);

private:
    std::mutex fxMutex;

    // Mic Boost
    bool _micBoostEnabled = false;
    float _micBoostGain = 1.4125f;  // +3 dB default

    // Compressor
    bool _compressorEnabled = false;
    float _compThreshold = -20.0f;   // dBFS
    float _compRatio = 4.0f;         // x:1
    float _compAttack = 0.01f;       // seconds
    float _compRelease = 0.1f;       // seconds
    float _compMakeupGain = 0.0f;    // dB
    float envelopeLevel = 0.0f;      // envelope follower state

    // Reverb (Schroeder)
    bool _reverbEnabled = false;
    float _reverbMix = 0.3f;        // 0..1 wet/dry
    int _reverbPreset = 0;          // 0-3
    bool _needsReverbReset = false;

    // Comb filter delays (samples at 48kHz) — ~32ms avg
    static constexpr int NUM_COMBS = 4;
    static constexpr int COMB_LENGTHS[NUM_COMBS] = {1557, 1617, 1491, 1422};

    // Allpass filter delays
    static constexpr int NUM_ALLPASS = 2;
    static constexpr int ALLPASS_LENGTHS[NUM_ALLPASS] = {225, 556};

    // Reverb preset parameters
    struct ReverbParams {
        float feedback;
        float damping;
        float allpassGain;
    };
    static constexpr ReverbParams REVERB_PRESETS[4] = {
        {0.70f, 0.4f, 0.5f},   // small room
        {0.80f, 0.3f, 0.5f},   // medium room
        {0.88f, 0.2f, 0.5f},   // large hall
        {0.85f, 0.15f, 0.6f},  // plate
    };

    // Comb filter buffers and state (L + R)
    float combBuffersL[NUM_COMBS][1617];  // max of COMB_LENGTHS
    float combBuffersR[NUM_COMBS][1617];
    int combIndicesL[NUM_COMBS] = {};
    int combIndicesR[NUM_COMBS] = {};
    float dampStateL[NUM_COMBS] = {};
    float dampStateR[NUM_COMBS] = {};

    // Allpass filter buffers and state (L + R)
    float allpassBuffersL[NUM_ALLPASS][556];  // max of ALLPASS_LENGTHS
    float allpassBuffersR[NUM_ALLPASS][556];
    int allpassIndicesL[NUM_ALLPASS] = {};
    int allpassIndicesR[NUM_ALLPASS] = {};

    // Internal processing
    void processCompressor(float* bufL, float* bufR, int numFrames,
                           float threshold, float ratio,
                           float attack, float release, float makeupGain);
    void processReverb(float* bufL, float* bufR, int numFrames,
                       float mix, int preset);
    void resetReverbBuffers();

    // Helpers
    static inline float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

#endif // AUDIO_FX_PROCESSOR_H
