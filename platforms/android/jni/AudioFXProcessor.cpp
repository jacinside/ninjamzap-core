#include "AudioFXProcessor.h"
#include <android/log.h>

#define LOG_TAG "AudioFX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Static member definitions
constexpr int AudioFXProcessor::COMB_LENGTHS[];
constexpr int AudioFXProcessor::ALLPASS_LENGTHS[];
constexpr AudioFXProcessor::ReverbParams AudioFXProcessor::REVERB_PRESETS[];

AudioFXProcessor::AudioFXProcessor() {
    resetReverbBuffers();
    LOGI("AudioFXProcessor initialized");
}

// ============================================================================
// Main processing (called from Oboe audio thread)
// ============================================================================

void AudioFXProcessor::process(float* inL, float* inR, int numFrames) {
    // Snapshot FX state under lock (one lock, all reads, unlock before processing)
    fxMutex.lock();
    bool doBoost = _micBoostEnabled;
    float boostGain = _micBoostGain;
    bool doComp = _compressorEnabled;
    bool doReverb = _reverbEnabled;
    float threshold = _compThreshold;
    float ratio = _compRatio;
    float attack = _compAttack;
    float release = _compRelease;
    float makeupGain = _compMakeupGain;
    float mix = _reverbMix;
    int preset = _reverbPreset;
    bool needsReset = _needsReverbReset;
    if (needsReset) _needsReverbReset = false;
    fxMutex.unlock();

    // Reset reverb buffers on audio thread (safe — only this thread touches them)
    if (needsReset) {
        resetReverbBuffers();
    }

    if (!doBoost && !doComp && !doReverb) return;

    // 1. Mic boost (first in chain)
    if (doBoost) {
        for (int i = 0; i < numFrames; i++) {
            inL[i] *= boostGain;
            inR[i] *= boostGain;
        }
    }

    // 2. Compressor
    if (doComp) {
        processCompressor(inL, inR, numFrames, threshold, ratio, attack, release, makeupGain);
    }

    // 3. Reverb
    if (doReverb) {
        processReverb(inL, inR, numFrames, mix, preset);
    }
}

// ============================================================================
// Compressor — envelope follower + gain reduction
// ============================================================================

void AudioFXProcessor::processCompressor(float* bufL, float* bufR, int numFrames,
                                          float threshold, float ratio,
                                          float attack, float release, float makeupGain) {
    const float sampleRate = 48000.0f;
    const float attackCoeff = expf(-1.0f / (attack * sampleRate));
    const float releaseCoeff = expf(-1.0f / (release * sampleRate));
    const float makeupLinear = powf(10.0f, makeupGain / 20.0f);

    for (int i = 0; i < numFrames; i++) {
        float peak = fmaxf(fabsf(bufL[i]), fabsf(bufR[i]));

        // Envelope follower (stereo-linked)
        if (peak > envelopeLevel) {
            envelopeLevel = attackCoeff * envelopeLevel + (1.0f - attackCoeff) * peak;
        } else {
            envelopeLevel = releaseCoeff * envelopeLevel + (1.0f - releaseCoeff) * peak;
        }

        // Gain computation in dB
        float envDB = envelopeLevel > 1e-10f ? 20.0f * log10f(envelopeLevel) : -100.0f;
        float gainDB = 0.0f;
        if (envDB > threshold) {
            float excess = envDB - threshold;
            gainDB = -(excess - excess / ratio);
        }

        float gainLinear = powf(10.0f, gainDB / 20.0f) * makeupLinear;
        bufL[i] *= gainLinear;
        bufR[i] *= gainLinear;
    }
}

// ============================================================================
// Reverb — Schroeder (4 comb + 2 allpass filters)
// ============================================================================

