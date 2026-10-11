// Scenario 28 — the sender toggles voice chat (live mode, flag 2) on its audio channel while
// streaming video; after voice chat is turned off, receivers must deliver video again.
//
// With a live audio channel the receiver delivers that user's video frame by frame (the live
// delay line), bypassing the interval GUID match. Turning voice chat off brings the user back to
// the interval path: the receiver flushes the channel's audio decode state on the flag change, but
// the video sync state (prev_ds_guid, hold_count, synced, next/pending/accumulating, the live
// delay line) was kept. Field report (NinjamZap bot feeding OBS, 2026-10-10): after a musician
// turned voice chat off, the bot delivered no video for that user for minutes — the video kept
// arriving over the network — until voice chat was turned on again. Peers saw about an interval
// of live and interval frames mixed at each switch.
//
// Toggle on / off several times, at different points of the interval. After each "off", once the
// transition is over, the receiver must deliver the sender's video in every interval.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "catch_amalgamated.hpp"

#include "FakeFrame.h"
#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"
#include "njclient.h"

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

// Sender's audio channel 0 into / out of NINJAM live (voice chat) mode, as the app does.
void setVoiceChat(videosync::TestClient &sender, bool on) {
  sender.raw()->SetLocalChannelInfo(0, nullptr, false, 0, false, 0, false, false, false, 0,
                                    /*setflags=*/true, on ? 2 : 0);
  sender.raw()->NotifyServerOfChannelChange();
}

void waitIntervals(videosync::TestClient &c, int n) {
  int target = c.intervalSwapCount() + n;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10 * n);
  while (c.intervalSwapCount() < target && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
}

double intervalSeconds(videosync::TestClient &c) {
  int bpm = c.getBPM(), bpi = c.getBPI();
  return (bpm > 0 && bpi > 0) ? 60.0 * bpi / bpm : 2.0;
}

size_t framesFrom(videosync::TestClient &receiver, const char *who) {
  size_t n = 0;
  for (auto &f : receiver.drainVideoFrames())
    if (f.username.find(who) != std::string::npos) n++;
  return n;
}
} // namespace

TEST_CASE("28_voice_chat_toggle — video resumes after the sender turns voice chat off",
          "[sync][scenario28]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient receiver(makeOpts("anonymous:vc_recv", /*sendVideo=*/false));
  REQUIRE(receiver.connectAndJoin(8s));
  videosync::TestClient sender(makeOpts("anonymous:vc_sender", /*sendVideo=*/true));
  REQUIRE(sender.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  // Camera: frames at ~10 fps the whole time, like the app.
  std::atomic<bool> stop{false};
  std::thread camera([&] {
    uint32_t seq = 0;
    while (!stop.load()) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)sender.intervalSwapCount(), /*pad*/ 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(100ms);
    }
  });

  waitIntervals(sender, 4);
  receiver.drainVideoFrames();
  waitIntervals(sender, 2);
  size_t baseline = framesFrom(receiver, "vc_sender");
  std::fprintf(stderr, "[scenario28] baseline: %zu frames in 2 intervals\n", baseline);
  REQUIRE(baseline > 0);

  const double interval = intervalSeconds(sender);
  int failures = 0;
  for (int k = 0; k < 4; ++k) {
    // Switch at k/4 of the interval.
    waitIntervals(sender, 1);
    std::this_thread::sleep_for(std::chrono::duration<double>(interval * k / 4.0));
    setVoiceChat(sender, true);
    waitIntervals(sender, 3);
    size_t live = framesFrom(receiver, "vc_sender");

    std::this_thread::sleep_for(std::chrono::duration<double>(interval * k / 4.0));
    log.clear();
    setVoiceChat(sender, false);
    waitIntervals(sender, 3);   // transition
    size_t transition = framesFrom(receiver, "vc_sender");
    waitIntervals(sender, 4);   // must be delivering again
    size_t after = framesFrom(receiver, "vc_sender");
    size_t holds = log.match(R"(video HOLD: key=vc_sender)").size();
    size_t drops = log.match(R"(video DROP-RESYNC: key=vc_sender)").size();
    size_t plays = log.match(R"(video PLAY: key=vc_sender)").size();
    std::fprintf(stderr,
                 "[scenario28] cycle %d (switch at %d/4): live=%zu transition=%zu after=%zu  PLAY=%zu HOLD=%zu DROP-RESYNC=%zu\n",
                 k, k, live, transition, after, plays, holds, drops);
    if (after == 0) failures++;
    CHECK(after > 0);
  }

  stop = true;
  camera.join();
  std::fprintf(stderr, "[scenario28] cycles without video after voice chat off: %d / 4\n", failures);
  sender.disconnect();
  receiver.disconnect();
}
