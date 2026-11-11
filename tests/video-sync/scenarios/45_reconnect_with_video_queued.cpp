// Scenario 45 — reconnecting while video kept being queued logs in.
//
// An app reconnecting after a lost connection (another Wi-Fi) keeps its camera running, so
// video goes on being queued while there is no connection. The server takes the client's first
// message as its answer to the login challenge; video drained into the new socket before that
// answer made it refuse the login ("invalid authorization reply"), every attempt.
//
// Setup: a sender with video connects, then disconnects and queues video with no connection,
// and also right after Connect (before the challenge arrives), as a camera does.
// Verify: the reconnect logs in (status OK), and its video reaches a receiver afterwards.
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
  o.recordOutput = !sendVideo;
  return o;
}

// What a camera does with no connection: raw video of an interval it began, frame by frame.
void queueRawVideo(NJClient *c, int frames) {
  unsigned char guid[16];
  c->RawDataSendBegin(guid, 0x4E5A5644 /* NZVD */, 1, 0);
  for (int i = 0; i < frames; i++) {
    auto f = videosync::makeFakeFrame((uint32_t)i, 0, 512);
    c->RawDataSendWrite(guid, f.data(), (int)f.size(), i == frames - 1);
  }
}
} // namespace

TEST_CASE("45_reconnect_with_video_queued — a reconnect with video queued logs in",
          "[sync][scenario45]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient sender(makeOpts("anonymous:rq_sender", true));
  videosync::TestClient receiver(makeOpts("anonymous:rq_recv", false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  // 1 s intervals, so the check below sees several: an earlier scenario may have left 8-16 s.
  if (sender.getBPI() != 4 || sender.getBPM() != 240) {
    sender.sendChatMessage("MSG", "!vote bpi 4");
    sender.sendChatMessage("MSG", "!vote bpm 240");
    receiver.sendChatMessage("MSG", "!vote bpi 4");
    receiver.sendChatMessage("MSG", "!vote bpm 240");
    const auto until = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < until && (sender.getBPI() != 4 || sender.getBPM() != 240))
      std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.getBPM() == 240);
  sender.sendFakeSPSPPS();
  std::this_thread::sleep_for(2s);

  // The connection goes away; the camera keeps going.
  sender.disconnect();
  queueRawVideo(sender.raw(), 30);

  // The camera also queues right after Connect, before the server's challenge.
  std::atomic<bool> stop{false};
  std::thread camera([&] {
    while (!stop) {
      queueRawVideo(sender.raw(), 3);
      std::this_thread::sleep_for(20ms);
    }
  });
  const bool loggedIn = sender.connectAndJoin(8s);
  stop = true;
  camera.join();
  std::fprintf(stderr, "[scenario45] reconnect status=%d (%s) error=\"%s\"\n", sender.raw()->GetStatus(),
               loggedIn ? "logged in" : "refused", sender.raw()->GetErrorStr() ? sender.raw()->GetErrorStr() : "");
  REQUIRE(loggedIn);

  // Its video reaches the receiver again.
  log.clear();
  sender.sendFakeSPSPPS();
  uint32_t seq = 0;
  const auto end = std::chrono::steady_clock::now() + 6s;
  while (std::chrono::steady_clock::now() < end) {
    auto f = videosync::makeFakeFrame(seq++, (uint32_t)sender.currentSwap(), 256);
    sender.sendVideoFrame(f.data(), (int)f.size());
    std::this_thread::sleep_for(80ms);
  }
  const auto begins = log.match(R"(video BEGIN.*rq_sender)");
  std::fprintf(stderr, "[scenario45] video BEGINs from the sender after reconnecting: %zu\n", begins.size());
  CHECK(!begins.empty());
}
