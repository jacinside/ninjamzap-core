// Scenario 14 — receiver disconnect / reconnect while sender streams.
//
// Models the real-world case where the iOS app goes to background (network
// drops), reconnects, and must resume receiving video without getting stuck.
//
// Sequence:
//   1. Sender + receiver-1 join. Receiver-1 gets at least 3 PLAY events.
//   2. Receiver-1 disconnects.
//   3. Sender keeps streaming for ~3 more intervals.
//   4. Receiver-2 (same username) reconnects.
//   5. After reconnect, receiver-2 must get at least 2 PLAY events and NO
//      DROP-RESYNC — the sender's VideoRecvState for peers and the server's
//      relay state must not be corrupted by the mid-stream disconnect.
//
// Key assertion: the reconnected receiver re-syncs within a few intervals.
// If hold_count never resets after the old peer's stale state is evicted, the
// new receiver would spin in HOLD until DROP-RESYNC fires.
#include <chrono>
#include <cstdio>
#include <memory>
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
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(20ms);
    }
  }
}
} // namespace

TEST_CASE("14_receiver_reconnect — reconnected receiver re-syncs without DROP-RESYNC",
          "[sync][scenario14]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:rr_sender", /*sendVideo=*/true));
  REQUIRE(sender.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  // Phase 1: initial receiver gets synced.
  {
    videosync::TestClient recv1(makeOpts("anonymous:rr_recv", /*sendVideo=*/false));
    REQUIRE(recv1.connectAndJoin(8s));

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           (sender.intervalSwapCount() < 1 || recv1.intervalSwapCount() < 1)) {
      std::this_thread::sleep_for(50ms);
    }
    REQUIRE(sender.intervalSwapCount() >= 1);

    uint32_t seq = 0;
    streamIntervals(sender, seq, 3);
    std::this_thread::sleep_for(1s);

    auto phase1Plays = log.match(R"(SWAP#\d+ video PLAY: key=rr_sender:1)");
    std::fprintf(stderr, "[scenario14] phase1 PLAY=%zu\n", phase1Plays.size());
    REQUIRE(phase1Plays.size() >= 2);

    recv1.disconnect();
  }

  // Sender keeps streaming while receiver is gone.
  std::this_thread::sleep_for(500ms);
  uint32_t seq2 = 100;
  streamIntervals(sender, seq2, 3);

  // Phase 2: new receiver reconnects.
  log.clear();
  videosync::TestClient recv2(makeOpts("anonymous:rr_recv", /*sendVideo=*/false));
  REQUIRE(recv2.connectAndJoin(8s));

  auto deadline2 = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline2 && recv2.intervalSwapCount() < 1) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(recv2.intervalSwapCount() >= 1);

  streamIntervals(sender, seq2, 5);
  std::this_thread::sleep_for(2s);

  auto phase2Plays = log.match(R"(SWAP#\d+ video PLAY: key=rr_sender:1)");
  auto phase2Drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=rr_sender:1)");

  std::fprintf(stderr, "[scenario14] phase2 PLAY=%zu  DROP-RESYNC=%zu\n",
               phase2Plays.size(), phase2Drops.size());

  // Reconnected receiver must get video again.
  REQUIRE(phase2Plays.size() >= 2);
  // Reconnect must NOT cause DROP-RESYNC — hold_count should reset via USER-LEAVE.
  CHECK(phase2Drops.empty());

  recv2.disconnect();
  sender.disconnect();
}
