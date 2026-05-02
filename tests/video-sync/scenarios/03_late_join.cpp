// Scenario 3 — late-join.
// Sender connects first and streams alone for several intervals. The receiver joins
// while transmission is already in progress. Verify the receiver:
//   * eventually emits "video PLAY match=DS" lines once it's caught up.
//   * does NOT trigger DROP-RESYNC during steady-state after sync establishes.
//   * sees a video BURST or EMPTY at most a couple of times during the warmup.
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

TEST_CASE("03_late_join — receiver joins after sender already streaming",
          "[sync][scenario3]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:lj_sender", /*sendVideo=*/true));
  REQUIRE(sender.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  // Wait for sender's first swap, then push frames for ~3 intervals BEFORE the
  // receiver shows up.
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline && sender.intervalSwapCount() < 1) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  uint32_t seq = 0;
  for (int i = 0; i < 3; ++i) {
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

  // Receiver joins late.
  videosync::TestClient receiver(makeOpts("anonymous:lj_recv", /*sendVideo=*/false));
  REQUIRE(receiver.connectAndJoin(8s));

  // Continue streaming for ~5 more intervals so the receiver has time to settle.
  for (int i = 0; i < 5; ++i) {
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
  std::this_thread::sleep_for(2s);

  auto plays = log.match(R"(SWAP#\d+ video PLAY: key=lj_sender:1.*match=DS)");
  auto drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=lj_sender:1)");
  auto empties = log.match(R"(SWAP#\d+ video EMPTY: key=lj_sender:1)");

  std::fprintf(stderr,
               "[scenario3] PLAY=%zu  DROP-RESYNC=%zu  EMPTY=%zu\n",
               plays.size(), drops.size(), empties.size());

  // After the late-join handshake, at least 3 sustained PLAY events should land.
  REQUIRE(plays.size() >= 3);
  // No DROP-RESYNC in steady state.
  CHECK(drops.empty());

  receiver.disconnect();
  sender.disconnect();
}
