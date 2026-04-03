#include "OboeEngine.h"
#include <android/log.h>

#define LOG_TAG "OboeEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

OboeEngine::OboeEngine() {
    m_callback = std::make_shared<NinjamOboeCallback>();
    m_fxProcessor = std::make_unique<AudioFXProcessor>();
    m_recorder = std::make_unique<SessionRecorder>();
    m_callback->setFXProcessor(m_fxProcessor.get());
    m_callback->setRecorder(m_recorder.get());
}

OboeEngine::~OboeEngine() {
    stop();
}

void OboeEngine::setClient(NinjamClientRef* client) {
    m_client = client;
    m_callback->setClient(client);
}

bool OboeEngine::start(int32_t sampleRate, int32_t framesPerBuffer) {
    if (m_running.load()) {
        LOGI("Engine already running, stopping first");
        stop();
    }

    m_sampleRate = sampleRate;
    m_framesPerBuffer = framesPerBuffer;

    LOGI("Starting audio engine: %dHz, %d frames/buffer", sampleRate, framesPerBuffer);

    // Open output first (it drives the callback)
    if (!openOutputStream()) {
        LOGE("Failed to open output stream");
        return false;
    }

    // Open input for microphone capture
    if (!openInputStream()) {
        LOGE("Failed to open input stream (continuing without mic)");
        // Non-fatal: user can still hear remote audio without mic
    }

    // Wire input stream to callback so it can read mic data
    if (m_inputStream) {
        m_callback->setInputStream(m_inputStream.get());

        // Start input stream
        oboe::Result inputResult = m_inputStream->requestStart();
        if (inputResult != oboe::Result::OK) {
            LOGE("Failed to start input stream: %s", oboe::convertToText(inputResult));
            m_callback->setInputStream(nullptr);
            m_inputStream.reset();
        }
    }

    // Start output stream (this begins the audio callback)
    oboe::Result outputResult = m_outputStream->requestStart();
    if (outputResult != oboe::Result::OK) {
        LOGE("Failed to start output stream: %s", oboe::convertToText(outputResult));
        closeStreams();
        return false;
    }

    m_running.store(true);

    // Log actual negotiated parameters
    LOGI("Audio engine started successfully:");
    LOGI("  Output: %dHz, %d ch, %d frames, %s",
         m_outputStream->getSampleRate(),
         m_outputStream->getChannelCount(),
         m_outputStream->getFramesPerBurst(),
         oboe::convertToText(m_outputStream->getPerformanceMode()));
    if (m_inputStream) {
        LOGI("  Input:  %dHz, %d ch, %d frames",
             m_inputStream->getSampleRate(),
             m_inputStream->getChannelCount(),
             m_inputStream->getFramesPerBurst());
    }

    return true;
}

void OboeEngine::stop() {
    if (!m_running.load()) return;

    LOGI("Stopping audio engine");
    m_running.store(false);
    m_callback->setInputStream(nullptr);
    closeStreams();
}

bool OboeEngine::isRunning() const {
    return m_running.load();
}

int32_t OboeEngine::getSampleRate() const {
    if (m_outputStream) return m_outputStream->getSampleRate();
    return m_sampleRate;
}

int32_t OboeEngine::getFramesPerBuffer() const {
    if (m_outputStream) return m_outputStream->getFramesPerBurst();
    return m_framesPerBuffer;
}

void OboeEngine::getOutputPeaks(float* left, float* right) const {
    m_callback->getOutputPeaks(left, right);
}

void OboeEngine::getInputPeaks(float* left, float* right) const {
    m_callback->getInputPeaks(left, right);
}

void OboeEngine::setPerformanceMode(oboe::PerformanceMode mode) {
    m_performanceMode = mode;
}

void OboeEngine::setSharingMode(oboe::SharingMode mode) {
    m_sharingMode = mode;
}

// ============================================================================
// Private
// ============================================================================

bool OboeEngine::openOutputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(m_performanceMode)
           ->setSharingMode(m_sharingMode)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Stereo)
           ->setSampleRate(m_sampleRate)
           ->setFramesPerDataCallback(m_framesPerBuffer)
           ->setDataCallback(m_callback)
           ->setErrorCallback(m_callback)
           ->setUsage(oboe::Usage::Game);  // Low-latency usage hint

    oboe::Result result = builder.openStream(m_outputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return false;
    }

    // Update sample rate to what the device actually gave us
    m_sampleRate = m_outputStream->getSampleRate();
    return true;
}

bool OboeEngine::openInputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(m_performanceMode)
           ->setSharingMode(oboe::SharingMode::Shared)  // Input usually needs Shared
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Mono)   // Most mics are mono
           ->setSampleRate(m_sampleRate)                  // Match output sample rate
           ->setInputPreset(oboe::InputPreset::VoicePerformance);  // Low-latency mic preset

    oboe::Result result = builder.openStream(m_inputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }

    return true;
}

void OboeEngine::closeStreams() {
    if (m_inputStream) {
        m_inputStream->requestStop();
        m_inputStream->close();
        m_inputStream.reset();
    }
    if (m_outputStream) {
        m_outputStream->requestStop();
        m_outputStream->close();
        m_outputStream.reset();
    }
}
