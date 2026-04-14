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
    int32_t outRate = m_outputStream->getSampleRate();
    int32_t outBurst = m_outputStream->getFramesPerBurst();
    int32_t outBufCap = m_outputStream->getBufferCapacityInFrames();
    int32_t outBufSize = m_outputStream->getBufferSizeInFrames();
    double outLatencyMs = (double)outBurst / outRate * 1000.0;
    double outBufMs = (double)outBufSize / outRate * 1000.0;

    LOGI("Audio engine started successfully:");
    LOGI("  Output: %dHz, %d ch, burst=%d (%.1fms), bufSize=%d (%.1fms), bufCap=%d, %s %s",
         outRate, m_outputStream->getChannelCount(),
         outBurst, outLatencyMs,
         outBufSize, outBufMs,
         outBufCap,
         oboe::convertToText(m_outputStream->getPerformanceMode()),
         oboe::convertToText(m_outputStream->getSharingMode()));
    if (m_inputStream) {
        int32_t inRate = m_inputStream->getSampleRate();
        int32_t inBurst = m_inputStream->getFramesPerBurst();
        int32_t inBufSize = m_inputStream->getBufferSizeInFrames();
        double inLatencyMs = (double)inBurst / inRate * 1000.0;
        double inBufMs = (double)inBufSize / inRate * 1000.0;
        LOGI("  Input:  %dHz, %d ch, burst=%d (%.1fms), bufSize=%d (%.1fms), %s %s",
             inRate, m_inputStream->getChannelCount(),
             inBurst, inLatencyMs,
             inBufSize, inBufMs,
             oboe::convertToText(m_inputStream->getPerformanceMode()),
             oboe::convertToText(m_inputStream->getSharingMode()));
        double totalMs = inLatencyMs + outLatencyMs;
        LOGI("  Estimated roundtrip: %.1fms (input burst + output burst)", totalMs);
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

int32_t OboeEngine::getOutputBurstSize() const {
    return m_outputStream ? m_outputStream->getFramesPerBurst() : 0;
}

int32_t OboeEngine::getOutputBufferSize() const {
    return m_outputStream ? m_outputStream->getBufferSizeInFrames() : 0;
}

int32_t OboeEngine::getInputBurstSize() const {
    return m_inputStream ? m_inputStream->getFramesPerBurst() : 0;
}

int32_t OboeEngine::getInputBufferSize() const {
    return m_inputStream ? m_inputStream->getBufferSizeInFrames() : 0;
}

void OboeEngine::setDirectMonitor(bool enabled) {
    LOGI("setDirectMonitor: %s", enabled ? "ON" : "OFF");
    m_callback->setDirectMonitor(enabled);
}

void OboeEngine::setDirectMonitorGain(float gain) {
    m_callback->setDirectMonitorGain(gain);
}

void OboeEngine::setInputDeviceId(int32_t deviceId) {
    LOGI("setInputDeviceId: %d", deviceId);
    m_inputDeviceId = deviceId;
    if (m_running.load()) {
        // Hot-swap input stream without touching output
        if (m_inputStream) {
            m_callback->setInputStream(nullptr);
            m_inputStream->requestStop();
            m_inputStream->close();
            m_inputStream.reset();
        }
        if (openInputStream()) {
            m_callback->setInputStream(m_inputStream.get());
            oboe::Result result = m_inputStream->requestStart();
            if (result != oboe::Result::OK) {
                LOGE("Failed to restart input stream: %s", oboe::convertToText(result));
                m_callback->setInputStream(nullptr);
                m_inputStream.reset();
            } else {
                LOGI("Input stream restarted with device %d (%dHz, %d ch)",
                     deviceId, m_inputStream->getSampleRate(), m_inputStream->getChannelCount());
            }
        }
    }
}

void OboeEngine::setOutputDeviceId(int32_t deviceId) {
    LOGI("setOutputDeviceId: %d", deviceId);
    m_outputDeviceId = deviceId;
    if (m_running.load()) {
        // Output drives the audio callback — full restart required
        int32_t savedSampleRate = m_sampleRate;
        int32_t savedFramesPerBuffer = m_framesPerBuffer;
        stop();
        start(savedSampleRate, savedFramesPerBuffer);
    }
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

    if (m_outputDeviceId != oboe::kUnspecified) {
        builder.setDeviceId(m_outputDeviceId);
        LOGI("Output stream targeting device %d", m_outputDeviceId);
    }

    oboe::Result result = builder.openStream(m_outputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return false;
    }

    // Update sample rate to what the device actually gave us
    m_sampleRate = m_outputStream->getSampleRate();

    // Set output buffer size based on latency profile.
    // m_framesPerBuffer encodes the user's latency preference:
    //   128 (ultra_low) → 1 burst — minimum latency, may glitch on weak devices
    //   256 (low)       → 2 bursts — good balance
    //   512 (safe)      → 3 bursts — safest, higher latency
    int32_t burst = m_outputStream->getFramesPerBurst();
    int32_t numBursts = (m_framesPerBuffer <= 128) ? 1 : (m_framesPerBuffer <= 256) ? 2 : 3;
    m_outputStream->setBufferSizeInFrames(burst * numBursts);
    LOGI("Output buffer: %d bursts x %d frames = %d frames (%.1fms)",
         numBursts, burst, burst * numBursts,
         (double)(burst * numBursts) / m_sampleRate * 1000.0);

    return true;
}

bool OboeEngine::openInputStream() {
    oboe::AudioStreamBuilder builder;

    // USB audio interfaces typically expose stereo (or more) channels.
    // Built-in mics are mono. Request stereo when a specific device is
    // selected so we capture channels 1+2; fall back to mono for the
    // default built-in mic. The callback already handles both cases.
    auto channelCount = (m_inputDeviceId != oboe::kUnspecified)
        ? oboe::ChannelCount::Stereo
        : oboe::ChannelCount::Mono;

    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(m_performanceMode)
           ->setSharingMode(oboe::SharingMode::Shared)  // Input usually needs Shared
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(channelCount)
           ->setSampleRate(m_sampleRate)                  // Match output sample rate
           ->setInputPreset(oboe::InputPreset::VoicePerformance);  // Low-latency mic preset

    if (m_inputDeviceId != oboe::kUnspecified) {
        builder.setDeviceId(m_inputDeviceId);
        LOGI("Input stream targeting device %d (stereo)", m_inputDeviceId);
    }

    oboe::Result result = builder.openStream(m_inputStream);

    // If stereo open failed on the USB device, fall back to mono
    if (result != oboe::Result::OK && channelCount == oboe::ChannelCount::Stereo) {
        LOGI("Stereo input failed, falling back to mono");
        builder.setChannelCount(oboe::ChannelCount::Mono);
        result = builder.openStream(m_inputStream);
    }

    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }

    // Set input buffer size matching output latency profile
    int32_t inBurst = m_inputStream->getFramesPerBurst();
    int32_t inNumBursts = (m_framesPerBuffer <= 128) ? 1 : (m_framesPerBuffer <= 256) ? 2 : 3;
    m_inputStream->setBufferSizeInFrames(inBurst * inNumBursts);

    LOGI("Input stream opened: %d ch, %dHz, burst=%d (%.1fms), bufSize=%d (%d bursts)",
         m_inputStream->getChannelCount(), m_inputStream->getSampleRate(),
         inBurst, (double)inBurst / m_inputStream->getSampleRate() * 1000.0,
         m_inputStream->getBufferSizeInFrames(), inNumBursts);
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
