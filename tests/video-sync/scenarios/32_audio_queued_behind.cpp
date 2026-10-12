// Scenario 32 — video keeps playing when the receiver's audio falls an extra
// interval behind.
//
// The receiver's audio clock stalls for more than one interval (an audio engine
// restart, a device route change) and the lost time isn't made up. Audio intervals
// that arrived meanwhile stay queued: from then on the receiver plays each sender
// interval one swap later than before, with one more interval in next_ds.
//
// Video was matched only against the playing audio (ds) or the previous one, so
// every video interval now pointed at audio still sitting in the queue: HOLD, the
// next BEGIN replaced it, DROP-RESYNC after four, and video never came back while
// the extra interval stayed queued. Video whose audio is queued must wait for it
// (awaiting) and play when that audio does.
#include <chrono>
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

TEST_CASE("32_audio_queued_behind — video survives the receiver's audio falling behind",
          "[sync][scenario32]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient sender(makeOpts("anonymous:qb_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:qb_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1))
    std::this_thread::sleep_for(20ms);
  REQUIRE(receiver.intervalSwapCount() >= 1);

  // BPM 240 / BPI 4: 1 s intervals. Stream video throughout; after ~3 intervals
  // stall the receiver's audio clock for 1.5 intervals.
  uint32_t seq = 0;
  bool stalled = false;
  auto stallAt = std::chrono::steady_clock::now() + 3s;
  auto end = std::chrono::steady_clock::now() + 11s;
  while (std::chrono::steady_clock::now() < end) {
    auto f = videosync::makeFakeFrame(seq, seq / 10, 256);
    sender.sendVideoFrame(f.data(), (int)f.size());
    ++seq;
    if (!stalled && std::chrono::steady_clock::now() >= stallAt) {
      log.clear(); // count only what happens from the stall on
      receiver.stallAudio(1500);
      stalled = true;
    }
    std::this_thread::sleep_for(80ms);
  }
  std::this_thread::sleep_for(1500ms);

  auto queued = log.match(R"(SWAP#\d+ audio: user=qb_sender ch=0 ds=Y nds0=Y)");
  auto plays = log.match(R"(SWAP#\d+ video PLAY: key=qb_sender:1)");
  auto drops = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=qb_sender:1)");
  std::fprintf(stderr, "[scenario32] after stall: queuedAudioSwaps=%zu PLAY=%zu DROP-RESYNC=%zu\n",
               queued.size(), plays.size(), drops.size());

  // The stall must actually leave audio queued behind ds, or this tests nothing.
  REQUIRE(queued.size() >= 3);
  // ~8 intervals of video follow the stall; all but the first one or two must play.
  CHECK(plays.size() >= 5);
  CHECK(drops.empty());
}
