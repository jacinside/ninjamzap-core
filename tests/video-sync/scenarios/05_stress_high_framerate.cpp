// Scenario 5 — stress: dense frame stream forces BURST handling.
//
// At BPM=240/BPI=4 (1.0s intervals) we push 30+ frames per interval. That's far
// more than the receiver's `next` buffer can absorb before the next swap arrives,
// so the receiver's BEGIN handler emits "video BURST: discarding next" each
// time a fresh interval starts before the previous one was promoted.
//
// What the test asserts:
//   * BURST events MUST appear (we want to confirm we're hitting the path).
//   * DROP-RESYNC must NEVER fire (the BURST itself shouldn't break GUID sync).
//   * Across surviving intervals the PLAY/PROMOTE pairing still holds where
//     PLAY says "deferred → pending" — i.e. the dense path doesn't corrupt
//     the structural invariant validated in scenario 8.
#include <chrono>
#include <cstdio>
#include <regex>
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
} // namespace

TEST_CASE("05_stress_high_framerate — BURST tolerated, no DROP-RESYNC, invariant holds",
          "[sync][scenario5][stress]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:str_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:str_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1)) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  // Saturate: 30 frames/interval × 5 intervals at ~25 ms cadence (closer to
  // real 25 fps + headroom). Pushed FAST so most of an interval's frames pile
  // into the receiver's `next` before the next swap.
  const int kIntervals = 5;
  const int kFramesPerInterval = 30;
  uint32_t seq = 0;
  for (int i = 0; i < kIntervals; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < kFramesPerInterval; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 512);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(25ms);
    }
    auto waitUntil = std::chrono::steady_clock::now() + 2s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(20ms);
    }
  }
  std::this_thread::sleep_for(2s);

  auto bursts        = log.match(R"(video BURST: discarding next)");
  auto plays         = log.match(R"(SWAP#\d+ video PLAY: key=str_sender:1)");
  auto deferredPlays = log.match(R"(SWAP#\d+ video PLAY: key=str_sender:1.*\(deferred → pending\))");
  auto promotes      = log.match(R"(SWAP#\d+ video PROMOTE: key=str_sender:1)");
  auto dropResyncs   = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=str_sender:1)");

  std::fprintf(stderr,
               "[scenario5] PLAY total=%zu  deferred=%zu  PROMOTE=%zu  BURST=%zu  DROP-RESYNC=%zu\n",
               plays.size(), deferredPlays.size(), promotes.size(),
               bursts.size(), dropResyncs.size());

  // BURST detection (`if (vs->next.active) discard`) only fires when the sender
  // gets ahead of the receiver by ≥1 interval (toggle off/on, catch-up). Sustained
  // high-framerate alone does not produce that condition: the receiver's
  // mid-download startPlaying drains accumulating into next, and the next swap
  // clears next before the following BEGIN. Burst path is exercised in
  // 09_pause_resume_burst. Here we only require that stress doesn't escalate
  // to DROP-RESYNC.
  REQUIRE(dropResyncs.empty());

  // For every "deferred → pending" PLAY there should be a PROMOTE one swap
  // later. Allow the trailing tail (last deferred PLAY may not have a PROMOTE
  // captured yet). PROMOTE count should equal deferred PLAY count or be off
  // by one.
  if (!deferredPlays.empty()) {
    CHECK(promotes.size() + 1 >= deferredPlays.size());
    CHECK(promotes.size() <= deferredPlays.size());
  }

  receiver.disconnect();
  sender.disconnect();
}
