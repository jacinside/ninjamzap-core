// Scenario 40 — a sender whose intervals begin just before ours doesn't break up.
//
// Each client starts its interval clock when it connects, so the phase between a
// sender and a receiver is random per connection and stays until one reconnects.
// A remote interval starts playing at the receiver's next swap even while it is
// still streaming in. When the sender's intervals begin just before the receiver's
// boundaries, every interval plays only a fraction of a second ahead of its data:
// any network hiccup longer than that is a gap (silence, the late audio skipped) —
// every interval, for the whole session. Seen live: one musician 0.3 s from the
// boundary broke up for a whole session, the others (1.3 s and more) were clean,
// and reconnecting fixed it or moved it to someone else.
//
// Setup: shift the sender's clock so its intervals start ~0.25 s before the
// receiver's. A download that began too recently waits one more swap (LATE-START)
// and then plays a whole interval ahead of the stream.
// The harness runs with the guard off (TestClient::Options::lateStartGuardMs); the
// setup places the sender with it off, then turns on the app's default.
//
// 40a: the sender's network stalls 1.5 s in the middle of every interval; the
//      receiver must not starve.
// 40b: the sender sends video too; once its audio moved one interval later, every
//      frame must be shown with the audio of the interval it was captured in.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "catch_amalgamated.hpp"

#include "FakeFrame.h"
#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"
#include "ToneTag.h"
#include "njclient.h"

using namespace std::chrono_literals;

namespace {
constexpr int kSrate = 48000;

// The app's default (NJClient::config_late_start_guard_ms), turned on once the setup has
// placed the sender. NJ_TEST_LATE_START_GUARD_MS=0 shows the problem without it.
const double gGuardMs = std::getenv("NJ_TEST_LATE_START_GUARD_MS") ? std::atof(std::getenv("NJ_TEST_LATE_START_GUARD_MS")) : 1500.0;

videosync::TestClient::Options makeOpts(const std::string &user, bool send, bool video = false) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = send;
  o.toneInput = send; // real content: silence encodes to almost nothing
  o.audioBitrate = 128;
  o.sendVideo = send && video;
  o.recordOutput = !send && video;
  return o;
}

// Receiver position (samples into its interval) when the sender's next interval
// becomes playable on it (audio END: prebuffer reached).
int arrivalPos(videosync::TestClient &recv, const std::string &who, int &len) {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (log.waitForLine("audio END: user=" + who + " ch=0", 20s).empty()) return -1;
  int pos = 0;
  recv.raw()->GetPosition(&pos, &len);
  return pos;
}

// Sender votes realistic intervals (120 BPM / 16 BPI = 8 s), the receiver joins, and the
// sender's clock is shifted until its intervals become playable on the receiver
// ~0.25 s before the receiver's boundary, with one interval queued as in a live room.
// The server hands a joining client the interval in progress too, which can leave it an
// extra interval queued — a margin a live room doesn't give; a sender silent one more
// interval drains it. Returns the interval length in samples, or 0.
int placeSenderNearBoundary(videosync::TestClient &sender, videosync::TestClient &receiver,
                            const std::string &who, const char *tag) {
  auto &log = videosync::SyncLogCapture::instance();
  if (!sender.connectAndJoin(8s)) return 0;
  sender.sendChatMessage("MSG", "!vote bpm 120");
  sender.sendChatMessage("MSG", "!vote bpi 16");
  auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.getBPM() != 120 || sender.getBPI() != 16))
    std::this_thread::sleep_for(50ms);
  if (sender.getBPM() != 120 || sender.getBPI() != 16) return 0;
  std::this_thread::sleep_for(2s);

  if (!receiver.connectAndJoin(8s)) return 0;
  // A download starts playing once its first 8 KB arrived: ~0.5 s of real music at
  // 128 kbps, but the test tone compresses far better and takes seconds, which hides
  // the problem behind a margin no live session has. Match real content.
  receiver.raw()->config_play_prebuffer = 2048;
  const int swaps0 = receiver.intervalSwapCount();
  deadline = std::chrono::steady_clock::now() + 25s;
  while (std::chrono::steady_clock::now() < deadline && receiver.intervalSwapCount() < swaps0 + 2)
    std::this_thread::sleep_for(50ms);

  int len = 0;
  int pos = arrivalPos(receiver, who, len);
  if (pos < 0 || len <= 0) return 0;
  const int target = len - kSrate / 4;
  for (int attempt = 0; attempt < 4; attempt++) {
    const bool near = len - pos <= kSrate * 6 / 10 && pos <= target + kSrate / 10;
    const int shift = near ? len : ((target - pos) % len + len) % len + len;
    sender.stallAudio(shift * 1000 / kSrate);
    std::this_thread::sleep_for(std::chrono::milliseconds(shift * 1000 / kSrate) + 10s);
    pos = arrivalPos(receiver, who, len);
    log.clear();
    const auto swap = log.waitForLine("SWAP#\\d+ audio: user=" + who + " ch=0 ds=Y", 12s);
    const bool oneQueued = swap.find("nds0=N") != std::string::npos;
    std::fprintf(stderr, "[%s] interval %.2f s, sender playable at %.2f s (%.2f s before our boundary), %s\n",
                 tag, len / (double)kSrate, pos / (double)kSrate, (len - pos) / (double)kSrate,
                 oneQueued ? "one interval queued" : "extra interval queued");
    if (pos >= 0 && len - pos <= kSrate * 6 / 10 && oneQueued) return len;
  }
  return 0;
}
} // namespace

