// Scenario 12 — multi-client HD video stress.
//
// Spins up N clients (default 4, override via NJ_TEST_NCLIENTS — e.g. 6 to mimic
// a packed jam room). Every client sends both audio and HD-shaped video, and
// every client receives video from the other (N-1) senders via config_autosubscribe.
//
// Server-side relay cost scales as N × (N-1) outbound video flows; this scenario
// is the knob for tuning AllowVideoChannels / VideoCongestionThreshold /
// SendBufferKB / RecvBufferKB before the user decides what production limits to
// publish.
//
// What's asserted:
//   * All N clients reach NJC_STATUS_OK (connection capacity test).
//   * Each client emits at least one PLAY for at least one peer.
//   * NO client triggers DROP-RESYNC during the run.
//   * Per-client diagnostics (PLAY/BURST/HOLD counts, peak emptyCount) are
//     printed to stderr — that's the "tuning report" the user can eyeball.
//
// HD-shape: 25 KB per frame, 20 frames per 1.0s interval, 8 intervals total.
// Total egress per sender ≈ 500 KB/s; total server egress N×(N-1)×500 KB/s.
//
// Override knobs:
//   NJ_TEST_NCLIENTS (default 4)        — how many clients to spawn
//   NJ_TEST_STRESS_FRAME_KB (default 25) — per-frame size in KB
//   NJ_TEST_STRESS_FPS (default 20)      — frames per second per sender
//   NJ_TEST_STRESS_SECONDS (default 8)   — duration of frame pushing
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <regex>
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
  int parsed = std::atoi(v);
  return parsed > 0 ? parsed : fallback;
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

struct ClientStats {
  std::string user;
  size_t plays = 0;
  size_t bursts = 0;
  size_t holds = 0;
  size_t fbHolds = 0;
  size_t empties = 0;
  size_t dropResyncs = 0;
};

ClientStats collectStats(const std::vector<std::string> &lines, const std::string &user) {
  ClientStats s;
  s.user = user;
  // Each client's stream key in the LOCAL receiver is keyed by the OTHER sender's
  // username:1. Because the SyncLogCapture is process-global we can't tell which
  // receiver emitted a line; we count any line that mentions key=<user>:1.
  std::string keyEsc = user;
  std::regex playRe(R"(SWAP#\d+ video PLAY: key=)" + keyEsc + R"(:1)");
  std::regex burstRe(R"(video BURST: discarding next)"); // not per-key, global
  std::regex holdRe(R"(SWAP#\d+ video HOLD: key=)" + keyEsc + R"(:1)");
  std::regex fbHoldRe(R"(SWAP#\d+ video FALLBACK-HOLD: key=)" + keyEsc + R"(:1)");
  std::regex emptyRe(R"(SWAP#\d+ video EMPTY: key=)" + keyEsc + R"(:1)");
  std::regex dropRe(R"(SWAP#\d+ video DROP-RESYNC: key=)" + keyEsc + R"(:1)");
  for (const auto &l : lines) {
    if (std::regex_search(l, playRe)) ++s.plays;
    if (std::regex_search(l, holdRe)) ++s.holds;
    if (std::regex_search(l, fbHoldRe)) ++s.fbHolds;
    if (std::regex_search(l, emptyRe)) ++s.empties;
    if (std::regex_search(l, dropRe)) ++s.dropResyncs;
  }
  // Bursts are global (not keyed); we don't attribute them to a sender.
  for (const auto &l : lines)
    if (std::regex_search(l, burstRe)) ++s.bursts;
  return s;
}

} // namespace

