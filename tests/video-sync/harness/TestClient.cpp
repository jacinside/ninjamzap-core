#include "TestClient.h"

#include <algorithm>
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <vector>

#include "njclient.h"
#include "FakeFrame.h" // for nowMicros()
#include "ToneTag.h"
#include <cmath>

namespace videosync {

namespace {

void writeBE32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24) & 0xff);
  p[1] = (uint8_t)((v >> 16) & 0xff);
  p[2] = (uint8_t)((v >> 8) & 0xff);
  p[3] = (uint8_t)(v & 0xff);
}

void writeBE16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)((v >> 8) & 0xff);
  p[1] = (uint8_t)(v & 0xff);
}

} // namespace

TestClient::TestClient(Options opts) : opts_(std::move(opts)) {
  client_ = new NJClient();
  client_->config_savelocalaudio = 0;
  client_->config_autosubscribe = 1;
  client_->config_metronome_mute = true;
  client_->config_late_start_guard_ms = opts_.lateStartGuardMs;
  // Optional verbose RECV BLOCK printouts on stderr — surfaces every WRITE arrival.
  if (std::getenv("NJ_TEST_DEBUG")) client_->config_debug_level = 2;
  client_->RawData_Callback = &TestClient::rawDataCb;
  client_->RawData_User = this;
  client_->VideoFrameReady_Callback = &TestClient::videoReadyCb;
  client_->VideoFrameReady_User = this;
  client_->IntervalSwap_Callback = &TestClient::intervalSwapCb;
  client_->IntervalSwap_User = this;
  // Auto-accept the server license — host integration tests are headless.
  client_->LicenseAgreementCallback = [](void *, const char *) { return 1; };
  client_->LicenseAgreement_User = nullptr;
}

TestClient::~TestClient() {
  disconnect();
  delete client_;
  client_ = nullptr;
}

bool TestClient::connectAndJoin(std::chrono::milliseconds timeout) {
  if (opts_.port <= 0) return false;

  // host:port string
  char hostbuf[256];
  std::snprintf(hostbuf, sizeof(hostbuf), "%s:%d", opts_.host.c_str(), opts_.port);
  std::vector<char> userBuf(opts_.user.begin(), opts_.user.end());
  userBuf.push_back('\0');
  std::vector<char> passBuf(opts_.pass.begin(), opts_.pass.end());
  passBuf.push_back('\0');

  client_->Connect(hostbuf, userBuf.data(), passBuf.data());

  stop_.store(false);
  runThread_ = std::thread(&TestClient::runThreadFn, this);
  audioThread_ = std::thread(&TestClient::audioThreadFn, this);

  // Wait for OK status.
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int s = client_->GetStatus();
    if (s == NJClient::NJC_STATUS_OK) {
      // Configure local channels now that the connection is established.
      if (opts_.sendAudio) {
        client_->SetLocalChannelInfo(0, "audio",
                                     /*setsrcch*/ true, opts_.ch0Stereo ? 1024 : 0,
                                     /*setbitrate*/ true, opts_.audioBitrate,
                                     /*setbcast*/ true, true);
      }
      for (int c = 0; c < opts_.toneChannels; ++c) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "audio%d", c + 1);
        client_->SetLocalChannelInfo(c, nm,
                                     /*setsrcch*/ true, c,
                                     /*setbitrate*/ true, opts_.audioBitrate,
                                     /*setbcast*/ true, true);
      }
      if (opts_.extraAudioChannel >= 0) {
        client_->SetLocalChannelInfo(opts_.extraAudioChannel, "audio2",
                                     /*setsrcch*/ true, 1,
                                     /*setbitrate*/ true, opts_.audioBitrate,
                                     /*setbcast*/ true, true);
      }
      if (opts_.sendVideo) {
        client_->SetLocalChannelInfo(opts_.videoChannel, "video",
                                     /*setsrcch*/ false, 0,
                                     /*setbitrate*/ false, 0,
                                     /*setbcast*/ true, true,
                                     /*setoutch*/ false, 0,
                                     /*setflags*/ true, 0x10);
        client_->SetVideoChannel(opts_.videoChannel,
                                 (unsigned int)('H') | ((unsigned int)'2' << 8) |
                                     ((unsigned int)'6' << 16) | ((unsigned int)'4' << 24));
        videoChannelRegistered_.store(true);
      }
      client_->NotifyServerOfChannelChange();
      joined_.store(true);
      return true;
    }
    if (s == NJClient::NJC_STATUS_CANTCONNECT ||
        s == NJClient::NJC_STATUS_INVALIDAUTH ||
        s == NJClient::NJC_STATUS_DISCONNECTED) {
      std::fprintf(stderr, "[TestClient %s] connect failed status=%d err=%s\n",
                   opts_.user.c_str(), s, client_->GetErrorStr());
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::fprintf(stderr, "[TestClient %s] connect timeout, last status=%d\n",
               opts_.user.c_str(), client_->GetStatus());
  return false;
}