TEST_CASE("40a_late_start_margin — a sender starting just before our boundary doesn't break up",
          "[sync][scenario40]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient sender(makeOpts("anonymous:ls_sender", true));
  videosync::TestClient receiver(makeOpts("anonymous:ls_recv", false));
  // The setup must put the sender close to our boundary, or this tests nothing.
  REQUIRE(placeSenderNearBoundary(sender, receiver, "ls_sender", "scenario40a") > 0);
  log.clear();
  receiver.raw()->config_late_start_guard_ms = gGuardMs;
  // The guard acts at our next swap (in the app it is on from the start).
  const bool late = !log.waitForLine(R"(audio LATE-START: user=ls_sender)", 12s).empty();

  // Four intervals with a 1.5 s network stall on the sender in the middle of each.
  for (int i = 0; i < 4; i++) {
    auto deadline = std::chrono::steady_clock::now() + 10s;
    int spos = 0, slen = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      sender.raw()->GetPosition(&spos, &slen);
      if (slen > 0 && spos >= slen * 35 / 100 && spos < slen * 45 / 100) break;
      std::this_thread::sleep_for(10ms);
    }
    sender.stallNetwork(1500);
    std::this_thread::sleep_for(6s);
  }
  std::this_thread::sleep_for(10s); // the last stalled interval plays

  const auto starves = log.match(R"(audio STARVE begin: user=ls_sender)");
  const auto plays = log.match(R"(SWAP#\d+ audio: user=ls_sender ch=0 ds=Y)");
  std::fprintf(stderr, "[scenario40a] LATE-START=%d STARVE=%zu swaps playing=%zu\n",
               late ? 1 : 0, starves.size(), plays.size());
  for (const auto &l : starves) std::fprintf(stderr, "  %s\n", l.c_str());

  REQUIRE(plays.size() >= 4); // the sender's audio kept playing
  CHECK(starves.empty());
}

TEST_CASE("40b_late_start_video — video follows the audio it moved one interval later",
          "[sync][scenario40]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient sender(makeOpts("anonymous:lv_sender", true, true));
  videosync::TestClient receiver(makeOpts("anonymous:lv_recv", false, true));
  const int len = placeSenderNearBoundary(sender, receiver, "lv_sender", "scenario40b");
  REQUIRE(len > 0);
  sender.sendFakeSPSPPS();
  log.clear();
  receiver.raw()->config_late_start_guard_ms = gGuardMs;
  // The guard acts at our next swap (in the app it is on from the start); video starts
  // once the sender's audio plays one interval later.
  const bool late = !log.waitForLine(R"(audio LATE-START: user=lv_sender)", 12s).empty();
  // That swap plays nothing of the sender: no audio, its video interval holds.
  std::this_thread::sleep_for(500ms);
  receiver.drainVideoFrames();
  log.clear();

  // Video at ~12 fps for 5 intervals, each frame tagged with the sender interval it is
  // captured in; then two more intervals for the last ones to play.
  const int startTag = sender.currentSwap();
  uint32_t seq = 0;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(5 * len / kSrate * 1000);
  int lastTag = startTag;
  while (std::chrono::steady_clock::now() < end) {
    lastTag = sender.currentSwap();
    auto f = videosync::makeFakeFrame(seq++, (uint32_t)lastTag, 256);
    sender.sendVideoFrame(f.data(), (int)f.size());
    std::this_thread::sleep_for(80ms);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2 * len / kSrate * 1000));

  const auto drops = log.match(R"(SWAP#\d+ video (DROP-RESYNC|HOLD).*key=lv_sender)");
  // The first interval may start mid-way; the last one may still be in flight.
  const int firstTag = startTag + 1, checkTo = lastTag - 1;
  std::vector<float> L, R;
  receiver.outputSnapshot(L, R);
  const int kWin = 2048;
  int checked = 0, mismatched = 0, silent = 0, printed = 0;
  for (const auto &f : receiver.drainVideoFrames()) {
    videosync::FakeFramePayload p;
    if (!videosync::parseFakeFrame(f.data.data(), (int)f.data.size(), p)) continue;
    const int tag = (int)p.intervalIdx;
    if (tag < firstTag || tag > checkTo || f.outputSample < 0 ||
        f.outputSample + kWin > (int64_t)L.size()) continue;
    const int a = videosync::detectToneTag(L.data() + f.outputSample, kWin, kSrate);
    ++checked;
    if (a < 0) { ++silent; continue; }
    if (a != tag % videosync::kToneTags) {
      ++mismatched;
      if (printed++ < 12)
        std::fprintf(stderr, "[scenario40b]   CONTENT seq=%u videoInterval=%d audio=%d recvSwap=%d\n",
                     p.seq, tag, a, f.receivedAtSwap);
    }
  }
  std::fprintf(stderr, "[scenario40b] LATE-START=%d HOLD/DROP=%zu video intervals %d..%d: frames=%d mismatched=%d silent=%d\n",
               late ? 1 : 0, drops.size(), firstTag, checkTo, checked, mismatched, silent);
  for (const auto &l : drops) std::fprintf(stderr, "  %s\n", l.c_str());

  REQUIRE(late); // the sender's audio moved one interval later
  CHECK(checked >= 150);   // ~3 whole intervals of frames were shown
  CHECK(mismatched == 0);
  CHECK(silent == 0);
  CHECK(drops.empty());
}
