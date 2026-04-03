#include "SessionRecorder.h"
#include <android/log.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

#define LOG_TAG "SessionRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================================
// Lifecycle
// ============================================================================

SessionRecorder::SessionRecorder() = default;

SessionRecorder::~SessionRecorder() {
    if (m_state.load() == State::RECORDING) {
        m_encodeRunning.store(false);
        if (m_encodeThread.joinable()) m_encodeThread.join();
        teardownEncoder();
    }
}

// ============================================================================
// Start
// ============================================================================

bool SessionRecorder::start(const char* recordingsDir, const char* roomName,
                            const char* myUsername, const char** participants, int participantCount) {
    if (m_state.load() != State::IDLE) {
        LOGE("Cannot start recording — already recording");
        return false;
    }

    m_recordingsDir = recordingsDir;
    m_roomName = roomName ? roomName : "";
    m_myUsername = myUsername ? myUsername : "";
    m_participants.clear();
    for (int i = 0; i < participantCount; i++) {
        if (participants[i]) m_participants.emplace_back(participants[i]);
    }
    m_sampleCount = 0;
    m_encodedFrames = 0;
    m_consecutiveErrors = 0;
    m_startTimeMs = currentTimeMs();

    // Ensure recordings directory exists
    mkdir(recordingsDir, 0755);

    // Generate temp filename
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    m_tempFilePath = std::string(recordingsDir) + "/ninjamzap_recording_" + std::to_string(epoch) + ".m4a";

    // Setup AAC encoder + muxer
    if (!setupEncoder()) {
        LOGE("Failed to setup AAC encoder");
        return false;
    }

    // Clear ring buffer
    m_writePos.store(0, std::memory_order_relaxed);
    m_readPos.store(0, std::memory_order_relaxed);

    m_state.store(State::RECORDING, std::memory_order_release);

    // Start background encode thread
    m_encodeRunning.store(true);
    m_encodeThread = std::thread(&SessionRecorder::encodeLoop, this);

    LOGI("Recording started: %s", m_tempFilePath.c_str());
    return true;
}

// ============================================================================
// Stop
// ============================================================================

