#include "OboeCallback.h"
#include "abNinjam/ninjamclientAdapter.h"
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <algorithm>

#define LOG_TAG "NinjamOboe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

NinjamOboeCallback::NinjamOboeCallback() = default;
NinjamOboeCallback::~NinjamOboeCallback() = default;

void NinjamOboeCallback::setClient(NinjamClientRef* client) {
    m_client = client;
}

void NinjamOboeCallback::setInputStream(oboe::AudioStream* inputStream) {
    m_inputStream = inputStream;
}

void NinjamOboeCallback::setFXProcessor(AudioFXProcessor* fxProcessor) {
    m_fxProcessor = fxProcessor;
}

void NinjamOboeCallback::setRecorder(SessionRecorder* recorder) {
    m_recorder = recorder;
}

oboe::DataCallbackResult NinjamOboeCallback::onAudioReady(
    oboe::AudioStream* outputStream,
    void* audioData,
    int32_t numFrames) {

    auto* outputBuffer = static_cast<float*>(audioData);
    int32_t framesToProcess = std::min(numFrames, MAX_FRAMES);

    // Read input from microphone stream
    memset(m_inLeft, 0, framesToProcess * sizeof(float));
    memset(m_inRight, 0, framesToProcess * sizeof(float));

    // Read raw mic samples — peak measured AFTER localGain below so the VU
    // reflects the actual send level (iOS parity per AppleAudioEngine.swift
    // line ~955 "Measure input level after send gain").
    int32_t readCount = 0;
    if (m_inputStream) {
        int32_t channelCount = m_inputStream->getChannelCount();
        if (channelCount == 1) {
            oboe::ResultWithValue<int32_t> result =
                m_inputStream->read(m_inLeft, framesToProcess, 0);
            if (result.value() > 0) {
                readCount = result.value();
                memcpy(m_inRight, m_inLeft, readCount * sizeof(float));
            }
        } else if (channelCount == 2) {
            float interleavedInput[MAX_FRAMES * 2];
            oboe::ResultWithValue<int32_t> result =
                m_inputStream->read(interleavedInput, framesToProcess, 0);
            if (result.value() > 0) {
                readCount = result.value();
                deinterleave(interleavedInput, m_inLeft, m_inRight, readCount);
            }
        }
    }

    // Apply FX to local input before NINJAM processing (mic boost → compressor → reverb)
    if (m_fxProcessor) {
        m_fxProcessor->process(m_inLeft, m_inRight, framesToProcess);
    }

    // Apply local channel gain to the mic IN PLACE — scales both what NJClient
    // sends to the server AND what the user hears in the monitor. Mirrors iOS
    // setInputGain on the built-in mic.
    const float localGain = m_localGain.load(std::memory_order_relaxed);
    if (localGain != 1.0f) {
        for (int32_t i = 0; i < framesToProcess; i++) {
            m_inLeft[i]  *= localGain;
            m_inRight[i] *= localGain;
        }
    }

    // Input VU AFTER localGain — shows the actual send/monitor level so the
    // slider visibly drives the local channel meter.
    if (readCount > 0) {
        m_inputPeakL.store(calculatePeak(m_inLeft, readCount), std::memory_order_relaxed);
        m_inputPeakR.store(calculatePeak(m_inRight, readCount), std::memory_order_relaxed);
    }

    // Process through NINJAM. Use processAudio3 with a separate metronome
    // bus so the recording tap can omit it (metronome-free recording — iOS
    // parity per AudioSessionManager.swift:682 + develop commit 32d259c).
    // Requires Kotlin to have called setMetronomeChannel(2 | 1024) at session
    // start; otherwise outMetro stays silent and behavior matches the old
    // processAudioSIMD path (metronome mixed into outLeft/outRight).
    memset(m_outLeft, 0, framesToProcess * sizeof(float));
    memset(m_outRight, 0, framesToProcess * sizeof(float));
    memset(m_outMetro, 0, framesToProcess * sizeof(float));

    if (m_client && m_client->adapter) {
        auto* adapter = static_cast<NinjamClientAdapter*>(m_client->adapter);
        adapter->processAudio3(
            m_inLeft, m_inRight,
            m_outLeft, m_outRight,
            m_outMetro,
            framesToProcess
        );
    }

    // Recording tap: capture music + local input (NO metronome). Mirrors
    // iOS AppleAudioEngine which sends bus0/bus1 to the recorder but not
    // bus2 (the metronome). Local mic is included so the musician hears
    // themselves on playback even if direct monitoring routed mic elsewhere.
    if (m_recorder && m_recorder->isRecording()) {
        float recL[MAX_FRAMES];
        float recR[MAX_FRAMES];
        for (int32_t i = 0; i < framesToProcess; i++) {
            recL[i] = m_outLeft[i] + m_inLeft[i];
            recR[i] = m_outRight[i] + m_inRight[i];
        }
        m_recorder->writeSamples(recL, recR, framesToProcess);
    }

    // Mix metronome into speakers AFTER the recording tap so the user still
    // hears the click in real time but the recording file is metronome-free.
    for (int32_t i = 0; i < framesToProcess; i++) {
        m_outLeft[i]  += m_outMetro[i];
        m_outRight[i] += m_outMetro[i];
    }

    // Master gain — scales the FINAL mix (NJClient music + local monitor +
    // metronome). iOS AVAudioEngine mixer.outputVolume equivalent.
    const float masterGain = m_masterGain.load(std::memory_order_relaxed);
    if (masterGain != 1.0f) {
        for (int32_t i = 0; i < framesToProcess; i++) {
            m_outLeft[i]  *= masterGain;
            m_outRight[i] *= masterGain;
        }
    }

    // Output peaks AFTER metronome + master gain — VU reflects what user
    // actually hears. Peak-hold with 0.85 release matches iOS
    // AppleAudioEngine.swift:1040 so 12 ms callback transients survive the
    // 50 ms VU poll interval (without hold, the metronome click is written
    // and overwritten before Kotlin reads it).
    constexpr float RELEASE = 0.85f;
    float newL = calculatePeak(m_outLeft, framesToProcess);
    float newR = calculatePeak(m_outRight, framesToProcess);
    float prevL = m_outputPeakL.load(std::memory_order_relaxed);
    float prevR = m_outputPeakR.load(std::memory_order_relaxed);
    m_outputPeakL.store(newL > prevL ? newL : prevL * RELEASE, std::memory_order_relaxed);
    m_outputPeakR.store(newR > prevR ? newR : prevR * RELEASE, std::memory_order_relaxed);

    // Interleave output for Oboe (stereo output stream)
    int32_t outputChannels = outputStream->getChannelCount();
    if (outputChannels == 2) {
        interleave(m_outLeft, m_outRight, outputBuffer, framesToProcess);
    } else if (outputChannels == 1) {
        // Mono output: mix L+R
        for (int32_t i = 0; i < framesToProcess; i++) {
            outputBuffer[i] = (m_outLeft[i] + m_outRight[i]) * 0.5f;
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void NinjamOboeCallback::onErrorBeforeClose(oboe::AudioStream* stream, oboe::Result error) {
    LOGE("Oboe stream error before close: %s", oboe::convertToText(error));
}

void NinjamOboeCallback::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) {
    LOGE("Oboe stream error after close: %s", oboe::convertToText(error));
    // TODO: Notify Kotlin layer to restart the audio engine
    // This is the equivalent of iOS AVAudioEngine configurationChangeNotification
}

void NinjamOboeCallback::getOutputPeaks(float* left, float* right) const {
    *left = m_outputPeakL.load(std::memory_order_relaxed);
    *right = m_outputPeakR.load(std::memory_order_relaxed);
}

void NinjamOboeCallback::getInputPeaks(float* left, float* right) const {
    *left = m_inputPeakL.load(std::memory_order_relaxed);
    *right = m_inputPeakR.load(std::memory_order_relaxed);
}

// ============================================================================
// Helpers
// ============================================================================

void NinjamOboeCallback::deinterleave(const float* interleaved, float* left, float* right, int32_t numFrames) {
    for (int32_t i = 0; i < numFrames; i++) {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }
}

void NinjamOboeCallback::interleave(const float* left, const float* right, float* interleaved, int32_t numFrames) {
    for (int32_t i = 0; i < numFrames; i++) {
        interleaved[i * 2] = left[i];
        interleaved[i * 2 + 1] = right[i];
    }
}

float NinjamOboeCallback::calculatePeak(const float* buffer, int32_t numFrames) {
    float peak = 0.0f;
    for (int32_t i = 0; i < numFrames; i++) {
        float abs = std::fabs(buffer[i]);
        if (abs > peak) peak = abs;
    }
    return peak;
}
