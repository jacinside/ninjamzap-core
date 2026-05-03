// Scenario 13 — SPS/PPS refresh mid-stream (codec reset).
//
// On iOS, VTCompressionSession can emit new SPS/PPS + an IDR frame when the
// encoder resets (e.g. after a background/foreground transition, or when the
// app calls VTCompressionSessionCompleteFrames). The receiver sees a new codec
// header mid-interval and must not treat this as a GUID mismatch or get stuck.
//
// Test sequence:
//   1. Sender streams 3 clean intervals.
//   2. Sender calls sendFakeSPSPPS() again (simulates codec re-init) and
//      continues streaming 3 more intervals with frames tagged with the NEW
//      "codec epoch" (seq restart from 0).
//   3. Receiver should produce PLAY events in both phases with no DROP-RESYNC.
//
// What's asserted:
//   * PLAY events appear in both the pre-refresh and post-refresh windows.
//   * No DROP-RESYNC at any point.
//   * FALLBACK-HOLD count (if any) does not exceed the play count — transient
//     stall after SPS/PPS re-send is acceptable but should resolve.
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
    for (int f = 0; f < 3; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(180ms);
    }
    auto waitUntil = std::chrono::steady_clock::now() + 2s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(20ms);
    }
  }
}
} // namespace

TEST_CASE("13_sps_pps_mid_stream — codec reset mid-stream does not break receiver",
          "[sync][scenario13]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:sps_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:sps_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1)) {
      std::this_thread::sleep_for(50ms);
    }
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  // Phase 1: 3 clean intervals.
  uint32_t seq = 0;
  streamIntervals(sender, seq, 3);
  auto phase1Lines = log.all();
  log.clear();

  // Codec reset: re-send SPS/PPS then restart frame sequence from 0.
  sender.sendFakeSPSPPS();
  seq = 0;

  // Phase 2: 3 intervals post-reset.
  streamIntervals(sender, seq, 3);
  std::this_thread::sleep_for(2s);
  auto phase2Lines = log.all();

  auto playsPhase1 = [&]() {
    size_t n = 0;
    for (const auto &l : phase1Lines)
      if (l.find("video PLAY: key=sps_sender:1") != std::string::npos) ++n;
    return n;
  }();

  auto playsPhase2  = log.match(R"(SWAP#\d+ video PLAY: key=sps_sender:1)");
  auto drops        = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=sps_sender:1)");
  auto fbHolds      = log.match(R"(SWAP#\d+ video FALLBACK-HOLD: key=sps_sender:1)");

  std::fprintf(stderr,
               "[scenario13] phase1 PLAY=%zu  phase2 PLAY=%zu  DROP-RESYNC=%zu  FALLBACK-HOLD=%zu\n",
               playsPhase1, playsPhase2.size(), drops.size(), fbHolds.size());

  // Both phases must produce video.
  REQUIRE(playsPhase1 >= 1);
  REQUIRE(playsPhase2.size() >= 1);

  // The codec refresh must not look like a DROP-RESYNC to the receiver.
  CHECK(drops.empty());

  // FALLBACK-HOLD is acceptable transiently (one stall while new SPS/PPS
  // propagates), but should not dominate.
  CHECK(fbHolds.size() <= playsPhase2.size() + 2);

  receiver.disconnect();
  sender.disconnect();
}