bool SessionRecorder::stop(const char** currentParticipants, int currentParticipantCount,
                           char* outTempPath, int tempPathLen,
                           char* outRoomName, int roomNameLen,
                           char* outSuggestedFilename, int filenameLen,
                           double* outDuration, int64_t* outFileSize) {
    if (m_state.load() != State::RECORDING) return false;

    m_state.store(State::STOPPING, std::memory_order_release);

    // Merge current participants
    if (currentParticipants) {
        for (int i = 0; i < currentParticipantCount; i++) {
            if (!currentParticipants[i]) continue;
            std::string p(currentParticipants[i]);
            bool found = false;
            for (const auto& existing : m_participants) {
                if (existing == p) { found = true; break; }
            }
            if (!found) m_participants.push_back(p);
        }
    }

    LOGI("Stopping recording...");

    // Stop encode thread
    m_encodeRunning.store(false);
    if (m_encodeThread.joinable()) m_encodeThread.join();

    // Final drain
    drainRingBuffer();

    // Signal end of stream to encoder
    if (m_codec) {
        ssize_t bufIdx = AMediaCodec_dequeueInputBuffer(m_codec, 100000); // 100ms timeout
        if (bufIdx >= 0) {
            AMediaCodec_queueInputBuffer(m_codec, bufIdx, 0, 0, 0,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        }
        // Drain remaining encoded data
        drainEncoder();
    }

    // Teardown encoder
    teardownEncoder();

    // Calculate metadata
    double duration = (currentTimeMs() - m_startTimeMs) / 1000.0;
    int64_t fileSize = 0;
    struct stat st;
    if (stat(m_tempFilePath.c_str(), &st) == 0) {
        fileSize = st.st_size;
    }

    // Fill out params
    if (outTempPath) strncpy(outTempPath, m_tempFilePath.c_str(), tempPathLen - 1);
    if (outRoomName) strncpy(outRoomName, m_roomName.c_str(), roomNameLen - 1);
    if (outSuggestedFilename) {
        std::string suggested = generateFilename();
        strncpy(outSuggestedFilename, suggested.c_str(), filenameLen - 1);
    }
    if (outDuration) *outDuration = duration;
    if (outFileSize) *outFileSize = fileSize;

    m_state.store(State::IDLE, std::memory_order_release);
    LOGI("Recording stopped: %.1fs, %lld bytes", duration, (long long)fileSize);
    return true;
}

// ============================================================================
// Save / Discard
// ============================================================================

bool SessionRecorder::save(const char* finalPath) {
    if (m_tempFilePath.empty()) return false;

    // Remove existing file at destination
    unlink(finalPath);

    if (rename(m_tempFilePath.c_str(), finalPath) != 0) {
        LOGE("Failed to rename %s -> %s: %s", m_tempFilePath.c_str(), finalPath, strerror(errno));
        return false;
    }

    LOGI("Recording saved: %s", finalPath);
    m_tempFilePath.clear();
    return true;
}

void SessionRecorder::discard() {
    if (m_state.load() == State::RECORDING) {
        m_encodeRunning.store(false);
        if (m_encodeThread.joinable()) m_encodeThread.join();
        teardownEncoder();
        m_state.store(State::IDLE, std::memory_order_release);
    }

    if (!m_tempFilePath.empty()) {
        unlink(m_tempFilePath.c_str());
        LOGI("Recording discarded");
        m_tempFilePath.clear();
    }
}

// ============================================================================
// Audio Thread Interface (lock-free)
// ============================================================================

void SessionRecorder::writeSamples(const float* left, const float* right, int count) {
    if (m_state.load(std::memory_order_acquire) != State::RECORDING) return;

    int wp = m_writePos.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        m_ringL[wp] = left[i];
        m_ringR[wp] = right[i];
        wp = (wp + 1) % RING_BUFFER_SIZE;
    }
    m_writePos.store(wp, std::memory_order_release);
    m_sampleCount += count;
}

double SessionRecorder::getElapsedTime() const {
    if (m_state.load() != State::RECORDING) return 0;
    return (currentTimeMs() - m_startTimeMs) / 1000.0;
}

// ============================================================================
// Background Encode Thread
// ============================================================================

void SessionRecorder::encodeLoop() {
    LOGI("Encode thread started");
    while (m_encodeRunning.load()) {
        drainRingBuffer();
        std::this_thread::sleep_for(std::chrono::milliseconds(ENCODE_INTERVAL_MS));
    }
    LOGI("Encode thread stopped");
}