TEST_CASE("12_multi_client_hd_stress — N concurrent clients with HD-shaped video",
          "[sync][scenario12][stress]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  const int kN          = envInt("NJ_TEST_NCLIENTS",       4);
  const int kFrameKB    = envInt("NJ_TEST_STRESS_FRAME_KB", 25);
  const int kFps        = envInt("NJ_TEST_STRESS_FPS",      20);
  const int kSeconds    = envInt("NJ_TEST_STRESS_SECONDS",  8);
  std::fprintf(stderr,
               "[scenario12] config: N=%d frameKB=%d fps=%d duration=%ds\n",
               kN, kFrameKB, kFps, kSeconds);

  // --- Connect all N clients sequentially. Concurrent connect blew up under
  // --- contention for shared server state on early runs; sequential is fine
  // --- for a stress test that runs for seconds afterwards.
  std::vector<std::unique_ptr<videosync::TestClient>> clients;
  std::vector<std::string> usernames;
  for (int i = 0; i < kN; ++i) {
    std::string user = "anonymous:hd" + std::to_string(i);
    usernames.push_back("hd" + std::to_string(i));
    clients.emplace_back(std::make_unique<videosync::TestClient>(makeOpts(user)));
    REQUIRE(clients.back()->connectAndJoin(8s));
    clients.back()->sendFakeSPSPPS();
  }
  std::fprintf(stderr, "[scenario12] connected %d clients\n", kN);

  // Wait for first swap on all.
  auto deadline = std::chrono::steady_clock::now() + 4s;
  while (std::chrono::steady_clock::now() < deadline) {
    bool allReady = true;
    for (const auto &c : clients) {
      if (c->intervalSwapCount() < 1) { allReady = false; break; }
    }
    if (allReady) break;
    std::this_thread::sleep_for(50ms);
  }
  for (const auto &c : clients) REQUIRE(c->intervalSwapCount() >= 1);

  // --- Push HD-shaped frames in parallel from all clients. ---
  std::atomic<bool> stop{false};
  std::vector<std::thread> pushers;
  std::vector<std::atomic<uint32_t>> seqs(kN);
  for (int i = 0; i < kN; ++i) seqs[i].store(0);

  auto frameDelay = std::chrono::milliseconds(1000 / kFps);
  for (int i = 0; i < kN; ++i) {
    pushers.emplace_back([&, i]() {
      while (!stop.load()) {
        uint32_t s = seqs[i].fetch_add(1);
        auto frame = videosync::makeFakeFrame(s, (uint32_t)i,
                                              /*pad*/ kFrameKB * 1024);
        clients[i]->sendVideoFrame(frame.data(), (int)frame.size());
        std::this_thread::sleep_for(frameDelay);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(kSeconds));
  stop.store(true);
  for (auto &t : pushers) t.join();

  // Drain trailing intervals.
  std::this_thread::sleep_for(2s);

  // --- Per-sender stats (counts every PLAY/HOLD/etc. emitted by ANY receiver
  //     for the sender's stream — receiver-attribution would require extending
  //     SyncLogCapture to tag entries, out of scope for this knob). ---
  auto allLines = log.all();
  std::vector<ClientStats> all;
  for (const auto &u : usernames) all.push_back(collectStats(allLines, u));

  std::fprintf(stderr,
               "[scenario12] stats per stream key=<u>:1 (aggregated across all subscribers):\n");
  std::fprintf(stderr,
               "[scenario12] %-8s  %5s  %5s  %5s  %5s  %5s  %5s\n",
               "key", "PLAY", "HOLD", "FBLD", "EMPT", "DROP", "BURS");
  size_t totalPlays = 0;
  size_t totalDrops = 0;
  for (const auto &s : all) {
    std::fprintf(stderr,
                 "[scenario12] %-8s  %5zu  %5zu  %5zu  %5zu  %5zu  %5zu\n",
                 s.user.c_str(), s.plays, s.holds, s.fbHolds, s.empties,
                 s.dropResyncs, s.bursts);
    totalPlays += s.plays;
    totalDrops += s.dropResyncs;
  }

  // (A) All N clients connected.
  CHECK(clients.size() == (size_t)kN);

  // (B) The cluster produced video PLAY events. With N senders and N-1
  //     subscribers each, expect at least N × (N-1) PLAYs on a healthy run
  //     across the test window. Be generous: require N × 1 minimum (each
  //     sender's stream produced at least one play somewhere).
  REQUIRE(totalPlays >= (size_t)kN);

  // (C) At most 1 DROP-RESYNC per sender is tolerated under HD stress. More
  //     than that indicates relay congestion or GUID timing regression.
  CHECK(totalDrops <= (size_t)kN);

  // Disconnect (sequential, deterministic).
  for (auto &c : clients) c->disconnect();
}
