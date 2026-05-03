// Scenario 21 — video sender with no audio channel (sendAudio=false).
//
// NINJAM video sync relies on audio GUID matching: the sender embeds the current
// audio-DS GUID in each video interval marker, and the receiver matches it to
// the audio slot's GUID at swap time. When the sender has no audio channel,
// `audioHasData` is always false on the receiver, which means:
//
//   * The `(no audio)` HOLD path fires every swap — hold_count resets to 0.
//   * DROP-RESYNC never fires (hold_count never accumulates).
//   * Video is never played via the normal PLAY path.
//   * The FALLBACK path may fire if `accumulating` has data.
//
// This is a DOCUMENTED LIMITATION: video-only streams cannot sync with the
// GUID-based alignment. The test exists to:
//   1. Confirm the receiver does not deadlock or crash.
//   2. Confirm DROP-RESYNC does NOT fire (the no-audio HOLD resets the counter).
//   3. Document the PLAY count (expected: 0 or very few via FALLBACK).
//
// A future extension that uses a separate sync channel could lift this limitation.
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
videosync::TestClient::Options makeOpts(const std::string &user,
                                        bool sendAudio, bool sendVideo) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = sendAudio;
  o.sendVideo = sendVideo;
  return o;
}
} // namespace

TEST_CASE("21_video_only_sender — no audio means no GUID match; no DROP-RESYNC",
          "[sync][scenario21]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  // Sender: video only, no audio.
  videosync::TestClient sender(
      makeOpts("anonymous:vo_sender", /*audio=*/false, /*video=*/true));
  // Receiver: audio only (needs audio to participate in the session).
  videosync::TestClient receiver(
      makeOpts("anonymous:vo_recv", /*audio=*/true, /*video=*/false));

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

  uint32_t seq = 0;
  for (int i = 0; i < 6; ++i) {
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

  auto plays      = log.match(R"(SWAP#\d+ video PLAY: key=vo_sender:1)");
  auto holdsNoAud = log.match(R"(SWAP#\d+ video HOLD: key=vo_sender:1 \(no audio\))");
  auto drops      = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=vo_sender:1)");
  auto fallbacks  = log.match(R"(SWAP#\d+ video FALLBACK: key=vo_sender:1)");

  std::fprintf(stderr,
               "[scenario21] PLAY=%zu  HOLD(no audio)=%zu  FALLBACK=%zu  DROP-RESYNC=%zu\n",
               plays.size(), holdsNoAud.size(), fallbacks.size(), drops.size());

  // The hold (no audio) path must fire — confirms we're exercising the right code.
  CHECK(holdsNoAud.size() >= 1);

  // Critical: no DROP-RESYNC. The (no audio) hold resets hold_count to 0 every
  // swap, so the kHoldCapDrop=4 ceiling is never reached.
  CHECK(drops.empty());

  // PLAY count is informational — normally 0 for a video-only sender.
  std::fprintf(stderr, "[scenario21] documented limitation: "
               "video-only senders cannot sync via GUID matching. PLAY=%zu\n",
               plays.size());

  receiver.disconnect();
  sender.disconnect();
}
