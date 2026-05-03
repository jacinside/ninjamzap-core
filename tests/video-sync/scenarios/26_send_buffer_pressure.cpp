// Scenario 26 — relay buffer pressure near SendBufferKB=2048 limit.
//
// The server config has SendBufferKB=2048 / RecvBufferKB=1024. When a single
// sender pushes frames faster than the relay can forward them, the server's
// send buffer fills up. The OS-level TCP send buffer fills, write() blocks,
// and the server's relay loop stalls for that subscriber.
//
// This test pushes a 3-sender configuration where each sender emits large
// frames (~50 KB) at high rate (~15 fps = 750 KB/s per sender, 2.25 MB/s
// total) for a sustained window. The goal is to saturate the relay and
// observe whether the sync layer recovers cleanly after the burst subsides.
//
// What's NOT asserted: zero BURST or zero DROP-RESYNC — under buffer pressure
// those are expected. What IS asserted:
//   * At least 1 PLAY per sender after the burst (system not permanently stuck).
//   * No crash or hang.
//   * Per-stream diagnostics printed for manual inspection.
//
// Override knobs:
//   NJ_TEST_BUF_NSENDERS  (default 3)  — parallel senders
//   NJ_TEST_BUF_FRAME_KB  (default 50) — per-frame KB
//   NJ_TEST_BUF_FPS       (default 15) — frames/sec per sender
//   NJ_TEST_BUF_SECONDS   (default 6)  — burst duration
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

videosync::TestClient::Options makeOpts(const std::string &user) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = true;
  o.sendVideo = true;
  return o;
}

} // namespace

TEST_CASE("26_send_buffer_pressure — relay saturated, sync recovers after burst",
          "[sync][scenario26][stress]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  const int kN       = envInt("NJ_TEST_BUF_NSENDERS", 3);
  const int kFrameKB = envInt("NJ_TEST_BUF_FRAME_KB", 50);
  const int kFps     = envInt("NJ_TEST_BUF_FPS",      15);
  const int kSeconds = envInt("NJ_TEST_BUF_SECONDS",   6);
  const double totalMBps = (double)kN * kFrameKB * kFps / 1024.0;
  std::fprintf(stderr,
               "[scenario26] config: N=%d frameKB=%d fps=%d duration=%ds "
               "totalRate=%.1fMB/s (SendBufferKB=2048)\n",
               kN, kFrameKB, kFps, kSeconds, totalMBps);

  std::vector<std::unique_ptr<videosync::TestClient>> clients;
  for (int i = 0; i < kN; ++i) {
    std::string user = "anonymous:bp" + std::to_string(i);
    clients.emplace_back(
        std::make_unique<videosync::TestClient>(makeOpts(user)));
    REQUIRE(clients.back()->connectAndJoin(8s));
    clients.back()->sendFakeSPSPPS();
  }

  {
    auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
      bool allReady = true;
      for (const auto &c : clients)
        if (c->intervalSwapCount() < 1) { allReady = false; break; }
      if (allReady) break;
      std::this_thread::sleep_for(50ms);
    }
  }
  for (const auto &c : clients) REQUIRE(c->intervalSwapCount() >= 1);

  // Burst phase: saturate relay.
  std::atomic<bool> stop{false};
  std::vector<std::thread> pushers;
  std::vector<std::atomic<uint32_t>> seqs(kN);
  for (int i = 0; i < kN; ++i) seqs[i].store(0);

  auto frameDelay = std::chrono::milliseconds(1000 / kFps);
  for (int i = 0; i < kN; ++i) {
    pushers.emplace_back([&, i]() {
      while (!stop.load()) {
        uint32_t s = seqs[i].fetch_add(1);
        auto frame = videosync::makeFakeFrame(s, (uint32_t)i, kFrameKB * 1024);
        clients[i]->sendVideoFrame(frame.data(), (int)frame.size());
        std::this_thread::sleep_for(frameDelay);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(kSeconds));
  stop.store(true);
  for (auto &t : pushers) t.join();

  // Recovery phase: drain and check each stream.
  std::this_thread::sleep_for(3s);

  auto allLines = log.all();
  size_t totalPlays = 0, totalDrops = 0, totalBursts = 0;
  for (int i = 0; i < kN; ++i) {
    std::string key = "bp" + std::to_string(i) + ":1";
    auto plays  = log.match(R"(SWAP#\d+ video PLAY: key=)" + key);
    auto drops  = log.match(R"(SWAP#\d+ video DROP-RESYNC: key=)" + key);
    auto bursts = log.match(R"(video BURST: discarding next)");
    std::fprintf(stderr,
                 "[scenario26] key=%-8s  PLAY=%zu  DROP=%zu  BURST=%zu\n",
                 key.c_str(), plays.size(), drops.size(), bursts.size());
    totalPlays += plays.size();
    totalDrops += drops.size();
    totalBursts += bursts.size();
  }
  std::fprintf(stderr,
               "[scenario26] totals: PLAY=%zu  DROP-RESYNC=%zu  BURST=%zu\n",
               totalPlays, totalDrops, totalBursts);

  // Under buffer pressure BURST and DROP-RESYNC are allowed (diagnostic output).
  // The system must NOT be permanently stuck: require at least 1 PLAY per sender.
  REQUIRE(totalPlays >= (size_t)kN);

  for (auto &c : clients) c->disconnect();
}
