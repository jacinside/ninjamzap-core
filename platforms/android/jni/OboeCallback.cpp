#include "OboeCallback.h"
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

oboe::DataCallbackResult NinjamOboeCallback::onAudioReady(
    oboe::AudioStream* outputStream,
    void* audioData,
    int32_t numFrames) {

    auto* outputBuffer = static_cast<float*>(audioData);
    int32_t framesToProcess = std::min(numFrames, MAX_FRAMES);

    // Read input from microphone stream
    memset(m_inLeft, 0, framesToProcess * sizeof(float));
    memset(m_inRight, 0, framesToProcess * sizeof(float));

    if (m_inputStream) {
        int32_t channelCount = m_inputStream->getChannelCount();

        if (channelCount == 1) {
            // Mono input: read directly into left, copy to right
            oboe::ResultWithValue<int32_t> result =
                m_inputStream->read(m_inLeft, framesToProcess, 0);

            if (result.value() > 0) {
                memcpy(m_inRight, m_inLeft, result.value() * sizeof(float));
                m_inputPeakL.store(calculatePeak(m_inLeft, result.value()), std::memory_order_relaxed);
                m_inputPeakR.store(m_inputPeakL.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
        } else if (channelCount == 2) {
            // Stereo input: read interleaved then deinterleave
            float interleavedInput[MAX_FRAMES * 2];
            oboe::ResultWithValue<int32_t> result =
                m_inputStream->read(interleavedInput, framesToProcess, 0);

            if (result.value() > 0) {
                deinterleave(interleavedInput, m_inLeft, m_inRight, result.value());
                m_inputPeakL.store(calculatePeak(m_inLeft, result.value()), std::memory_order_relaxed);
                m_inputPeakR.store(calculatePeak(m_inRight, result.value()), std::memory_order_relaxed);
            }
        }
    }

    // Process through NINJAM (mixes local input with remote streams)
    memset(m_outLeft, 0, framesToProcess * sizeof(float));
    memset(m_outRight, 0, framesToProcess * sizeof(float));

    if (m_client) {
        NinjamClient_processAudioSIMD(
            m_client,
            m_inLeft, m_inRight,
            m_outLeft, m_outRight,
            framesToProcess
        );
    }

    // Track output peaks
    m_outputPeakL.store(calculatePeak(m_outLeft, framesToProcess), std::memory_order_relaxed);
    m_outputPeakR.store(calculatePeak(m_outRight, framesToProcess), std::memory_order_relaxed);

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
