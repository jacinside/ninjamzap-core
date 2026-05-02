// Scenario 4 — gap in video transmission.
// Sender + receiver join, sender streams 3 intervals, then calls pauseVideo() and
// stops pushing frames for 2 intervals (no BEGIN markers reach the receiver), then
// resumeVideo() and streams 3 more.
//
// Verify the receiver:
//   * Emits PLAY events during the active phases.
//   * Emits at least one EMPTY (or no PLAY) line during the gap.
//   * Recovers PLAY after the gap without persistent DROP-RESYNC.
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

void streamForOneInterval(videosync::TestClient &sender, uint32_t &seq) {
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
} // namespace

TEST_CASE("04_gap_in_video — sender pauses video mid-stream then resumes",
          "[sync][scenario4]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:gap_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:gap_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  // Wait for first swap.
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1)) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  uint32_t seq = 0;

  // Phase 1: stream 3 intervals.
  for (int i = 0; i < 3; ++i) streamForOneInterval(sender, seq);
  log.clear(); // throw away handshake noise

  // Phase 2: pause and let 2 intervals pass with NO video frames or BEGINs.
  sender.pauseVideo();
  for (int i = 0; i < 2; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    auto waitUntil = std::chrono::steady_clock::now() + 2s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(20ms);
    }
  }

  // Phase 3: resume and stream 3 more intervals.
  sender.resumeVideo();
  sender.sendFakeSPSPPS(); // SPS/PPS gets cleared on resume in our wrapper; re-cache
  for (int i = 0; i < 3; ++i) streamForOneInterval(sender, seq);
  std::this_thread::sleep_for(2s);

  auto plays   = log.match(R"(SWAP#\d+ video PLAY: key=gap_sender:1.*match=DS)");
  auto empties = log.match(R"(SWAP#\d+ video EMPTY: key=gap_sender:1)");
  auto drops   = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=gap_sender:1)");

  std::fprintf(stderr,
               "[scenario4] PLAY=%zu  EMPTY=%zu  DROP-RESYNC=%zu (after pause clear)\n",
               plays.size(), empties.size(), drops.size());

  // We re-cleared the log right before pausing, so PLAY count reflects only
  // post-pause activity (gap + resume). The gap should produce at least one
  // EMPTY line; resume should produce at least 2 PLAYs.
  CHECK(empties.size() >= 1);
  REQUIRE(plays.size() >= 2);
  CHECK(drops.empty());

  receiver.disconnect();
  sender.disconnect();
}
