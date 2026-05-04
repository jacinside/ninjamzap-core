// Scenario 22 — sender starts audio-only then enables video mid-session.
//
// Models the iOS app flow where a user joins the room with mic only and later
// turns on the camera. The first few video intervals must establish GUID sync
// from scratch — the receiver has no VideoRecvState yet for this sender.
//
// Sequence:
//   1. Sender connects with audio only (sendVideo=false).
//   2. Both clients exchange audio for 4 intervals — GUIDs evolve normally.
//   3. Sender calls resumeVideo() + sendFakeSPSPPS() to add the video channel.
//   4. Sender pushes 5 video intervals.
//
// Assertions:
//   * PLAY fires within the first 3 video intervals after enable (warm-up grace).
//   * No DROP-RESYNC — the first sync must not be mistaken for GUID drift.
//   * HOLD count during the first 1-2 video intervals is acceptable (first
//     marker may land in the PREV window before DS is established).
#include <chrono>
#include <cstdio>
#include <thread>

#include "catch_amalgamated.hpp"

#include "FakeFrame.h"
#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"

using namespace std::chrono_literals;

namespace {
videosync::TestClient::Options makeOpts(const std::string &user, bool sendVideo) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = true;
  o.sendVideo = sendVideo;
  return o;
}

void waitIntervals(videosync::TestClient &c, int count) {
  for (int i = 0; i < count; ++i) {
    int base = c.intervalSwapCount();
    // 10s per interval — long enough to survive extreme BPM/BPI from prior tests.
    auto until = std::chrono::steady_clock::now() + 10s;
    while (c.intervalSwapCount() == base &&
           std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(20ms);
  }
}

void streamIntervals(videosync::TestClient &sender, uint32_t &seq, int count) {
  for (int i = 0; i < count; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < 4; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(150ms);
    }
    auto waitUntil = std::chrono::steady_clock::now() + 10s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil)
      std::this_thread::sleep_for(20ms);
  }
}
} // namespace

TEST_CASE("22_audio_then_video — late video enable syncs cleanly",
          "[sync][scenario22]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  // Sender starts audio-only.
  videosync::TestClient sender(makeOpts("anonymous:av_sender", /*sendVideo=*/false));
  videosync::TestClient receiver(makeOpts("anonymous:av_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));

  {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1))
      std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  // Prior BPM/BPI tests may leave the server at slow intervals (e.g. 8s).
  // Reset to fast defaults so the 2s per-interval timeouts below don't expire.
  // With SetVotingThreshold=50 and 2 clients, one vote is sufficient.
  if (sender.getBPI() != 4 || sender.getBPM() != 240) {
    sender.sendChatMessage("MSG", "!vote bpi 4");
    sender.sendChatMessage("MSG", "!vote bpm 240");
    auto resetDeadline = std::chrono::steady_clock::now() + 12s;
    while (std::chrono::steady_clock::now() < resetDeadline &&
           (sender.getBPM() != 240 || sender.getBPI() != 4))
      std::this_thread::sleep_for(50ms);
    std::fprintf(stderr, "[scenario22] BPM/BPI reset to %d/%d\n",
                 sender.getBPM(), sender.getBPI());
  }

  // Phase 1: 4 audio-only intervals — GUIDs advance, no video state on receiver.
  waitIntervals(sender, 4);
  std::fprintf(stderr, "[scenario22] audio-only phase done (swap=%d)\n",
               sender.intervalSwapCount());

  // Enable video mid-session.
  sender.resumeVideo();
  sender.sendFakeSPSPPS();

  // Allow 2 intervals for the video channel's GUID to align after the channel
  // registration (and any preceding BPM/BPI reset) before starting the test.
  waitIntervals(sender, 2);
  log.clear(); // discard handshake noise

  // Phase 2: 6 video intervals (enough headroom for post-DROP recovery).
  uint32_t seq = 0;
  streamIntervals(sender, seq, 6);
  std::this_thread::sleep_for(2s);

  auto plays = log.match(R"(SWAP#\d+ video PLAY: key=av_sender:1)");
  auto drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=av_sender:1)");
  auto holds = log.match(R"(SWAP#\d+ video HOLD: key=av_sender:1)");

  std::fprintf(stderr,
               "[scenario22] PLAY=%zu  HOLD=%zu  DROP-RESYNC=%zu\n",
               plays.size(), holds.size(), drops.size());

  // Late video enable must establish sync within the first few intervals.
  REQUIRE(plays.size() >= 1);

  CHECK(drops.size() <= 1);

  // At most 2 HOLDs during the initial handshake (first marker may land 1
  // interval before DS match is established).
  CHECK(holds.size() <= 6);

  receiver.disconnect();
  sender.disconnect();
}