void TestClient::disconnect() {
  if (stop_.exchange(true)) return;
  if (client_ && opts_.sendVideo) client_->StopVideoChannel();
  if (audioThread_.joinable()) audioThread_.join();
  if (runThread_.joinable()) runThread_.join();
  if (client_) client_->Disconnect();
  joined_.store(false);
}

void TestClient::sendVideoFrame(const void *data, int len) {
  if (!client_ || len <= 0) return;
  // Wire format expected by njclient: [4B BE total length][payload].
  std::vector<uint8_t> chunk(4 + len);
  writeBE32(chunk.data(), (uint32_t)len);
  std::memcpy(chunk.data() + 4, data, len);
  client_->QueueVideoFrame(chunk.data(), (int)chunk.size());
}

void TestClient::pauseVideo() {
  if (!client_) return;
  client_->StopVideoChannel();
}

void TestClient::resumeVideo() {
  if (!client_) return;
  if (!videoChannelRegistered_.exchange(true)) {
    // First time enabling video — register the channel with the server.
    client_->SetLocalChannelInfo(opts_.videoChannel, "video",
                                 /*setsrcch*/ false, 0,
                                 /*setbitrate*/ false, 0,
                                 /*setbcast*/ true, true,
                                 /*setoutch*/ false, 0,
                                 /*setflags*/ true, 0x10);
    client_->SetVideoChannel(opts_.videoChannel,
                             (unsigned int)('H') | ((unsigned int)'2' << 8) |
                                 ((unsigned int)'6' << 16) | ((unsigned int)'4' << 24));
    client_->NotifyServerOfChannelChange();
  } else {
    // Channel already registered — just re-enable it without resetting GUID.
    client_->SetVideoChannel(opts_.videoChannel,
                             (unsigned int)('H') | ((unsigned int)'2' << 8) |
                                 ((unsigned int)'6' << 16) | ((unsigned int)'4' << 24));
  }
}

void TestClient::notifyChannels() {
  if (client_) client_->NotifyServerOfChannelChange();
}

void TestClient::setVoiceChat(int ch, bool on, bool notify) {
  if (!client_) return;
  client_->SetLocalChannelInfo(ch, NULL,
                               /*setsrcch*/ false, 0,
                               /*setbitrate*/ false, 0,
                               /*setbcast*/ false, false,
                               /*setoutch*/ false, 0,
                               /*setflags*/ true, on ? 2 : 0);
  if (notify) client_->NotifyServerOfChannelChange();
}

void TestClient::outputSnapshot(std::vector<float> &l, std::vector<float> &r) {
  std::lock_guard<std::mutex> lk(outMu_);
  l = recL_;
  r = recR_;
}

static int findRemoteUser(NJClient *client, const std::string &user) {
  for (int i = 0; i < client->GetNumUsers(); ++i) {
    const char *n = client->GetUserState(i);
    if (!n) continue;
    std::string s(n);
    const size_t at = s.find('@');
    if (at != std::string::npos) s.resize(at);
    if (s == user || (s.size() > user.size() &&
                      s.compare(s.size() - user.size(), user.size(), user) == 0))
      return i;
  }
  return -1;
}

bool TestClient::subscribeRemoteChannel(const std::string &user, int ch, bool on) {
  if (!client_) return false;
  const int i = findRemoteUser(client_, user);
  if (i < 0) return false;
  client_->SetUserChannelState(i, ch, true, on, false, 0.f, false, 0.f, false, false, false, false);
  return true;
}

void TestClient::requestIntervalHold(const std::string &user, int ch, int swaps) {
#ifdef NINJAMCORE_TEST_HOOKS
  if (client_) client_->TestRequestIntervalHold(user.c_str(), ch, swaps);
#endif
}

bool TestClient::panRemoteChannel(const std::string &user, int ch, float pan) {
  if (!client_) return false;
  for (int i = 0; i < client_->GetNumUsers(); ++i) {
    const char *n = client_->GetUserState(i);
    if (!n) continue;
    std::string s(n);
    const size_t at = s.find('@');
    if (at != std::string::npos) s.resize(at);
    if (s == user || (s.size() > user.size() && s.compare(s.size() - user.size(), user.size(), user) == 0)) {
      client_->SetUserChannelState(i, ch, false, false, false, 0.f, true, pan, false, false, false, false);
      return true;
    }
  }
  return false;
}

