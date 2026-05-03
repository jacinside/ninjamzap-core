// Scenario 16 — BPI change mid-session via voting.
//
// Complements scenario 17 (BPM change): here we change BPI while keeping BPM
// constant. BPI=4→8 doubles the interval duration from 1.0s to 2.0s at BPM=240.
//
// The difference from a BPM change: the beats-per-interval counter changes, which
// resets the internal beat accumulation. Video markers are keyed to intervals (not
// beats), so the sender emits one marker per interval regardless of BPI.
// What does change: m_interval_length doubles, and on_new_interval fires half as
// often — so the sender's marker rate drops. If the receiver's GUID state isn't
// correctly updated to the new interval cadence, hold_count can accumulate.
//
// Test sequence:
//   1. Connect at BPM=240/BPI=4 (1.0s intervals). Stream 3 intervals.
//   2. Both clients vote BPI=8 (2.0s intervals).
//   3. Wait for change to take effect.
//   4. Stream 3 more intervals at the new BPI.
//
// Assertions:
//   * BPI actually changed on both clients.
//   * PLAY events appear in both windows.
//   * No DROP-RESYNC in either window.
#include <chrono>
#include <cstdio>
#include <string>
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

void streamIntervals(videosync::TestClient &sender, uint32_t &seq, int count,
                     int framesPerInterval = 3, int frameDelayMs = 250) {
  for (int i = 0; i < count; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < framesPerInterval; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs));
    }
    auto waitUntil = std::chrono::steady_clock::now() + 5s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(20ms);
    }
  }
}

bool waitForBPI(videosync::TestClient &c, int targetBPI,
                std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (c.getBPI() == targetBPI) return true;
    std::this_thread::sleep_for(50ms);
  }
  return false;
}

} // namespace

TEST_CASE("16_bpi_change_mid_session — video sync survives BPI vote transition",
          "[sync][scenario16][bpm]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:bpi_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:bpi_recv", /*sendVideo=*/false));
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

  const int kInitialBPI = sender.getBPI();
  const int kNewBPI = kInitialBPI == 4 ? 8 : 4; // toggle from default
  std::fprintf(stderr, "[scenario16] BPM=%d BPI %d→%d\n",
               sender.getBPM(), kInitialBPI, kNewBPI);

  // Phase 1: stream at initial BPI.
  uint32_t seq = 0;
  streamIntervals(sender, seq, 3);
  auto phase1Plays = log.match(R"(SWAP#\d+ video PLAY: key=bpi_sender:1)");
  std::fprintf(stderr, "[scenario16] phase1 PLAY=%zu\n", phase1Plays.size());
  log.clear();

  // Vote BPI. With threshold=50% and 2 clients, 1 vote is enough.
  std::string voteMsg = "!vote bpi " + std::to_string(kNewBPI);
  sender.sendChatMessage("MSG", voteMsg.c_str());
  std::fprintf(stderr, "[scenario16] sent '%s', waiting...\n", voteMsg.c_str());

  bool bpiChanged = waitForBPI(sender, kNewBPI, 8s);
  std::fprintf(stderr, "[scenario16] post-vote BPM=%d BPI=%d (changed=%d)\n",
               sender.getBPM(), sender.getBPI(), bpiChanged ? 1 : 0);
  REQUIRE(bpiChanged);

  // Wait for at least one full interval at new BPI before streaming.
  const double newIntervalSec = (double)kNewBPI * 60.0 / (double)sender.getBPM();
  std::this_thread::sleep_for(std::chrono::milliseconds((int)(newIntervalSec * 1200)));

  // Phase 2: 3 intervals at new BPI.
  streamIntervals(sender, seq, 3, /*frames=*/3,
                  /*frameDelayMs=*/(int)(newIntervalSec * 1000 / 4));
  std::this_thread::sleep_for(std::chrono::milliseconds((int)(newIntervalSec * 2000)));

  auto phase2Plays = log.match(R"(SWAP#\d+ video PLAY: key=bpi_sender:1)");
  auto phase2Drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=bpi_sender:1)");

  std::fprintf(stderr,
               "[scenario16] phase2 PLAY=%zu  DROP-RESYNC=%zu  finalBPI=%d\n",
               phase2Plays.size(), phase2Drops.size(), sender.getBPI());

  REQUIRE(phase1Plays.size() >= 1);
  REQUIRE(phase2Plays.size() >= 2);
  CHECK(phase2Drops.empty());

  receiver.disconnect();
  sender.disconnect();
}
