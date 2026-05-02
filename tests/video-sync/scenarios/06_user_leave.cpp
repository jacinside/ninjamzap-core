// Scenario 6 — sender disconnects mid-session, video state is reset.
//
// Sender + receiver join, sender streams 3 intervals, then sender disconnects.
// On the receiver side, when chanpresentmask hits 0, njclient.cpp emits a
//   SYNCLOG("USER-LEAVE video state reset: ...")
// and clears prev_ds_guid + last_played_audio_guid + hold_count + synced.
// This prevents stale state from causing spurious PREV matches if a different
// user reuses the same name later.
//
// Verify:
//   * "USER-LEAVE video state reset" line appears on the receiver after the disconnect.
//   * If a second sender with the same name connects and streams, no DROP-RESYNC fires.
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
} // namespace

TEST_CASE("06_user_leave — sender disconnect resets video state, reconnect re-syncs",
          "[sync][scenario6]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient receiver(makeOpts("anonymous:ul_recv", /*sendVideo=*/false));
  REQUIRE(receiver.connectAndJoin(8s));

  // First sender: stream a few intervals, then disconnect.
  uint32_t seq = 0;
  {
    videosync::TestClient sender1(makeOpts("anonymous:ul_sender", /*sendVideo=*/true));
    REQUIRE(sender1.connectAndJoin(8s));
    sender1.sendFakeSPSPPS();
    for (int i = 0; i < 3; ++i) streamForOneInterval(sender1, seq);
    sender1.disconnect();
  }
  // Allow the receiver's Run() loop time to process the user-leave message.
  std::this_thread::sleep_for(2s);

  auto leaves = log.match(R"(USER-LEAVE video state reset: key=ul_sender:1)");
  std::fprintf(stderr, "[scenario6] USER-LEAVE lines so far: %zu\n", leaves.size());
  CHECK(leaves.size() >= 1);

  // Second sender with the same anonymous name reconnects and streams.
  log.clear();
  {
    videosync::TestClient sender2(makeOpts("anonymous:ul_sender", /*sendVideo=*/true));
    REQUIRE(sender2.connectAndJoin(8s));
    sender2.sendFakeSPSPPS();
    for (int i = 0; i < 4; ++i) streamForOneInterval(sender2, seq);
    sender2.disconnect();
  }
  std::this_thread::sleep_for(1s);

  auto plays2 = log.match(R"(SWAP#\d+ video PLAY: key=ul_sender:1.*match=DS)");
  auto drops2 = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=ul_sender:1)");
  std::fprintf(stderr,
               "[scenario6] post-reconnect: PLAY=%zu  DROP-RESYNC=%zu\n",
               plays2.size(), drops2.size());

  REQUIRE(plays2.size() >= 2);
  CHECK(drops2.empty());

  receiver.disconnect();
}