void SessionRecorder::drainRingBuffer() {
    if (!m_codec || m_state.load(std::memory_order_acquire) == State::IDLE) return;

    int wp = m_writePos.load(std::memory_order_acquire);
    int rp = m_readPos.load(std::memory_order_relaxed);
    int available = (wp - rp + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    if (available <= 0) return;

    // Interleave L/R into temp buffer
    // Process in chunks to avoid huge stack allocation
    static constexpr int CHUNK_FRAMES = 4096;
    float interleaved[CHUNK_FRAMES * CHANNELS];

    while (available > 0) {
        int framesToProcess = std::min(available, CHUNK_FRAMES);

        for (int i = 0; i < framesToProcess; i++) {
            int idx = (rp + i) % RING_BUFFER_SIZE;
            interleaved[i * 2] = m_ringL[idx];
            interleaved[i * 2 + 1] = m_ringR[idx];
        }

        if (!feedEncoder(interleaved, framesToProcess)) {
            m_consecutiveErrors++;
            if (m_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
                LOGE("Too many encode errors, stopping recording");
                m_encodeRunning.store(false);
                return;
            }
        } else {
            m_consecutiveErrors = 0;
        }

        rp = (rp + framesToProcess) % RING_BUFFER_SIZE;
        m_readPos.store(rp, std::memory_order_release);
        available -= framesToProcess;
    }

    // Drain encoded output
    drainEncoder();
}

// ============================================================================
// AAC Encoder
// ============================================================================

bool SessionRecorder::setupEncoder() {
    // Create AAC encoder
    m_codec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!m_codec) {
        LOGE("Failed to create AAC encoder");
        return false;
    }

    // Configure encoder
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, SAMPLE_RATE);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, CHANNELS);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, BIT_RATE);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_AAC_PROFILE, 2); // AAC-LC

    media_status_t status = AMediaCodec_configure(m_codec, format, nullptr, nullptr,
                                                   AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK) {
        LOGE("Failed to configure AAC encoder: %d", status);
        AMediaCodec_delete(m_codec);
        m_codec = nullptr;
        return false;
    }

    status = AMediaCodec_start(m_codec);
    if (status != AMEDIA_OK) {
        LOGE("Failed to start AAC encoder: %d", status);
        AMediaCodec_delete(m_codec);
        m_codec = nullptr;
        return false;
    }

    // Create muxer (M4A container)
    int fd = open(m_tempFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOGE("Failed to open output file: %s (%s)", m_tempFilePath.c_str(), strerror(errno));
        AMediaCodec_stop(m_codec);
        AMediaCodec_delete(m_codec);
        m_codec = nullptr;
        return false;
    }

    m_muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    close(fd);

    if (!m_muxer) {
        LOGE("Failed to create media muxer");
        AMediaCodec_stop(m_codec);
        AMediaCodec_delete(m_codec);
        m_codec = nullptr;
        return false;
    }

    m_trackIndex = -1;
    m_muxerStarted = false;

    LOGI("AAC encoder setup: %dHz, %dch, %dbps", SAMPLE_RATE, CHANNELS, BIT_RATE);
    return true;
}

void SessionRecorder::teardownEncoder() {
    if (m_muxer && m_muxerStarted) {
        AMediaMuxer_stop(m_muxer);
    }
    if (m_muxer) {
        AMediaMuxer_delete(m_muxer);
        m_muxer = nullptr;
    }
    if (m_codec) {
        AMediaCodec_stop(m_codec);
        AMediaCodec_delete(m_codec);
        m_codec = nullptr;
    }
    m_trackIndex = -1;
    m_muxerStarted = false;
}

bool SessionRecorder::feedEncoder(const float* interleaved, int frameCount) {
    if (!m_codec) return false;

    // Convert float [-1,1] to int16 PCM for the encoder
    int sampleCount = frameCount * CHANNELS;
    int16_t pcmBuffer[sampleCount];
    for (int i = 0; i < sampleCount; i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, interleaved[i]));
        pcmBuffer[i] = static_cast<int16_t>(clamped * 32767.0f);
    }

    int bytesNeeded = sampleCount * sizeof(int16_t);
    int bytesWritten = 0;

    while (bytesWritten < bytesNeeded) {
        ssize_t bufIdx = AMediaCodec_dequeueInputBuffer(m_codec, 10000); // 10ms timeout
        if (bufIdx < 0) {
            // No input buffer available — drain output first
            drainEncoder();
            bufIdx = AMediaCodec_dequeueInputBuffer(m_codec, 10000);
            if (bufIdx < 0) return false; // Still no buffer
        }

        size_t bufSize = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, bufIdx, &bufSize);
        if (!buf) return false;

        int bytesToCopy = std::min((int)bufSize, bytesNeeded - bytesWritten);
        memcpy(buf, reinterpret_cast<uint8_t*>(pcmBuffer) + bytesWritten, bytesToCopy);

        // Calculate presentation time from encoder-side frame counter
        // bytesToCopy covers (bytesToCopy / (CHANNELS * sizeof(int16_t))) frames
        int framesInChunk = bytesToCopy / (CHANNELS * sizeof(int16_t));
        int64_t presentationTimeUs = (m_encodedFrames * 1000000LL) / SAMPLE_RATE;
        m_encodedFrames += framesInChunk;

        AMediaCodec_queueInputBuffer(m_codec, bufIdx, 0, bytesToCopy, presentationTimeUs, 0);
        bytesWritten += bytesToCopy;
    }

    return true;
}

