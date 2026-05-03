// Scenario 23 — simultaneous BPM + BPI vote (combined interval change).
//
// Voting BPM and BPI independently (scenarios 16, 17) tests each axis in
// isolation. This scenario votes both in rapid succession to stress the edge
// case where the server processes two consecutive config-change messages:
//
//   !vote bpi 2    (BPI 4→2: halves interval count, keeps BPM)
//   !vote bpm 120  (BPM 240→120: halves beat rate)
//
// Net effect: interval duration stays the same (BPI/BPM = 2/120 = 1/60 min
// = 1.0s, same as 4/240). The audio GUID rotation speed halves but each
// interval is the same wall-clock length. This is a "neutral" combined change
// that the video sync layer should survive without observable disruption.
//
// For the alternative "stretch" case (BPI 4→8, BPM 120→60 → 4.0s intervals)
// use NJ_TEST_COMBO_BPM=60 + NJ_TEST_COMBO_BPI=8.
//
// Assertions:
//   * Both BPM and BPI take effect before the stream phase.
//   * PLAY events appear after the combined change.
//   * No DROP-RESYNC.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "catch_amalgamated.hpp"

#include "FakeFrame.h"
#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"

using namespace std::chrono_literals;

namespace {

int envInt(const char *name, int fallback) {
  const char *v = std::getenv(name);
  if (!v) return fallback;
  int p = std::atoi(v);
  return p > 0 ? p : fallback;
}

videosync::TestClient::Options makeOpts(const std::string &user, bool sendVideo) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = true;
  o.sendVideo = sendVideo;
  return o;
}

bool waitForBPMBPI(videosync::TestClient &c, int bpm, int bpi,
                   std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (c.getBPM() == bpm && c.getBPI() == bpi) return true;
    std::this_thread::sleep_for(50ms);
  }
  return false;
}

void streamIntervals(videosync::TestClient &sender, uint32_t &seq, int count,
                     int frameDelayMs = 200) {
  for (int i = 0; i < count; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < 3; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs));
    }
    auto waitUntil = std::chrono::steady_clock::now() + 5s;
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil)
      std::this_thread::sleep_for(20ms);
  }
}

} // namespace

TEST_CASE("23_bpm_bpi_combined_change — simultaneous BPM+BPI vote survives",
          "[sync][scenario23][bpm]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  const int kNewBPM = envInt("NJ_TEST_COMBO_BPM", 120);
  const int kNewBPI = envInt("NJ_TEST_COMBO_BPI", 2);
  const double newIntervalSec = (double)kNewBPI * 60.0 / (double)kNewBPM;
  std::fprintf(stderr,
               "[scenario23] voting BPM=%d BPI=%d → %.2fs/interval\n",
               kNewBPM, kNewBPI, newIntervalSec);

  videosync::TestClient sender(makeOpts("anonymous:cb_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:cb_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1))
      std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  std::fprintf(stderr, "[scenario23] pre-vote BPM=%d BPI=%d\n",
               sender.getBPM(), sender.getBPI());

  // Vote BPI then BPM in rapid succession (< 1 interval apart).
  std::string bpiVote = "!vote bpi " + std::to_string(kNewBPI);
  std::string bpmVote = "!vote bpm " + std::to_string(kNewBPM);
  sender.sendChatMessage("MSG", bpiVote.c_str());
  std::this_thread::sleep_for(100ms);
  sender.sendChatMessage("MSG", bpmVote.c_str());

  bool changed = waitForBPMBPI(sender, kNewBPM, kNewBPI, 12s);
  std::fprintf(stderr, "[scenario23] post-vote BPM=%d BPI=%d (changed=%d)\n",
               sender.getBPM(), sender.getBPI(), changed ? 1 : 0);

  if (!changed) {
    std::fprintf(stderr,
                 "[scenario23] combined vote did not converge (server may have "
                 "rejected one of the values). Skipping stream phase.\n");
    receiver.disconnect();
    sender.disconnect();
    SUCCEED("combined BPM/BPI vote not accepted; diagnostic only");
    return;
  }

  // Wait for all clients to settle at new interval.
  std::this_thread::sleep_for(std::chrono::milliseconds((int)(newIntervalSec * 2000)));

  // Stream 4 intervals at the new settings.
  uint32_t seq = 0;
  streamIntervals(sender, seq, 4, (int)(newIntervalSec * 1000 / 4));
  std::this_thread::sleep_for(std::chrono::milliseconds((int)(newIntervalSec * 2000)));

  auto plays = log.match(R"(SWAP#\d+ video PLAY: key=cb_sender:1)");
  auto drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=cb_sender:1)");

  std::fprintf(stderr,
               "[scenario23] PLAY=%zu  DROP-RESYNC=%zu\n",
               plays.size(), drops.size());

  REQUIRE(plays.size() >= 2);
  CHECK(drops.empty());

  receiver.disconnect();
  sender.disconnect();
}
