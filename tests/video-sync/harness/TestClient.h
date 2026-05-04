// TestClient — NJClient wrapper for host integration tests.
//
// One audio thread feeds AudioProc with silence at realtime sample rate; one network
// thread drives Run(). Optionally registers an audio (ch0) and a video (ch1, fourcc=H264,
// flags=0x10) local channel. Records every callback into thread-safe queues that the test
// can drain after the fact. Designed so each test case constructs and tears down two of
// these against a docker-hosted ninjamsrv on localhost.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NJClient;

namespace videosync {

struct VideoFrameRecord {
  std::string username;
  int chidx = 0;
  unsigned int fourcc = 0;
  int frameIndex = 0;
  int totalFrames = 0;
  std::vector<uint8_t> data;
  // Receiver's m_sync_interval_cnt at the moment of delivery (snapshot via callback).
  int receivedAtSwap = -1;
  // steady_clock micro-timestamp captured inside VideoFrameReady_Callback. Combined
  // with the sender's stamped sendTimestampUs (in FakeFrame payload) this gives
  // per-frame send→deliver latency.
  uint64_t deliverTimestampUs = 0;
};

struct RawDataRecord {
  int eventType = 0; // 0=begin, 1=data, 2=end
  unsigned char guid[16] = {};
  unsigned int fourcc = 0;
  std::string username;
  int chidx = 0;
  std::vector<uint8_t> data;
};

class TestClient {
public:
  struct Options {
    std::string host;          // "127.0.0.1"
    int port = 0;              // host port mapped to container's 2049
    std::string user = "anonymous:test";
    std::string pass = "";
    bool sendAudio = true;     // claim ch0, broadcast silence
    bool sendVideo = false;    // claim ch1, fourcc=H264, flags=0x10
    int videoChannel = 1;
    int audioBitrate = 64;
    // Audio thread pacing — number of samples per AudioProc call. Default = 480 (10ms@48k).
    int audioBlockSize = 480;
    int sampleRate = 48000;
  };

  explicit TestClient(Options opts);
  ~TestClient();

  // Connects, waits up to `timeout` for NJC_STATUS_OK, then registers local channels.
  // Returns true on success.
  bool connectAndJoin(std::chrono::milliseconds timeout =
                          std::chrono::seconds(5));

  void disconnect();

  // Wraps `data` in [4B BE length][bytes] and pushes via QueueVideoFrame.
  // Safe from any thread; queue is mutex-protected internally by NJClient.
  void sendVideoFrame(const void *data, int len);

  // Pauses video transmission. Mirrors what the iOS app does on a "stop camera"
  // event: StopVideoChannel sets m_video_active=false; on_new_interval() flushes
  // an END at the next interval boundary and from then on emits no BEGIN markers
  // until resumeVideo() re-activates.
  void pauseVideo();

  // Re-enables video transmission. SetVideoChannel reapplies the cached SPS/PPS
  // and from the next interval boundary the sender starts emitting BEGIN markers
  // again. Use after a pauseVideo() to simulate a mid-session toggle.
  void resumeVideo();

  // Caches synthetic SPS/PPS (so on_new_interval emits a non-empty parameter chunk).
  // Format the receiver expects: [2B SPS len BE][SPS bytes][2B PPS len BE][PPS bytes].
  void sendFakeSPSPPS();

  // Drains and returns recorded video frames from VideoFrameReady_Callback.
  std::vector<VideoFrameRecord> drainVideoFrames();

  // Drains raw-data events (BEGIN/DATA/END) — useful when we don't register a video
  // ready callback and want to inspect the raw stream.
  std::vector<RawDataRecord> drainRawData();

  // Sends a NINJAM chat message. `type` is typically "MSG" for public chat.
  // Use to send BPM/BPI votes: sendChatMessage("MSG", "!vote bpm 120").
  void sendChatMessage(const char *type, const char *msg);

  // Current BPM/BPI as reported by the server (via updateBPMinfo).
  int getBPM() const;
  int getBPI() const;

  // Snapshot of current m_sync_interval_cnt (read via reflection-ish: we expose it
  // through a tiny accessor we add to the wrapper since NJClient field is protected).
  int currentSwap() const { return currentSwap_.load(); }

  // Counts of callback firings, useful for sanity checks.
  int intervalSwapCount() const { return swapCallbackCount_.load(); }

  NJClient *raw() { return client_; }
  const std::string &username() const { return opts_.user; }

private:
  static void rawDataCb(void *userData, int eventType,
                        const unsigned char *guid, unsigned int fourcc,
                        const char *username, int chidx,
                        const void *data, int dataLen);
  static void videoReadyCb(void *userData, const char *username, int chidx,
                           unsigned int fourcc, int frameIndex, int totalFrames,
                           const void *data, int dataLen);
  static void intervalSwapCb(void *userData);

  void runThreadFn();
  void audioThreadFn();

  Options opts_;
  NJClient *client_ = nullptr;
  std::thread runThread_;
  std::thread audioThread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> joined_{false};
  std::atomic<bool> videoChannelRegistered_{false};
  std::atomic<int> currentSwap_{0};
  std::atomic<int> swapCallbackCount_{0};

  mutable std::mutex framesMu_;
  std::deque<VideoFrameRecord> frames_;

  mutable std::mutex rawMu_;
  std::deque<RawDataRecord> rawEvents_;
};

} // namespace videosync