void AudioFXProcessor::processReverb(float* bufL, float* bufR, int numFrames,
                                      float mix, int preset) {
    const ReverbParams& p = REVERB_PRESETS[preset];
    const float dry = 1.0f - mix;
    const float wet = mix;

    for (int i = 0; i < numFrames; i++) {
        float inputL = bufL[i];
        float inputR = bufR[i];

        float combSumL = 0.0f;
        float combSumR = 0.0f;

        // Process 4 comb filters
        for (int c = 0; c < NUM_COMBS; c++) {
            int idxL = combIndicesL[c];
            float delayedL = combBuffersL[c][idxL];
            dampStateL[c] = dampStateL[c] * p.damping + delayedL * (1.0f - p.damping);
            combBuffersL[c][idxL] = inputL + dampStateL[c] * p.feedback;
            combIndicesL[c] = (idxL + 1) % COMB_LENGTHS[c];
            combSumL += delayedL;

            int idxR = combIndicesR[c];
            float delayedR = combBuffersR[c][idxR];
            dampStateR[c] = dampStateR[c] * p.damping + delayedR * (1.0f - p.damping);
            combBuffersR[c][idxR] = inputR + dampStateR[c] * p.feedback;
            combIndicesR[c] = (idxR + 1) % COMB_LENGTHS[c];
            combSumR += delayedR;
        }

        float allpassOutL = combSumL / (float)NUM_COMBS;
        float allpassOutR = combSumR / (float)NUM_COMBS;

        // Process 2 allpass filters
        for (int a = 0; a < NUM_ALLPASS; a++) {
            int aidxL = allpassIndicesL[a];
            float bufValL = allpassBuffersL[a][aidxL];
            allpassBuffersL[a][aidxL] = allpassOutL + bufValL * p.allpassGain;
            allpassOutL = bufValL - allpassOutL * p.allpassGain;
            allpassIndicesL[a] = (aidxL + 1) % ALLPASS_LENGTHS[a];

            int aidxR = allpassIndicesR[a];
            float bufValR = allpassBuffersR[a][aidxR];
            allpassBuffersR[a][aidxR] = allpassOutR + bufValR * p.allpassGain;
            allpassOutR = bufValR - allpassOutR * p.allpassGain;
            allpassIndicesR[a] = (aidxR + 1) % ALLPASS_LENGTHS[a];
        }

        bufL[i] = inputL * dry + allpassOutL * wet;
        bufR[i] = inputR * dry + allpassOutR * wet;
    }
}

// ============================================================================
// Reverb buffer reset
// ============================================================================

void AudioFXProcessor::resetReverbBuffers() {
    for (int c = 0; c < NUM_COMBS; c++) {
        memset(combBuffersL[c], 0, COMB_LENGTHS[c] * sizeof(float));
        memset(combBuffersR[c], 0, COMB_LENGTHS[c] * sizeof(float));
        combIndicesL[c] = 0;
        combIndicesR[c] = 0;
        dampStateL[c] = 0.0f;
        dampStateR[c] = 0.0f;
    }
    for (int a = 0; a < NUM_ALLPASS; a++) {
        memset(allpassBuffersL[a], 0, ALLPASS_LENGTHS[a] * sizeof(float));
        memset(allpassBuffersR[a], 0, ALLPASS_LENGTHS[a] * sizeof(float));
        allpassIndicesL[a] = 0;
        allpassIndicesR[a] = 0;
    }
    envelopeLevel = 0.0f;
}

// ============================================================================
// State setters (called from JNI/main thread)
// ============================================================================

void AudioFXProcessor::setEnabled(const char* fxName, bool enabled) {
    std::lock_guard<std::mutex> lock(fxMutex);
    if (strcmp(fxName, "micBoost") == 0) {
        _micBoostEnabled = enabled;
    } else if (strcmp(fxName, "compressor") == 0) {
        _compressorEnabled = enabled;
        if (!enabled) envelopeLevel = 0.0f;
    } else if (strcmp(fxName, "reverb") == 0) {
        _reverbEnabled = enabled;
        if (!enabled) _needsReverbReset = true;
    }
}

void AudioFXProcessor::setParameter(const char* fxName, const char* param, float value) {
    std::lock_guard<std::mutex> lock(fxMutex);
    if (strcmp(fxName, "micBoost") == 0) {
        if (strcmp(param, "gain") == 0) _micBoostGain = clampf(value, 1.0f, 4.0f);
    } else if (strcmp(fxName, "compressor") == 0) {
        if (strcmp(param, "threshold") == 0) _compThreshold = clampf(value, -60.0f, 0.0f);
        else if (strcmp(param, "ratio") == 0) _compRatio = clampf(value, 1.0f, 20.0f);
        else if (strcmp(param, "attack") == 0) _compAttack = clampf(value, 0.001f, 0.5f);
        else if (strcmp(param, "release") == 0) _compRelease = clampf(value, 0.01f, 1.0f);
        else if (strcmp(param, "makeupGain") == 0) _compMakeupGain = clampf(value, 0.0f, 24.0f);
    } else if (strcmp(fxName, "reverb") == 0) {
        if (strcmp(param, "mix") == 0) _reverbMix = clampf(value, 0.0f, 1.0f);
        else if (strcmp(param, "preset") == 0) {
            _reverbPreset = (int)clampf(value, 0.0f, 3.0f);
            _needsReverbReset = true;
        }
    }
}

void AudioFXProcessor::getState(float* outState) {
    std::lock_guard<std::mutex> lock(fxMutex);
    outState[0] = _micBoostEnabled ? 1.0f : 0.0f;
    outState[1] = _micBoostGain;
    outState[2] = _reverbEnabled ? 1.0f : 0.0f;
    outState[3] = _reverbMix;
    outState[4] = (float)_reverbPreset;
    outState[5] = _compressorEnabled ? 1.0f : 0.0f;
    outState[6] = _compThreshold;
    outState[7] = _compRatio;
    outState[8] = _compAttack;
    outState[9] = _compRelease;
    outState[10] = _compMakeupGain;
}
