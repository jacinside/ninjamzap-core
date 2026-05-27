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
    std::lock_guard<std::mutex> lock(m_streamMutex);

    if (m_running.load()) {
        LOGI("Engine already running, closing streams first");
        m_running.store(false);
        m_callback->setInputStream(nullptr);
        closeStreams();
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
    std::lock_guard<std::mutex> lock(m_streamMutex);
    if (!m_running.load()) return;

    LOGI("Stopping audio engine");
    m_running.store(false);
    m_callback->setInputStream(nullptr);
    closeStreams();
}

int32_t OboeEngine::getInputSessionId() const {
    if (!m_inputStream) return 0;  // oboe::SessionId::None
    return m_inputStream->getSessionId();
}

void OboeEngine::setInputDeviceId(int32_t deviceId) {
    int32_t prev = m_inputDeviceId.exchange(deviceId);
    if (prev == deviceId) return;
    LOGI("Input device changed: %d -> %d", prev, deviceId);
    std::lock_guard<std::mutex> lock(m_streamMutex);
    if (m_running.load()) {
        reopenStreamsLocked();
    }
}

void OboeEngine::setOutputDeviceId(int32_t deviceId) {
    int32_t prev = m_outputDeviceId.exchange(deviceId);
    if (prev == deviceId) return;
    LOGI("Output device changed: %d -> %d", prev, deviceId);
    std::lock_guard<std::mutex> lock(m_streamMutex);
    if (m_running.load()) {
        reopenStreamsLocked();
    }
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

void OboeEngine::setDirectMonitor(bool enabled) {
    if (m_callback) m_callback->setDirectMonitor(enabled);
    LOGI("Direct monitor: %s", enabled ? "ON" : "OFF");
}

void OboeEngine::setLocalGain(float gain) {
    if (m_callback) m_callback->setLocalGain(gain);
}

void OboeEngine::setMasterGain(float gain) {
    if (m_callback) m_callback->setMasterGain(gain);
}

void OboeEngine::setInputPreset(oboe::InputPreset preset) {
    oboe::InputPreset prev = m_inputPreset.exchange(preset);
    if (prev == preset) return;
    LOGI("Input preset changed: %d -> %d (forces input stream reopen)",
         static_cast<int>(prev), static_cast<int>(preset));
    std::lock_guard<std::mutex> lock(m_streamMutex);
    if (m_running.load()) {
        reopenStreamsLocked();
    }
}

// ============================================================================
// Private
// ============================================================================

bool OboeEngine::openOutputStream() {
    // When the user has explicitly picked an output device, fall back to
    // Shared sharing mode. Exclusive (MMAP) ignores setDeviceId on many HAL
    // endpoints (only the default endpoint is exclusive-capable), so an
    // explicit pick would silently route to default. Shared honors the
    // device ID universally at the small cost of one extra HW mixer hop —
    // acceptable on the route-change path. Auto (kUnspecified) keeps the
    // low-latency Exclusive path.
    int32_t devId = m_outputDeviceId.load();
    oboe::SharingMode sharing = (devId == oboe::kUnspecified)
        ? m_sharingMode
        : oboe::SharingMode::Shared;
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(m_performanceMode)
           ->setSharingMode(sharing)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Stereo)
           ->setSampleRate(m_sampleRate)
           ->setFramesPerDataCallback(m_framesPerBuffer)
           ->setDataCallback(m_callback)
           ->setErrorCallback(m_callback)
           ->setDeviceId(devId)
           ->setUsage(oboe::Usage::Game);  // Low-latency usage hint

    oboe::Result result = builder.openStream(m_outputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return false;
    }

    // Update sample rate to what the device actually gave us
    m_sampleRate = m_outputStream->getSampleRate();
    LOGI("Output stream opened: requested deviceId=%d actual deviceId=%d sharing=%s",
         m_outputDeviceId.load(), m_outputStream->getDeviceId(),
         oboe::convertToText(m_outputStream->getSharingMode()));
    return true;
}

bool OboeEngine::openInputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(m_performanceMode)
           ->setSharingMode(oboe::SharingMode::Shared)  // Input usually needs Shared
           ->setFormat(oboe::AudioFormat::Float)
           // Open input as stereo. The built-in mic delivers mono (Android
           // up-mixes mono → stereo cleanly) but USB interfaces like the iRig
           // PRO DUO have two real channels (XLR mic + instrument). Forcing
           // Mono on a stereo USB device collapses both inputs into one
           // channel and we lose the second input. Stereo + the existing
           // deinterleave path keeps the user's two inputs distinct.
           ->setChannelCount(oboe::ChannelCount::Stereo)
           ->setSampleRate(m_sampleRate)                  // Match output sample rate
           ->setDeviceId(m_inputDeviceId.load())
           // Allocate an Android audio session ID so Kotlin can attach
           // AcousticEchoCanceler / NoiseSuppressor / AutomaticGainControl
           // to this stream (the AEC toggle in the connection screen).
           // Without Allocate, getSessionId() returns kSessionIdNone (0) and
           // AcousticEchoCanceler.create() rejects it.
           ->setSessionId(oboe::SessionId::Allocate)
           ->setInputPreset(m_inputPreset.load());  // VoicePerformance default; Unprocessed during video

    oboe::Result result = builder.openStream(m_inputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }
    LOGI("Input stream opened: requested deviceId=%d actual deviceId=%d sharing=%s",
         m_inputDeviceId.load(), m_inputStream->getDeviceId(),
         oboe::convertToText(m_inputStream->getSharingMode()));
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

bool OboeEngine::reopenStreamsLocked() {
    LOGI("Reopening streams (input=%d output=%d)",
         m_inputDeviceId.load(), m_outputDeviceId.load());
    m_running.store(false);
    m_callback->setInputStream(nullptr);
    closeStreams();

    if (!openOutputStream()) {
        LOGE("reopen: output failed");
        return false;
    }
    if (!openInputStream()) {
        LOGE("reopen: input failed (continuing without mic)");
    }
    if (m_inputStream) {
        m_callback->setInputStream(m_inputStream.get());
        oboe::Result r = m_inputStream->requestStart();
        if (r != oboe::Result::OK) {
            LOGE("reopen: input start failed: %s", oboe::convertToText(r));
            m_callback->setInputStream(nullptr);
            m_inputStream.reset();
        }
    }
    oboe::Result r = m_outputStream->requestStart();
    if (r != oboe::Result::OK) {
        LOGE("reopen: output start failed: %s", oboe::convertToText(r));
        closeStreams();
        return false;
    }
    m_running.store(true);
    return true;
}
