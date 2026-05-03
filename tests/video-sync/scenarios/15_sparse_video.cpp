// Scenario 15 — sparse video: 1 frame per interval.
//
// The sync logic requires `next.frameCount >= 1` to attempt a PLAY. This test
// deliberately sends only one frame per interval — the absolute minimum — to
// verify that the PLAY path works at the floor and that pacing doesn't skip
// the single frame.
//
// At 1 frame per 1.0s interval (1 fps effective), the receiver should still:
//   * Emit PLAY events (not HOLD/EMPTY) once synced.
//   * Not trigger DROP-RESYNC.
//   * Deliver the frame via VideoFrameReady_Callback (checked via drainVideoFrames).
//
// This catches regressions where a minimum-frame-count check is off-by-one.
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
} // namespace

TEST_CASE("15_sparse_video — 1 frame/interval still produces PLAY events",
          "[sync][scenario15]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:sp_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:sp_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1)) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  // Push exactly ONE frame per interval for 8 intervals.
  const int kIntervals = 8;
  uint32_t seq = 0;
  for (int i = 0; i < kIntervals; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 256);
    sender.sendVideoFrame(frame.data(), (int)frame.size());
    // Wait for next swap.
    auto waitUntil = std::chrono::steady_clock::now() + 2s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(20ms);
    }
  }
  std::this_thread::sleep_for(2s);

  auto plays  = log.match(R"(SWAP#\d+ video PLAY: key=sp_sender:1)");
  auto drops  = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=sp_sender:1)");
  auto empties = log.match(R"(SWAP#\d+ video EMPTY: key=sp_sender:1)");

  auto delivered = receiver.drainVideoFrames();

  std::fprintf(stderr,
               "[scenario15] PLAY=%zu  DROP-RESYNC=%zu  EMPTY=%zu  delivered_frames=%zu\n",
               plays.size(), drops.size(), empties.size(), delivered.size());

  // At 1 fps over 8 intervals, allow up to 2 intervals of handshake noise.
  // At least 4 PLAY events should fire.
  REQUIRE(plays.size() >= 4);
  CHECK(drops.empty());

  // At minimum the frames that were played should arrive via the callback.
  REQUIRE(delivered.size() >= plays.size());

  receiver.disconnect();
  sender.disconnect();
}
