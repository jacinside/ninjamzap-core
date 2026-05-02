// Scenario 2 — bug repro for the user-reported regression: "video plays almost real-
// time and doesn't wait an interval — video reproduced one interval earlier than audio."
//
// Root cause (today's code, njclient.cpp::on_new_interval, ~line 3158):
//
//     vs->playing.copyFrom(vs->next);   // plays during the receiver's interval [K, K+1)
//
// Audio for the same sender interval N is decoded at swap K (ds = next_ds[0]) but is
// audible only during [K+1, K+2) due to audio output buffering. Result: video appears
// one interval earlier than the matching audio. Fix would route video through a one-
// swap-deferred `pending` buffer (or otherwise hold playback until [K+1, K+2)).
//
// The test measures the actual end-to-end delay from `QueueVideoFrame()` on the sender
// to `VideoFrameReady_Callback` firing on the receiver, and compares it to the audio
// path's expected total delay (~2 × intervalDuration: 1× capture-to-receive,
// 1× decoder-to-speaker).
//
//   * Sender pushes a fake frame, embedding `nowMicros()` in its payload.
//   * Receiver records `nowMicros()` when its callback fires.
//   * Per-frame latency = recv_us − send_us.
//
// At BPM=240/BPI=4 (1.0s intervals):
//   * Correct (matched-audio sync) → ~1.5–2.0 s per frame.
//   * Buggy (real-time video)      → ~0.5–1.0 s per frame.
//
// The test FAILS today because the bug is present in the code. Once the fix lands
// (e.g., introduce a `pending` buffer to defer `next → playing` by one swap), the
// median latency will rise above the threshold and the test will pass.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

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

TEST_CASE("02_video_one_interval_early — measures actual send→deliver latency",
          "[sync][scenario2][bug-repro]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:bug_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:bug_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  const auto kIntervalUs = std::chrono::microseconds(1'000'000); // 240 BPM × 4 BPI

  // Wait for first swap on both before any frames are pushed.
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1)) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);
  REQUIRE(receiver.intervalSwapCount() >= 1);

  // Push a small batch (4 frames per interval) over many intervals. Small batch keeps
  // them under the BURST cap so most reach the receiver's `next` buffer in time.
  const int kIntervalsToTest = 6;
  const int kFramesPerInterval = 4;
  uint32_t seq = 0;
  for (int i = 0; i < kIntervalsToTest; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < kFramesPerInterval; ++f) {
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
  // Drain any in-flight intervals so trailing playbacks land.
  std::this_thread::sleep_for(2500ms);

  // Compute per-frame latencies.
  auto recv = receiver.drainVideoFrames();
  std::vector<int64_t> latenciesUs;
  for (const auto &r : recv) {
    videosync::FakeFramePayload p;
    if (!videosync::parseFakeFrame(r.data.data(), (int)r.data.size(), p)) continue;
    if (p.sendTimestampUs == 0 || r.deliverTimestampUs == 0) continue;
    int64_t lat = (int64_t)r.deliverTimestampUs - (int64_t)p.sendTimestampUs;
    latenciesUs.push_back(lat);
  }

  // Need a reasonable sample size to report a stable median.
  REQUIRE(latenciesUs.size() >= 5);

  std::sort(latenciesUs.begin(), latenciesUs.end());
  int64_t median = latenciesUs[latenciesUs.size() / 2];
  int64_t minLat = latenciesUs.front();
  int64_t maxLat = latenciesUs.back();

  // Acceptance band: 0.8× ≤ median ≤ 2.2× intervalDuration.
  //
  // * Below 0.8× ⇒ the receiver delivered the frame inside the SAME interval
  //   the sender produced it (real-time playback). That's the original
  //   "video plays one interval EARLIER than audio" bug.
  //
  // * Above 2.2× ⇒ the receiver is over-deferring — symptom of the iPad
  //   "video 2 intervals LATER than audio" regression.
  //
  // The exact target depends on the hardware audio output buffer, so we use a
  // generous band rather than a single tight threshold while the fix iterates.
  // Scenario 8 covers the structural PLAY → PROMOTE invariant separately and
  // doesn't depend on wall-clock timing.
  const int64_t kMinExpectedLatencyUs =
      (int64_t)((double)kIntervalUs.count() * 0.8);
  const int64_t kMaxExpectedLatencyUs =
      (int64_t)((double)kIntervalUs.count() * 2.2);

  int lowViolations = 0, highViolations = 0;
  for (auto l : latenciesUs) {
    if (l < kMinExpectedLatencyUs) ++lowViolations;
    else if (l > kMaxExpectedLatencyUs) ++highViolations;
  }
  int violations = lowViolations + highViolations;
  double violationPct = 100.0 * (double)violations / (double)latenciesUs.size();

  std::fprintf(stderr,
               "[scenario2] frames=%zu  min=%lldms  median=%lldms  max=%lldms\n"
               "[scenario2] expected band [%lldms, %lldms]  low=%d  high=%d  "
               "(%.1f%% out of band)\n",
               latenciesUs.size(),
               (long long)(minLat / 1000), (long long)(median / 1000),
               (long long)(maxLat / 1000),
               (long long)(kMinExpectedLatencyUs / 1000),
               (long long)(kMaxExpectedLatencyUs / 1000),
               lowViolations, highViolations, violationPct);

  // Diagnostic: log every PLAY/FALLBACK line so the developer sees what the receiver
  // actually did. FALLBACK is the most aggressive bypass-pending path; if it fired,
  // most/all frames are real-time.
  auto plays = log.match(R"(SWAP#\d+ video (PLAY|FALLBACK): key=bug_sender:1)");
  std::fprintf(stderr, "[scenario2] receiver swap events (%zu):\n", plays.size());
  for (const auto &l : plays) std::fprintf(stderr, "  %s\n", l.c_str());

  CAPTURE(median);
  CAPTURE(minLat);
  CAPTURE(maxLat);
  CAPTURE(violations);
  CAPTURE(violationPct);

  // === Bug assertions ===
  //
  // Median must land inside the [0.8×, 2.2×] interval band. Outside means either
  // the receiver is playing real-time (lower bound) or over-deferring (upper bound).
  CHECK(median >= kMinExpectedLatencyUs);
  CHECK(median <= kMaxExpectedLatencyUs);

  // Most frames must individually be inside the band. Allow 25% noise (handshake +
  // BURST + clock drift).
  CHECK(violationPct <= 25.0);

  receiver.disconnect();
  sender.disconnect();
}
