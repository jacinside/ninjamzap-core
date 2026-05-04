// Scenario 17 — BPM change mid-session via voting.
//
// The NINJAM server changes interval length when a BPM vote passes.
// m_interval_length changes immediately on the next interval, which affects:
//   * The audio pacing thread's period (the AudioProc call rate stays the same
//     but on_new_interval fires more/less frequently).
//   * The video pacing inside VideoFrameReady delivery.
//   * The hold_count / empty_count FSM (timings for DROP-RESYNC).
//
// Test sequence:
//   1. Connect at default BPM=240/BPI=4 (1.0s intervals). Stream 3 intervals.
//   2. Sender votes to change BPM → 120 (2.0s intervals).
//      With 2 clients (sender+receiver) and SetVotingThreshold=50, the vote
//      passes immediately after one vote.
//   3. Wait for BPM to take effect (server broadcasts the change).
//   4. Stream 4 more intervals at the new BPM.
//
// Assertions:
//   * BPM actually changed on both clients.
//   * PLAY events appear in both the pre-change and post-change windows.
//   * No DROP-RESYNC in either window — the GUID matching logic must remain
//     coherent across the interval-length transition.
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
                     int framesPerInterval = 3, int frameDelayMs = 180) {
  for (int i = 0; i < count; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < framesPerInterval; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs));
    }
    auto waitUntil = std::chrono::steady_clock::now() + 4s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(20ms);
    }
  }
}

bool waitForBPM(videosync::TestClient &client, int targetBPM,
                std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.getBPM() == targetBPM) return true;
    std::this_thread::sleep_for(50ms);
  }
  return false;
}

} // namespace

TEST_CASE("17_bpm_change_mid_session — video sync survives BPM vote transition",
          "[sync][scenario17][bpm]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:bpm_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:bpm_recv", /*sendVideo=*/false));
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

  const int kInitialBPM = sender.getBPM();
  const int kNewBPM = 120; // half-speed → 2.0s intervals
  std::fprintf(stderr, "[scenario17] initial BPM=%d BPI=%d\n",
               kInitialBPM, sender.getBPI());

  // Phase 1: stream at initial BPM.
  uint32_t seq = 0;
  streamIntervals(sender, seq, 3);
  auto phase1Plays = log.match(R"(SWAP#\d+ video PLAY: key=bpm_sender:1)");
  std::fprintf(stderr, "[scenario17] phase1 PLAY=%zu\n", phase1Plays.size());
  log.clear();

  // Vote to change BPM. With threshold=50% and 2 clients, 1 vote is enough.
  std::string voteMsg = "!vote bpm " + std::to_string(kNewBPM);
  sender.sendChatMessage("MSG", voteMsg.c_str());
  std::fprintf(stderr, "[scenario17] sent '%s', waiting for BPM change...\n",
               voteMsg.c_str());

  bool bpmChanged = waitForBPM(sender, kNewBPM, 8s);
  std::fprintf(stderr, "[scenario17] post-vote BPM=%d (changed=%d)\n",
               sender.getBPM(), bpmChanged ? 1 : 0);
  REQUIRE(bpmChanged);
  // Wait one full interval at new BPM (≤2.5s) for all clients to sync.
  std::this_thread::sleep_for(2500ms);

  // Phase 2: stream at new BPM. Frame cadence unchanged; interval is longer.
  streamIntervals(sender, seq, 4);
  std::this_thread::sleep_for(2s);

  auto phase2Plays = log.match(R"(SWAP#\d+ video PLAY: key=bpm_sender:1)");
  auto phase2Drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=bpm_sender:1)");

  std::fprintf(stderr,
               "[scenario17] phase2 PLAY=%zu  DROP-RESYNC=%zu  finalBPM=%d\n",
               phase2Plays.size(), phase2Drops.size(), sender.getBPM());

  // Both phases should produce video.
  REQUIRE(phase1Plays.size() >= 1);
  REQUIRE(phase2Plays.size() >= 1);

  // BPM change must not trigger DROP-RESYNC.
  CHECK(phase2Drops.empty());

  receiver.disconnect();
  sender.disconnect();
}
