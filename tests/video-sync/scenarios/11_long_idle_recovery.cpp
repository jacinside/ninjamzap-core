// Scenario 11 — long idle then recovery.
//
// Sender streams 3 intervals, calls pauseVideo() and stays paused for 6 full
// intervals (no BEGIN markers, no frames), then resumeVideo() and streams 3
// more.
//
// Asserts:
//   * EMPTY events accumulate during the idle window — emptyCount climbs.
//   * NO USER-LEAVE fires (the sender is paused, not disconnected).
//   * After resume, PLAY events land again — receiver isn't stuck.
//   * No DROP-RESYNC during the whole run.
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

void waitOneInterval(videosync::TestClient &sender) {
  int swapAtStart = sender.intervalSwapCount();
  auto waitUntil = std::chrono::steady_clock::now() + 2s;
  while (sender.intervalSwapCount() == swapAtStart &&
         std::chrono::steady_clock::now() < waitUntil) {
    std::this_thread::sleep_for(20ms);
  }
}

// Extract the highest emptyCount seen for `key` (track the climbing counter).
int peakEmptyCount(const std::vector<std::string> &lines, const std::string &key) {
  std::regex re(R"(SWAP#\d+ video EMPTY: key=)" + key +
                R"( emptyCount=(\d+))");
  int peak = 0;
  for (const auto &l : lines) {
    std::smatch m;
    if (std::regex_search(l, m, re)) {
      int v = std::stoi(m[1]);
      if (v > peak) peak = v;
    }
  }
  return peak;
}
} // namespace

TEST_CASE("11_long_idle_recovery — sender paused 6 intervals, recovers cleanly",
          "[sync][scenario11]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:idle_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:idle_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1)) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  uint32_t seq = 0;
  // Warmup: 3 intervals of clean streaming.
  for (int i = 0; i < 3; ++i) streamForOneInterval(sender, seq);
  log.clear();

  // Long idle window.
  sender.pauseVideo();
  const int kIdleIntervals = 6;
  for (int i = 0; i < kIdleIntervals; ++i) waitOneInterval(sender);

  auto duringIdle = log.all();
  int peakEmpty = peakEmptyCount(duringIdle, "idle_sender:1");
  auto leavesDuringIdle = log.match(R"(USER-LEAVE video state reset: key=idle_sender:1)");

  // Resume and stream 3 more intervals.
  log.clear();
  sender.resumeVideo();
  sender.sendFakeSPSPPS();
  for (int i = 0; i < 3; ++i) streamForOneInterval(sender, seq);
  std::this_thread::sleep_for(2s);

  auto playsAfter   = log.match(R"(SWAP#\d+ video PLAY: key=idle_sender:1)");
  auto dropsAfter   = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=idle_sender:1)");
  auto leavesAfter  = log.match(R"(USER-LEAVE video state reset: key=idle_sender:1)");

  std::fprintf(stderr,
               "[scenario11] idle: peakEmpty=%d  USER-LEAVE=%zu  ||  resume: PLAY=%zu  DROP-RESYNC=%zu  USER-LEAVE=%zu\n",
               peakEmpty, leavesDuringIdle.size(),
               playsAfter.size(), dropsAfter.size(), leavesAfter.size());

  // (A) EMPTY counter should have climbed during idle. We don't require a
  //     specific value because the receiver clamps at first PLAY post-resume,
  //     but a paused stream over 6 intervals must produce at least one EMPTY.
  CHECK(peakEmpty >= 1);

  // (B) The sender did NOT disconnect — server keeps the user, so the receiver
  //     must NOT have emitted USER-LEAVE.
  CHECK(leavesDuringIdle.empty());
  CHECK(leavesAfter.empty());

  // (C) Recovery: after resume the receiver must observe at least 2 PLAY events.
  REQUIRE(playsAfter.size() >= 2);

  // (D) No DROP-RESYNC anywhere — toggling pause/resume should not look like
  //     audio drift.
  CHECK(dropsAfter.empty());

  receiver.disconnect();
  sender.disconnect();
}
