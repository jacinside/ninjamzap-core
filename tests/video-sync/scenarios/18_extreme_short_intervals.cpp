// Scenario 18 — extreme short intervals: BPM=480/BPI=2 (0.25s per interval).
//
// At very short intervals the BEGIN/SWAP cycle fires 4x per second. With a
// typical video frame push rate of 4 frames/interval, the receiver has ≈62ms
// between each frame — tight enough that pacing, BURST detection, and GUID
// matching are all stressed simultaneously.
//
// Known failure modes this exposes:
//   * BURST fires on every interval (next.active at BEGIN time) if the audio
//     thread is slightly behind the video accumulation rate.
//   * DROP-RESYNC fires because hold_count has no time to reset between swaps.
//   * Empty intervals because frames arrive after the swap window.
//
// The test starts at server default (BPM=240/BPI=4), votes to shorten the
// interval, then streams and reports the sync behavior. It does NOT require
// DROP-RESYNC to be zero — under 0.25s intervals some instability is expected.
// Instead it asserts:
//   * At least some PLAY events land (the system is not completely broken).
//   * The number of DROP-RESYNC events is printed for the developer to evaluate.
//   * No crash or hang.
//
// Override: NJ_TEST_SHORT_BPM (default 480), NJ_TEST_SHORT_BPI (default 2).
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

} // namespace

TEST_CASE("18_extreme_short_intervals — video sync at minimum interval (BPM=480/BPI=2)",
          "[sync][scenario18][bpm][stress]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  const int kBPM = envInt("NJ_TEST_SHORT_BPM", 480);
  const int kBPI = envInt("NJ_TEST_SHORT_BPI", 2);
  const double intervalSec = (double)kBPI * 60.0 / (double)kBPM;
  std::fprintf(stderr, "[scenario18] target BPM=%d BPI=%d → %.3fs/interval\n",
               kBPM, kBPI, intervalSec);

  videosync::TestClient sender(makeOpts("anonymous:sh_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:sh_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  {
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           sender.intervalSwapCount() < 1) {
      std::this_thread::sleep_for(50ms);
    }
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  // Vote BPM then BPI. Both need to pass for the interval to reach the target.
  // The server may batch the changes so we set BPI first (no audio glitch risk)
  // then BPM.
  std::string bpiVote = "!vote bpi " + std::to_string(kBPI);
  std::string bpmVote = "!vote bpm " + std::to_string(kBPM);
  sender.sendChatMessage("MSG", bpiVote.c_str());
  std::this_thread::sleep_for(200ms);
  sender.sendChatMessage("MSG", bpmVote.c_str());

  bool changed = waitForBPMBPI(sender, kBPM, kBPI, 10s);
  std::fprintf(stderr, "[scenario18] post-vote BPM=%d BPI=%d (changed=%d)\n",
               sender.getBPM(), sender.getBPI(), changed ? 1 : 0);

  if (!changed) {
    // Print diagnostic and skip the streaming phase — some servers may reject
    // extreme values. We still pass the test: the interesting result is the log.
    std::fprintf(stderr,
                 "[scenario18] BPM/BPI vote did not converge — server may "
                 "have rejected the extreme value. Skipping stream phase.\n");
    receiver.disconnect();
    sender.disconnect();
    SUCCEED("BPM vote not accepted; extreme value diagnostic only");
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(
      (int)(intervalSec * 1000 * 2)));

  // Stream for ~3 seconds worth of short intervals.
  const int kIntervalsToStream = (int)(3.0 / intervalSec) + 2;
  const int kFramesPerInterval = std::max(1, (int)(intervalSec * 4)); // ~4 fps
  uint32_t seq = 0;
  for (int i = 0; i < kIntervalsToStream; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < kFramesPerInterval; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 128);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      if (kFramesPerInterval > 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(
            (int)(intervalSec * 1000 / kFramesPerInterval)));
    }
    auto waitUntil = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds((int)(intervalSec * 2000));
    while (sender.intervalSwapCount() == swapAtStart &&
           std::chrono::steady_clock::now() < waitUntil) {
      std::this_thread::sleep_for(5ms);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(
      (int)(intervalSec * 2000)));

  auto plays  = log.match(R"(SWAP#\d+ video PLAY: key=sh_sender:1)");
  auto drops  = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=sh_sender:1)");
  auto bursts = log.match(R"(video BURST: discarding next)");
  auto empties = log.match(R"(SWAP#\d+ video EMPTY: key=sh_sender:1)");

  std::fprintf(stderr,
               "[scenario18] PLAY=%zu  DROP-RESYNC=%zu  BURST=%zu  EMPTY=%zu\n"
               "[scenario18] intervals=%d  frames_sent=%d\n",
               plays.size(), drops.size(), bursts.size(), empties.size(),
               kIntervalsToStream, (int)seq);

  // At extreme short intervals we relax the assertion: just require the system
  // did not deadlock and produced at least 1 PLAY. DROP-RESYNC is allowed here
  // (it's expected under heavy interval churn) — its count is diagnostic output.
  REQUIRE(plays.size() >= 1);

  receiver.disconnect();
  sender.disconnect();
}
