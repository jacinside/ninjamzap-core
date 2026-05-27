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

    // Derive latency profile from framesPerBuffer (passed by Kotlin as
    // FeaturesFlags.inputBufferSize). JS calls setFeatureFlag BEFORE the
    // engine exists, so the JS→Kotlin pathway can't push setLatencyProfile
    // in time — apply the same mapping here at engine start so the first
    // openOutput/openInput pair already gets the right buffer multiplier
    // and performance mode.
    if (framesPerBuffer <= 128) {
        m_bufferMultiplier.store(2);
        m_performanceMode = oboe::PerformanceMode::LowLatency;
    } else if (framesPerBuffer <= 256) {
        m_bufferMultiplier.store(2);
        m_performanceMode = oboe::PerformanceMode::LowLatency;
    } else {
        m_bufferMultiplier.store(3);
        m_performanceMode = oboe::PerformanceMode::None;
    }
    LOGI("Starting audio engine: %dHz, %d frames/buffer (latency profile: multiplier=%d perfMode=%s)",
         sampleRate, framesPerBuffer,
         m_bufferMultiplier.load(), oboe::convertToText(m_performanceMode));

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

int32_t OboeEngine::getOutputBurst() const {
    return m_outputStream ? m_outputStream->getFramesPerBurst() : 0;
}

int32_t OboeEngine::getOutputBufferSize() const {
    return m_outputStream ? m_outputStream->getBufferSizeInFrames() : 0;
}

int32_t OboeEngine::getInputBurst() const {
    return m_inputStream ? m_inputStream->getFramesPerBurst() : 0;
}

int32_t OboeEngine::getInputBufferSize() const {
    return m_inputStream ? m_inputStream->getBufferSizeInFrames() : 0;
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

void OboeEngine::setLatencyProfile(int profile) {
    int32_t multiplier;
    oboe::PerformanceMode mode;
    switch (profile) {
        case 0: multiplier = 2; mode = oboe::PerformanceMode::LowLatency; break;  // ultra_low
        case 1: multiplier = 2; mode = oboe::PerformanceMode::LowLatency; break;  // low
        case 2:
        default: multiplier = 3; mode = oboe::PerformanceMode::None;       break; // safe
    }
    int32_t prevMult = m_bufferMultiplier.exchange(multiplier);
    oboe::PerformanceMode prevMode = m_performanceMode;
    m_performanceMode = mode;
    if (prevMult == multiplier && prevMode == mode) return;
    LOGI("Latency profile: %d (multiplier=%d perfMode=%s) — reopening",
         profile, multiplier, oboe::convertToText(mode));
    std::lock_guard<std::mutex> lock(m_streamMutex);
    if (m_running.load()) {
        reopenStreamsLocked();
    }
}

// ============================================================================
// Private
// ============================================================================

bool OboeEngine::openOutputStream() {
    // Try Exclusive (MMAP) FIRST regardless of deviceId — bypasses the
    // AAudio system mixer for ~5–10 ms less audible latency. The historical
    // concern was that Exclusive can ignore setDeviceId and silently route
    // to the platform default endpoint; we guard against that explicitly
    // below by comparing requested vs actual deviceId after open and
    // retrying as Shared if they don't match. This is the only knob that
    // moves audible latency below ~30 ms on Oboe-based Android audio.
    int32_t devId = m_outputDeviceId.load();
    oboe::SharingMode sharing = oboe::SharingMode::Exclusive;
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

    // If Exclusive routed us to a different deviceId than requested (HAL
    // doesn't support Exclusive on this endpoint), close and retry as
    // Shared so the explicit device pick is honored.
    if (result == oboe::Result::OK && devId != oboe::kUnspecified &&
        m_outputStream->getDeviceId() != devId) {
        LOGI("Exclusive output routed to deviceId=%d but requested %d — retrying Shared",
             m_outputStream->getDeviceId(), devId);
        m_outputStream->close();
        m_outputStream.reset();
        builder.setSharingMode(oboe::SharingMode::Shared);
        sharing = oboe::SharingMode::Shared;
        result = builder.openStream(m_outputStream);
    }

    if (result != oboe::Result::OK) {
        // Final fallback: if Exclusive failed outright, try Shared once.
        if (sharing == oboe::SharingMode::Exclusive) {
            LOGI("Exclusive output open failed (%s) — retrying Shared",
                 oboe::convertToText(result));
            builder.setSharingMode(oboe::SharingMode::Shared);
            sharing = oboe::SharingMode::Shared;
            result = builder.openStream(m_outputStream);
        }
        if (result != oboe::Result::OK) {
            LOGE("Failed to open output stream: %s", oboe::convertToText(result));
            return false;
        }
    }

    // Update sample rate to what the device actually gave us
    m_sampleRate = m_outputStream->getSampleRate();

    // Shrink the queue depth to (burst × multiplier). Default is much larger
    // for jitter safety; trimming it is the only knob that meaningfully
    // reduces audible latency on the Oboe path. The actual value AAudio
    // grants may be larger than requested.
    int32_t outBurst = m_outputStream->getFramesPerBurst();
    int32_t requestedOutBuf = outBurst * m_bufferMultiplier.load();
    auto outBufRes = m_outputStream->setBufferSizeInFrames(requestedOutBuf);
    int32_t actualOutBuf = outBufRes ? outBufRes.value() : m_outputStream->getBufferSizeInFrames();
    LOGI("Output stream opened: requested deviceId=%d actual deviceId=%d sharing=%s perfMode=%s burst=%d bufSize req=%d actual=%d",
         m_outputDeviceId.load(), m_outputStream->getDeviceId(),
         oboe::convertToText(m_outputStream->getSharingMode()),
         oboe::convertToText(m_outputStream->getPerformanceMode()),
         outBurst, requestedOutBuf, actualOutBuf);
    return true;
}

bool OboeEngine::openInputStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(m_performanceMode)
           ->setSharingMode(oboe::SharingMode::Exclusive)  // Try MMAP first; fall back to Shared below if rejected
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

    int32_t inDevId = m_inputDeviceId.load();
    if (result == oboe::Result::OK && inDevId != oboe::kUnspecified &&
        m_inputStream->getDeviceId() != inDevId) {
        LOGI("Exclusive input routed to deviceId=%d but requested %d — retrying Shared",
             m_inputStream->getDeviceId(), inDevId);
        m_inputStream->close();
        m_inputStream.reset();
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(m_inputStream);
    }
    if (result != oboe::Result::OK) {
        LOGI("Exclusive input open failed (%s) — retrying Shared",
             oboe::convertToText(result));
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(m_inputStream);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open input stream: %s", oboe::convertToText(result));
            return false;
        }
    }
    int32_t inBurst = m_inputStream->getFramesPerBurst();
    int32_t requestedInBuf = inBurst * m_bufferMultiplier.load();
    auto inBufRes = m_inputStream->setBufferSizeInFrames(requestedInBuf);
    int32_t actualInBuf = inBufRes ? inBufRes.value() : m_inputStream->getBufferSizeInFrames();
    LOGI("Input stream opened: requested deviceId=%d actual deviceId=%d sharing=%s burst=%d bufSize req=%d actual=%d",
         m_inputDeviceId.load(), m_inputStream->getDeviceId(),
         oboe::convertToText(m_inputStream->getSharingMode()),
         inBurst, requestedInBuf, actualInBuf);
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
