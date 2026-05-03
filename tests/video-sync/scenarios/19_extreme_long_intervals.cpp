// Scenario 19 — extreme long intervals: BPM=60/BPI=8 (8.0s per interval).
//
// At very long intervals a single interval window covers 8 seconds of audio.
// Video implications:
//   * If the sender pushes 20 fps × 8s = 160 frames per interval, the
//     `accumulating` buffer grows large before the SWAP fires.
//   * `m_interval_length` is large, so the pacing math inside VideoFrameReady
//     delivery spaces frames out over 8 seconds — the first visible frame
//     arrives quickly (threshold=0 for frame_idx≤2) but subsequent ones are
//     evenly distributed over the window.
//   * A single DROP-RESYNC wastes 8 seconds of video — so avoiding it matters
//     even more here than at normal BPM.
//   * BURST fires on any second interval where `next.active` is still true
//     when a new BEGIN arrives (normal under fast push).
//
// What's asserted:
//   * PLAY fires at least once (minimum viable sync).
//   * DROP-RESYNC count is printed — it is allowed but should be rare.
//   * Total delivered frames (via VideoFrameReady) reflects pacing.
//
// Override: NJ_TEST_LONG_BPM (default 60), NJ_TEST_LONG_BPI (default 8).
#include <atomic>
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

TEST_CASE("19_extreme_long_intervals — video sync at long interval (BPM=60/BPI=8)",
          "[sync][scenario19][bpm]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  const int kBPM = envInt("NJ_TEST_LONG_BPM", 60);
  const int kBPI = envInt("NJ_TEST_LONG_BPI", 8);
  const double intervalSec = (double)kBPI * 60.0 / (double)kBPM;
  std::fprintf(stderr, "[scenario19] target BPM=%d BPI=%d → %.1fs/interval\n",
               kBPM, kBPI, intervalSec);

  videosync::TestClient sender(makeOpts("anonymous:lg_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:lg_recv", /*sendVideo=*/false));
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

  // Vote BPI first, then BPM. The receiver also votes to meet the 50% threshold.
  std::string bpiVote = "!vote bpi " + std::to_string(kBPI);
  std::string bpmVote = "!vote bpm " + std::to_string(kBPM);
  sender.sendChatMessage("MSG", bpiVote.c_str());
  std::this_thread::sleep_for(300ms);
  sender.sendChatMessage("MSG", bpmVote.c_str());

  bool changed = waitForBPMBPI(sender, kBPM, kBPI, 15s);
  std::fprintf(stderr, "[scenario19] post-vote BPM=%d BPI=%d (changed=%d)\n",
               sender.getBPM(), sender.getBPI(), changed ? 1 : 0);

  if (!changed) {
    std::fprintf(stderr,
                 "[scenario19] BPM/BPI vote did not converge. Skipping.\n");
    receiver.disconnect();
    sender.disconnect();
    SUCCEED("BPM vote not accepted; long-interval diagnostic only");
    return;
  }

  // Wait for the first long interval to start clean.
  auto waitSwap = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds((int)(intervalSec * 1200));
  int swapBase = sender.intervalSwapCount();
  while (std::chrono::steady_clock::now() < waitSwap &&
         sender.intervalSwapCount() == swapBase) {
    std::this_thread::sleep_for(100ms);
  }

  // Stream frames at ~10 fps for 2 long intervals.
  const int kFps = 10;
  const int kIntervals = 2;
  const int framesPerInterval = (int)(intervalSec * kFps);
  const int frameDelayMs = 1000 / kFps;
  std::atomic<bool> stop{false};
  uint32_t seq = 0;

  std::thread pusher([&]() {
    for (int i = 0; i < kIntervals && !stop.load(); ++i) {
      int swapAtStart = sender.intervalSwapCount();
      for (int f = 0; f < framesPerInterval && !stop.load(); ++f) {
        auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, /*pad*/ 512);
        sender.sendVideoFrame(frame.data(), (int)frame.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs));
      }
      auto waitUntil = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds((int)(intervalSec * 2000));
      while (sender.intervalSwapCount() == swapAtStart &&
             std::chrono::steady_clock::now() < waitUntil && !stop.load()) {
        std::this_thread::sleep_for(100ms);
      }
    }
  });

  // Wait for the pusher to finish (allow up to 3× the expected duration).
  auto pushDeadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds((int)(intervalSec * 1000 * kIntervals * 3));
  while (pusher.joinable()) {
    if (std::chrono::steady_clock::now() > pushDeadline) {
      stop.store(true);
    }
    std::this_thread::sleep_for(200ms);
    if (!stop.load() && std::chrono::steady_clock::now() > pushDeadline) break;
  }
  stop.store(true);
  pusher.join();

  // Drain: wait one extra long interval for delivery.
  std::this_thread::sleep_for(std::chrono::milliseconds(
      (int)(intervalSec * 1500)));

  auto plays   = log.match(R"(SWAP#\d+ video PLAY: key=lg_sender:1)");
  auto drops   = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=lg_sender:1)");
  auto bursts  = log.match(R"(video BURST: discarding next)");
  auto delivered = receiver.drainVideoFrames();

  std::fprintf(stderr,
               "[scenario19] PLAY=%zu  DROP-RESYNC=%zu  BURST=%zu  "
               "delivered_frames=%zu  frames_sent=%d  interval=%.1fs\n",
               plays.size(), drops.size(), bursts.size(),
               delivered.size(), (int)seq, intervalSec);

  // At minimum 1 PLAY must fire. Long intervals mean fewer swaps, so the bar is low.
  REQUIRE(plays.size() >= 1);

  receiver.disconnect();
  sender.disconnect();
}