bool SessionRecorder::drainEncoder() {
    if (!m_codec || !m_muxer) return false;

    AMediaCodecBufferInfo info;
    while (true) {
        ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(m_codec, &info, 0); // Non-blocking

        if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // Get the output format and add track to muxer
            AMediaFormat* outFormat = AMediaCodec_getOutputFormat(m_codec);
            m_trackIndex = AMediaMuxer_addTrack(m_muxer, outFormat);
            AMediaFormat_delete(outFormat);

            if (m_trackIndex >= 0) {
                AMediaMuxer_start(m_muxer);
                m_muxerStarted = true;
                LOGI("Muxer started, track index: %d", m_trackIndex);
            }
            continue;
        }

        if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            break; // No more output available
        }

        if (outIdx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue; // Deprecated but handle gracefully
        }

        if (outIdx < 0) break; // Error

        // Write encoded data to muxer
        if (m_muxerStarted && m_trackIndex >= 0 && info.size > 0) {
            size_t bufSize = 0;
            uint8_t* buf = AMediaCodec_getOutputBuffer(m_codec, outIdx, &bufSize);
            if (buf) {
                AMediaMuxer_writeSampleData(m_muxer, m_trackIndex, buf, &info);
            }
        }

        AMediaCodec_releaseOutputBuffer(m_codec, outIdx, false);

        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
            LOGI("End of stream reached");
            break;
        }
    }
    return true;
}

// ============================================================================
// Filename Generation (mirrors iOS RecordingMetadata.generateFilename)
// ============================================================================

std::string SessionRecorder::generateFilename() const {
    // Date string: yyyyMMdd_HHmmss
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);
    char dateStr[32];
    strftime(dateStr, sizeof(dateStr), "%Y%m%d_%H%M%S", &tm_buf);

    // Sanitize room name
    std::string room = sanitize(m_roomName, 20);

    // Filter participants (skip ninbot_*, strip @ip)
    std::vector<std::string> filtered;
    for (const auto& p : m_participants) {
        if (p.rfind("ninbot_", 0) == 0) continue; // starts with ninbot_
        std::string name = p;
        auto atPos = name.find('@');
        if (atPos != std::string::npos) name = name.substr(0, atPos);
        // Remove spaces
        name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
        if (!name.empty()) filtered.push_back(name);
    }

    // Add my username
    {
        std::string me = m_myUsername;
        auto atPos = me.find('@');
        if (atPos != std::string::npos) me = me.substr(0, atPos);
        me.erase(std::remove(me.begin(), me.end(), ' '), me.end());
        if (!me.empty()) {
            bool found = false;
            for (const auto& f : filtered) {
                if (f == me) { found = true; break; }
            }
            if (!found) filtered.push_back(me);
        }
    }

    // Limit to 5 participants
    std::string participantsStr;
    int count = 0;
    for (const auto& f : filtered) {
        if (count >= 5) break;
        if (!participantsStr.empty()) participantsStr += "-";
        participantsStr += f;
        count++;
    }

    return std::string(dateStr) + "_" + room + "_" + participantsStr + ".m4a";
}

std::string SessionRecorder::sanitize(const std::string& str, int maxLen) {
    std::string result;
    for (char c : str) {
        if (std::isalnum(c)) result += c;
        if ((int)result.size() >= maxLen) break;
    }
    return result;
}

double SessionRecorder::currentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}
