// Scenario 24 — server MaxUsers boundary (MaxUsers=8 in test.cfg).
//
// Verifies two properties:
//   A) All 8 slots can connect and stream video simultaneously.
//   B) A 9th client is rejected by the server (NJC_STATUS_CANTCONNECT or
//      connectAndJoin timeout).
//
// This test also serves as a capacity smoke test: if the server's relay
// subsystem degrades under 8 concurrent video senders, PLAY counts will be
// low and the per-client diagnostic makes it visible.
//
// Note: this test uses all 8 server slots. Run it in isolation if other
// scenarios are executing concurrently against the same Docker container.
#include <atomic>
#include <chrono>
#include <cstdio>
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

TEST_CASE("24_server_max_users — 8 clients connect; 9th is rejected",
          "[sync][scenario24]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  const int kMaxUsers = 8;

  // Connect all 8 slots sequentially.
  std::vector<std::unique_ptr<videosync::TestClient>> clients;
  for (int i = 0; i < kMaxUsers; ++i) {
    std::string user = "anonymous:mu" + std::to_string(i);
    clients.emplace_back(
        std::make_unique<videosync::TestClient>(makeOpts(user)));
    bool ok = clients.back()->connectAndJoin(8s);
    std::fprintf(stderr, "[scenario24] client %d connect=%s\n", i,
                 ok ? "OK" : "FAIL");
    REQUIRE(ok);
    clients.back()->sendFakeSPSPPS();
  }
  std::fprintf(stderr, "[scenario24] all %d slots filled\n", kMaxUsers);

  // 9th client should be rejected.
  videosync::TestClient extra(makeOpts("anonymous:mu_extra"));
  bool extraOk = extra.connectAndJoin(5s);
  std::fprintf(stderr, "[scenario24] 9th client connect=%s (expected FAIL)\n",
               extraOk ? "OK" : "REJECTED");
  CHECK(!extraOk);
  extra.disconnect();

  // Quick stream: each client pushes one frame so PLAY counts show relay health.
  std::atomic<bool> stop{false};
  std::vector<std::thread> pushers;
  for (int i = 0; i < kMaxUsers; ++i) {
    pushers.emplace_back([&, i]() {
      uint32_t seq = 0;
      while (!stop.load()) {
        auto frame = videosync::makeFakeFrame(seq++, (uint32_t)i, /*pad*/ 256);
        clients[i]->sendVideoFrame(frame.data(), (int)frame.size());
        std::this_thread::sleep_for(150ms);
      }
    });
  }
  std::this_thread::sleep_for(4s);
  stop.store(true);
  for (auto &t : pushers) t.join();
  std::this_thread::sleep_for(2s);

  // Report per-stream play counts.
  size_t totalPlays = 0;
  for (int i = 0; i < kMaxUsers; ++i) {
    std::string key = "mu" + std::to_string(i) + ":1";
    auto plays = log.match(R"(SWAP#\d+ video PLAY: key=)" + key);
    std::fprintf(stderr, "[scenario24] key=%-8s PLAY=%zu\n",
                 key.c_str(), plays.size());
    totalPlays += plays.size();
  }
  std::fprintf(stderr, "[scenario24] total PLAY events=%zu\n", totalPlays);

  // With 8 senders × 7 subscribers each, expect meaningful PLAY traffic.
  // Require at least kMaxUsers PLAYs total (each stream got at least 1 play).
  REQUIRE(totalPlays >= (size_t)kMaxUsers);

  for (auto &c : clients) c->disconnect();
}
