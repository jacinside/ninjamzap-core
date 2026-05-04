// Scenario 25 — sender streams video without ever calling sendFakeSPSPPS().
//
// In production, the iOS encoder emits SPS/PPS before the first IDR frame.
// NinjamZap caches this via SetVideoSPSPPS() so njclient can prepend it at
// each interval boundary. If the cache is empty, the receiver's first frames
// have no codec header — a real H.264 decoder would reject them, but the
// sync layer must not crash or get stuck.
//
// What's expected:
//   * `next.frameCount` is > 0 (frames arrive) but the interval data has no
//     SPS/PPS prepended.
//   * PLAY events may fire (the sync layer doesn't validate frame content).
//   * No DROP-RESYNC — missing SPS/PPS is not a GUID mismatch.
//   * No crash or deadlock.
//
// After 3 intervals the sender calls sendFakeSPSPPS() and continues.
// Assertions:
//   * PLAY events appear in the post-SPS/PPS phase (recovery confirmed).
//   * No DROP-RESYNC at any point.
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

void streamIntervals(videosync::TestClient &sender, uint32_t &seq, int count) {
  for (int i = 0; i < count; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < 4; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(150ms);
    }
    auto waitUntil = std::chrono::steady_clock::now() + 2s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil)
      std::this_thread::sleep_for(20ms);
  }
}
} // namespace

TEST_CASE("25_no_initial_spspps — missing SPS/PPS does not break sync state",
          "[sync][scenario25]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:ns_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:ns_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  // Intentionally skip sendFakeSPSPPS() here.

  {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           sender.intervalSwapCount() < 1)
      std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  // Phase 1: 3 intervals WITHOUT SPS/PPS.
  uint32_t seq = 0;
  streamIntervals(sender, seq, 3);
  auto phase1Drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=ns_sender:1)");
  std::fprintf(stderr, "[scenario25] phase1 (no SPS/PPS): DROP-RESYNC=%zu\n",
               phase1Drops.size());

  // Phase 2: send SPS/PPS then 4 more intervals.
  log.clear();
  sender.sendFakeSPSPPS();
  streamIntervals(sender, seq, 4);
  std::this_thread::sleep_for(2s);

  auto phase2Plays = log.match(R"(SWAP#\d+ video PLAY: key=ns_sender:1)");
  auto phase2Drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=ns_sender:1)");

  std::fprintf(stderr,
               "[scenario25] phase2 (with SPS/PPS): PLAY=%zu  DROP-RESYNC=%zu\n",
               phase2Plays.size(), phase2Drops.size());

  // The sync state machine must not have been corrupted by missing SPS/PPS.
  // After sendFakeSPSPPS() the receiver must receive PLAY events.
  REQUIRE(phase2Plays.size() >= 1);

  // No DROP-RESYNC in either phase — missing codec header is not a GUID event.
  // Allow at most 1 to tolerate stale server GUID state from prior BPM/BPI tests.
  CHECK(phase1Drops.empty());
  CHECK(phase2Drops.size() <= 1);

  receiver.disconnect();
  sender.disconnect();
}
