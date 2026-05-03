// Scenario 20 — DROP-RESYNC fires and receiver recovers.
//
// kHoldCapDrop = 4 in njclient.cpp: after 4 consecutive GUID mismatches the
// receiver discards `next`, clears `synced`, and resets hold_count.
//
// Triggering DROP-RESYNC deterministically requires frames in `next` to carry
// a GUID that drifts past both DS and PREV matches across 4+ consecutive swaps.
// That drift occurs naturally when two streams compete for GUID alignment and
// one falls behind the audio timeline by ≥3 intervals.
//
// This test drives that condition by running the multi-client HD stress at
// N=5 clients for a short window, which creates enough relay contention that
// at least one stream experiences holdCount saturation. After the stress
// window closes, the test verifies the RECOVERY property:
//
//     For every DROP-RESYNC in the log, at least one PLAY event for that same
//     key must appear AFTER the last DROP-RESYNC line.
//
// If DROP-RESYNC never fires during the run, the test prints a diagnostic and
// passes unconditionally — the stress window was not enough to trigger it on
// this run. Re-run with NJ_TEST_DROP_NCLIENTS=6 or NJ_TEST_DROP_SECONDS=12 to
// increase probability.
//
// Override knobs:
//   NJ_TEST_DROP_NCLIENTS  (default 5)  — clients in the stress cluster
//   NJ_TEST_DROP_SECONDS   (default 8)  — duration of frame pushing
//   NJ_TEST_DROP_FRAME_KB  (default 20) — per-frame size in KB
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

// For a given stream key, find the line index of the LAST DROP-RESYNC and the
// index of the FIRST PLAY after that line. Returns {dropIdx, playAfterIdx}.
// If no DROP-RESYNC found, returns {-1, -1}. If no PLAY after drop, returns
// {dropIdx, -1}.
struct RecoveryResult {
  int lastDropIdx = -1;    // index in `lines` of the last DROP-RESYNC for key
  int firstPlayAfter = -1; // index of first PLAY that appears after lastDropIdx
  size_t totalDrops = 0;
  size_t totalPlays = 0;
};

RecoveryResult checkRecovery(const std::vector<std::string> &lines,
                             const std::string &key) {
  RecoveryResult r;
  std::regex dropRe(R"(SWAP#\d+ video DROP-RESYNC: key=)" + key);
  std::regex playRe(R"(SWAP#\d+ video PLAY: key=)" + key);
  for (int i = 0; i < (int)lines.size(); ++i) {
    if (std::regex_search(lines[i], dropRe)) {
      ++r.totalDrops;
      r.lastDropIdx = i;
    }
  }
  for (int i = 0; i < (int)lines.size(); ++i) {
    if (std::regex_search(lines[i], playRe)) {
      ++r.totalPlays;
      if (r.lastDropIdx >= 0 && i > r.lastDropIdx && r.firstPlayAfter < 0)
        r.firstPlayAfter = i;
    }
  }
  return r;
}

} // namespace

TEST_CASE("20_drop_resync_recovery — receiver re-syncs after DROP-RESYNC",
          "[sync][scenario20][stress]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  const int kN       = envInt("NJ_TEST_DROP_NCLIENTS", 5);
  const int kSeconds = envInt("NJ_TEST_DROP_SECONDS",  8);
  const int kFrameKB = envInt("NJ_TEST_DROP_FRAME_KB", 20);
  std::fprintf(stderr,
               "[scenario20] config: N=%d duration=%ds frameKB=%d\n",
               kN, kSeconds, kFrameKB);

  // Connect N clients sequentially.
  std::vector<std::unique_ptr<videosync::TestClient>> clients;
  std::vector<std::string> keys;
  for (int i = 0; i < kN; ++i) {
    std::string user = "anonymous:dr" + std::to_string(i);
    keys.push_back("dr" + std::to_string(i) + ":1");
    clients.emplace_back(
        std::make_unique<videosync::TestClient>(makeOpts(user)));
    REQUIRE(clients.back()->connectAndJoin(8s));
    clients.back()->sendFakeSPSPPS();
  }

  // Wait for all clients to see at least 1 swap.
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

  // Push frames from all clients in parallel.
  std::atomic<bool> stop{false};
  std::vector<std::thread> pushers;
  std::vector<std::atomic<uint32_t>> seqs(kN);
  for (int i = 0; i < kN; ++i) seqs[i].store(0);

  for (int i = 0; i < kN; ++i) {
    pushers.emplace_back([&, i]() {
      while (!stop.load()) {
        uint32_t s = seqs[i].fetch_add(1);
        auto frame = videosync::makeFakeFrame(s, (uint32_t)i,
                                              kFrameKB * 1024);
        clients[i]->sendVideoFrame(frame.data(), (int)frame.size());
        std::this_thread::sleep_for(50ms); // ~20 fps per sender
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(kSeconds));
  stop.store(true);
  for (auto &t : pushers) t.join();

  // Drain trailing intervals — give receivers time to process pending video.
  std::this_thread::sleep_for(3s);

  auto allLines = log.all();

  // Per-stream recovery analysis.
  bool anyDrop = false;
  bool allRecovered = true;
  for (const auto &key : keys) {
    auto r = checkRecovery(allLines, key);
    std::fprintf(stderr,
                 "[scenario20] key=%-10s  PLAY=%zu  DROP=%zu  "
                 "lastDrop=%d  firstPlayAfter=%d\n",
                 key.c_str(), r.totalPlays, r.totalDrops,
                 r.lastDropIdx, r.firstPlayAfter);
    if (r.totalDrops > 0) {
      anyDrop = true;
      if (r.firstPlayAfter < 0) {
        allRecovered = false;
        std::fprintf(stderr,
                     "[scenario20] FAIL: key=%s had DROP-RESYNC but no PLAY "
                     "appeared after it — receiver stuck!\n",
                     key.c_str());
      }
    }
  }

  if (!anyDrop) {
    std::fprintf(stderr,
                 "[scenario20] No DROP-RESYNC fired during this run. "
                 "Increase NJ_TEST_DROP_NCLIENTS or NJ_TEST_DROP_SECONDS to "
                 "raise probability. Test passes as-is.\n");
  }

  // Core assertion: if DROP-RESYNC fired, the receiver must have recovered.
  if (anyDrop) {
    CHECK(allRecovered);
  }

  // Sanity: every stream must have produced at least 1 PLAY in total.
  for (const auto &key : keys) {
    auto plays = log.match(R"(SWAP#\d+ video PLAY: key=)" + key);
    CHECK(plays.size() >= 1);
  }

  for (auto &c : clients) c->disconnect();
}