void TestClient::sendFakeSPSPPS() {
  // Fake SPS/PPS: just synthetic bytes so receiver has something to cache. Size doesn't matter
  // for sync tests — it never reaches a real H.264 decoder on the host.
  // The cached buffer must already include the 4-byte BE length prefix because
  // on_new_interval() forwards it as a single WRITE and the receiver's frame
  // reassembler reads the first 4 bytes as a length. (iOS wraps it the same way
  // in VideoCaptureManager.swift::appendLengthPrefixed.)
  uint8_t inner[2 + 12 + 2 + 6];
  uint8_t *p = inner;
  writeBE16(p, 12); p += 2;
  uint8_t sps[12] = {0x67, 0x42, 0xc0, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  std::memcpy(p, sps, 12); p += 12;
  writeBE16(p, 6); p += 2;
  uint8_t pps[6] = {0x68, 0xce, 0x06, 0xe2, 0x00, 0x00};
  std::memcpy(p, pps, 6); p += 6;

  uint8_t wire[4 + sizeof(inner)];
  writeBE32(wire, (uint32_t)sizeof(inner));
  std::memcpy(wire + 4, inner, sizeof(inner));
  client_->SetVideoSPSPPS(wire, (int)sizeof(wire));
}

std::vector<VideoFrameRecord> TestClient::drainVideoFrames() {
  std::lock_guard<std::mutex> lk(framesMu_);
  std::vector<VideoFrameRecord> out(frames_.begin(), frames_.end());
  frames_.clear();
  return out;
}

void TestClient::sendChatMessage(const char *type, const char *msg) {
  if (client_) client_->ChatMessage_Send(type, msg);
}

int TestClient::getBPM() const {
  return client_ ? (int)client_->GetActualBPM() : 0;
}

int TestClient::getBPI() const {
  return client_ ? client_->GetBPI() : 0;
}

std::vector<RawDataRecord> TestClient::drainRawData() {
  std::lock_guard<std::mutex> lk(rawMu_);
  std::vector<RawDataRecord> out(rawEvents_.begin(), rawEvents_.end());
  rawEvents_.clear();
  return out;
}

void TestClient::rawDataCb(void *userData, int eventType,
                           const unsigned char *guid, unsigned int fourcc,
                           const char *username, int chidx,
                           const void *data, int dataLen) {
  auto *self = static_cast<TestClient *>(userData);
  RawDataRecord rec;
  rec.eventType = eventType;
  if (guid) std::memcpy(rec.guid, guid, 16);
  rec.fourcc = fourcc;
  rec.username = username ? username : "";
  rec.chidx = chidx;
  if (data && dataLen > 0) {
    rec.data.assign(static_cast<const uint8_t *>(data),
                    static_cast<const uint8_t *>(data) + dataLen);
  }
  std::lock_guard<std::mutex> lk(self->rawMu_);
  self->rawEvents_.emplace_back(std::move(rec));
}

void TestClient::videoReadyCb(void *userData, const char *username, int chidx,
                              unsigned int fourcc, int frameIndex, int totalFrames,
                              const void *data, int dataLen) {
  auto *self = static_cast<TestClient *>(userData);
  VideoFrameRecord rec;
  rec.username = username ? username : "";
  rec.chidx = chidx;
  rec.fourcc = fourcc;
  rec.frameIndex = frameIndex;
  rec.totalFrames = totalFrames;
  if (data && dataLen > 0) {
    rec.data.assign(static_cast<const uint8_t *>(data),
                    static_cast<const uint8_t *>(data) + dataLen);
  }
  rec.receivedAtSwap = self->currentSwap_.load();
  rec.deliverTimestampUs = videosync::nowMicros();
  rec.outputSample = self->blockEndSample_.load();
  std::lock_guard<std::mutex> lk(self->framesMu_);
  self->frames_.emplace_back(std::move(rec));
}

void TestClient::intervalSwapCb(void *userData) {
  auto *self = static_cast<TestClient *>(userData);
  self->swapCallbackCount_.fetch_add(1);
  // Track sync interval count by counting callbacks. We can't read m_sync_interval_cnt
  // directly without exposing it, but the callback fires exactly once per swap.
  self->currentSwap_.fetch_add(1);
}

void TestClient::runThreadFn() {
  while (!stop_.load()) {
    int ns = netStallMs_.exchange(0);
    if (ns > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ns));
    int sleepy = client_->Run();
    if (sleepy) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void TestClient::audioThreadFn() {
  const int block = opts_.audioBlockSize;
  const int srate = opts_.sampleRate;
  std::vector<float> silenceL(block, 0.0f);
  std::vector<float> silenceR(block, 0.0f);
  std::vector<float> outL(block, 0.0f);
  std::vector<float> outR(block, 0.0f);
  float *inbuf[kMaxToneChannels] = {silenceL.data(), silenceR.data()};
  const int nIn = std::max(2, opts_.toneChannels);
  std::vector<std::vector<float>> chIn(nIn, std::vector<float>(block, 0.0f));
  if (opts_.toneChannels > 0)
    for (int c = 0; c < nIn; ++c) inbuf[c] = chIn[c].data();
  float *outbuf[2] = {outL.data(), outR.data()};

  // Realtime pacing: sleep for `block/srate` between calls so on_new_interval()
  // fires approximately once per (BPI * 60 / BPM) seconds — same as production.
  auto period = std::chrono::microseconds((int)((double)block * 1e6 / (double)srate));
  auto next = std::chrono::steady_clock::now();
  // NJ_TEST_STALL_CATCHUP=1: never drop late time; run blocks back-to-back until
  // caught up, like the bot's wall-clock pump (RoomBot::RunAudioPump).
  const char *cu = std::getenv("NJ_TEST_STALL_CATCHUP");
  const bool catchup = cu && *cu == '1';
  while (!stop_.load()) {
    int stall = stallMs_.exchange(0);
    if (stall > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(stall));
      // Default: the stalled time is lost (a device whose clock slipped).
      // NJ_TEST_STALL_CATCHUP=1: make it up with back-to-back blocks, like the
      // bot's wall-clock pump after a CPU stall.
      if (!catchup) next = std::chrono::steady_clock::now();
    }
    if (joined_.load()) {
      if (opts_.toneChannels > 0) {
        int pos = 0, len = 0;
        client_->GetPosition(&pos, &len);
        const int boundary = (len > 0 && pos >= 0 && len - pos < block) ? len - pos : block;
        const int tag = currentSwap_.load();
        const uint32_t silent = silentMask_.load(std::memory_order_relaxed);
        for (int c = 0; c < opts_.toneChannels; ++c) {
          const float g = (silent & (1u << c)) ? 0.f : 0.08f * chGain_[c].load(std::memory_order_relaxed);
          const float gate = chGateFrom_[c].load(std::memory_order_relaxed);
          for (int i = 0; i < block; ++i) {
            const int t = i < boundary ? tag : tag + 1;
            const int at = i < boundary ? pos + i : i - boundary; // sample within its interval
            const bool gated = gate > 0.f && len > 0 && at < gate * len;
            chPhase_[c] += 2.0 * M_PI * toneFreqFor(c, t) / srate;
            if (chPhase_[c] > 2.0 * M_PI) chPhase_[c] -= 2.0 * M_PI;
            chIn[c][i] = gated ? 0.f : g * (float)std::sin(chPhase_[c]);
          }
        }
      } else if (opts_.toneInput) {
        // Tag each input sample with the interval it's recorded in; the interval
        // boundary inside this block (if any) is where AudioProc will swap.
        int pos = 0, len = 0;
        client_->GetPosition(&pos, &len);
        const int boundary = (len > 0 && pos >= 0 && len - pos < block) ? len - pos : block;
        const int tag = currentSwap_.load();
        for (int i = 0; i < block; ++i) {
          const int t = i < boundary ? tag : tag + 1;
          const double w = 2.0 * M_PI * videosync::toneFreqForTag(t) / srate;
          tonePhase_ += w;
          if (tonePhase_ > 2.0 * M_PI) tonePhase_ -= 2.0 * M_PI;
          silenceR[i] = (float)(0.3 * std::sin(tonePhase_));
          silenceL[i] = opts_.silentCh0 ? 0.f : silenceR[i] * opts_.ch0Gain;
        }
      }
      // frames delivered inside this AudioProc are shown at the end of this block
      if (opts_.recordOutput) blockEndSample_.store(outCount_ + block);
      client_->AudioProc(inbuf, opts_.toneChannels > 0 ? nIn : 2, outbuf, 2, block, srate,
                         /*justmonitor*/ false, /*isPlaying*/ true,
                         /*isSeek*/ false, /*cursessionpos*/ -1.0);
      if (opts_.recordOutput) {
        outCount_ += block;
        std::lock_guard<std::mutex> lk(outMu_);
        recL_.insert(recL_.end(), outL.begin(), outL.end());
        recR_.insert(recR_.end(), outR.begin(), outR.end());
      }
    }
    next += period;
    auto now = std::chrono::steady_clock::now();
    if (next > now) std::this_thread::sleep_until(next);
    else if (!catchup) next = now; // catch-up on slow steps without piling up
  }
}

} // namespace videosync
