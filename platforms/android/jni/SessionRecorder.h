#ifndef SESSION_RECORDER_H
#define SESSION_RECORDER_H

#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <cstdint>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>

/**
 * Records NINJAM session audio to AAC .m4a files.
 *
 * Architecture (mirrors iOS SessionRecorder.swift):
 *   Audio thread → lock-free ring buffer → background encode thread → AAC → .m4a file
 *
 * The audio thread calls writeSamples() which is lock-free and real-time safe.
 * A background thread drains the ring buffer every 50ms, feeds samples to
 * AMediaCodec (AAC encoder), and writes encoded packets via AMediaMuxer.
 *
 * Thread safety:
 *   - writeSamples(): called from Oboe audio thread (lock-free)
 *   - start/stop/save/discard: called from JNI thread (main/session thread)
 *   - encode loop: runs on dedicated background thread
 */
class SessionRecorder {
public:
    enum class State { IDLE, RECORDING, STOPPING };

    SessionRecorder();
    ~SessionRecorder();

    // Start recording to a temp file in the given directory
    bool start(const char* recordingsDir, const char* roomName,
               const char* myUsername, const char** participants, int participantCount);

    // Stop recording and return metadata via out params
    // Returns true if recording was active
    bool stop(const char** currentParticipants, int currentParticipantCount,
              // Out params:
              char* outTempPath, int tempPathLen,
              char* outRoomName, int roomNameLen,
              char* outSuggestedFilename, int filenameLen,
              double* outDuration, int64_t* outFileSize);

    // Save temp recording to final path
    bool save(const char* finalPath);

    // Discard temp recording
    void discard();

    // Called from audio thread — must be lock-free and real-time safe
    void writeSamples(const float* left, const float* right, int count);

    // Get elapsed recording time in seconds
    double getElapsedTime() const;

    // State
    State getState() const { return m_state.load(std::memory_order_acquire); }
    bool isRecording() const { return m_state.load(std::memory_order_acquire) == State::RECORDING; }

private:
    // Config
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS = 2;
    static constexpr int BIT_RATE = 128000;
    static constexpr int RING_BUFFER_SECONDS = 2;
    static constexpr int RING_BUFFER_SIZE = SAMPLE_RATE * RING_BUFFER_SECONDS;
    static constexpr int ENCODE_INTERVAL_MS = 50;
    static constexpr int MAX_CONSECUTIVE_ERRORS = 10;

    // State
    std::atomic<State> m_state{State::IDLE};

    // Ring buffers (SPSC: audio thread writes, encode thread reads)
    float m_ringL[RING_BUFFER_SIZE] = {};
    float m_ringR[RING_BUFFER_SIZE] = {};
    std::atomic<int> m_writePos{0};
    std::atomic<int> m_readPos{0};

    // Metadata
    std::string m_roomName;
    std::string m_myUsername;
    std::vector<std::string> m_participants;
    int64_t m_sampleCount = 0;
    double m_startTimeMs = 0;

    // File
    std::string m_tempFilePath;
    std::string m_recordingsDir;

    // AAC encoder
    AMediaCodec* m_codec = nullptr;
    AMediaMuxer* m_muxer = nullptr;
    int m_trackIndex = -1;
    bool m_muxerStarted = false;

    // Background encode thread
    std::thread m_encodeThread;
    std::atomic<bool> m_encodeRunning{false};
    int m_consecutiveErrors = 0;

    // Encoder-side sample counter (only updated on encode thread)
    int64_t m_encodedFrames = 0;

    // Encode loop
    void encodeLoop();
    void drainRingBuffer();
    bool feedEncoder(const float* interleaved, int frameCount);
    bool drainEncoder();

    // Setup/teardown
    bool setupEncoder();
    void teardownEncoder();

    // Filename generation (mirrors iOS)
    std::string generateFilename() const;
    static std::string sanitize(const std::string& str, int maxLen);
    static double currentTimeMs();
};

#endif // SESSION_RECORDER_H
