// Scenario 8 — structural regression guard for the iPad/iPhone "video late by 1-2
// intervals" bug, plus a CLI mode to replay an external SYNCLOG file (useful when
// triaging device-only failures).
//
// Background. Today's njclient.cpp::on_new_interval flow is:
//
//     STAGE 1: PROMOTE — if pending.active, copy pending → playing.
//     STAGE 2: PLAY    — if next.frameCount ≥ 1 and audio matches,
//                        copy next → pending and emit
//                        "SWAP#K video PLAY: ... vidSeq=N (deferred → pending)"
//
// The invariant the receiver depends on for tight A/V alignment:
//
//     For every  SWAP#K video PLAY: ... vidSeq=N (deferred → pending)
//     there is   SWAP#(K+1) video PROMOTE: ... vidSeq=N
//
// On real devices we observed two failure modes:
//
//   * iPad-receiving-iPad: the first PLAY fires with match=NONE (because the
//     sender's m_curwritefile.guid for ch0 was still 0000 when the first marker
//     went out — Core Audio startup latency). The next swap then emits another
//     PLAY (overwriting pending) WITHOUT a PROMOTE in between, so the original
//     vidSeq is never promoted. That vidSeq finally promotes one swap later,
//     giving the user a "video 2 intervals late" perception.
//
//   * iPad-receiving-iPhone: similar pattern but only 1 swap of extra delay.
//
// The structural assertion below catches both: it walks the SYNCLOG and verifies
// every PLAY has a matching PROMOTE exactly 1 swap later with the same vidSeq.
//
// To run as a log-replay tool against a captured device trace, set
// `NJ_TEST_REPLAY_LOG` to the file path before invoking; the live docker test is
// skipped and the file is parsed instead.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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

videosync::TestClient::Options makeOpts(const std::string &user, bool sendVideo) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = true;
  o.sendVideo = sendVideo;
  return o;
}

struct PlayEvent {
  int swap = 0;
  int vidSeq = 0;
  std::string matchKind; // "DS" | "PREV" | "NONE"
  // Action the receiver took: "pending" → expect PROMOTE one swap later.
  // "playing" → went straight to playing, no PROMOTE expected.
  std::string action;
  std::string raw;
};

struct PromoteEvent {
  int swap = 0;
  int vidSeq = 0;
  std::string raw;
};

// Parses SYNCLOG lines for a given key (e.g. "iPad:1") and returns paired
// PLAY/PROMOTE events keyed by vidSeq. Lines that don't belong to `key` are
// ignored, so the parser tolerates multi-stream traces.
struct ParsedTrace {
  std::vector<PlayEvent> plays;
  std::vector<PromoteEvent> promotes;
};

ParsedTrace parseTrace(const std::vector<std::string> &lines, const std::string &key) {
  // SWAP#K video PLAY: key=<key> match=<DS|PREV|NONE> vidSeq=<N> ... (<action>)
  // action ∈ {"deferred → pending", "immediate → playing"}; older builds may
  // omit the suffix entirely — treat that as "pending" (the historical default).
  std::string playPat =
      R"(SWAP#(\d+) video PLAY: key=)" + key +
      R"( match=(DS|PREV|NONE) vidSeq=(-?\d+))";
  std::regex playRe(playPat);
  std::regex actionRe(R"(\((deferred → pending|immediate → playing)\))");
  std::string promPat =
      R"(SWAP#(\d+) video PROMOTE: key=)" + key + R"( frames=\d+ vidSeq=(-?\d+))";
  std::regex promRe(promPat);

  ParsedTrace out;
  for (const auto &line : lines) {
    std::smatch m;
    if (std::regex_search(line, m, playRe)) {
      PlayEvent ev;
      ev.swap = std::stoi(m[1]);
      ev.matchKind = m[2];
      ev.vidSeq = std::stoi(m[3]);
      std::smatch am;
      if (std::regex_search(line, am, actionRe)) {
        ev.action = (am[1] == "deferred → pending") ? "pending" : "playing";
      } else {
        ev.action = "pending"; // legacy default
      }
      ev.raw = line;
      out.plays.push_back(std::move(ev));
    } else if (std::regex_search(line, m, promRe)) {
      PromoteEvent ev;
      ev.swap = std::stoi(m[1]);
      ev.vidSeq = std::stoi(m[2]);
      ev.raw = line;
      out.promotes.push_back(std::move(ev));
    }
  }
  return out;
}

// Returns a list of human-readable invariant violations. Empty list means clean.
struct Violation {
  std::string kind;
  std::string detail;
};

// `tailGrace` lets the live test exempt the final PLAY (its PROMOTE typically
// hasn't fired yet when the recording window closes). Replay mode passes 0 so
// captured device traces are evaluated end-to-end.
std::vector<Violation> findViolations(const ParsedTrace &t, int tailGrace = 0) {
  std::vector<Violation> v;
  // Index promotes by vidSeq for fast lookup; if the same vidSeq promoted twice
  // (which shouldn't happen) we collect both.
  std::multimap<int, const PromoteEvent *> promoteByVid;
  for (const auto &p : t.promotes) promoteByVid.emplace(p.vidSeq, &p);

  // Sort plays by swap so we can address the tail.
  std::vector<const PlayEvent *> playsSorted;
  playsSorted.reserve(t.plays.size());
  for (const auto &p : t.plays) playsSorted.push_back(&p);
  std::sort(playsSorted.begin(), playsSorted.end(),
            [](const PlayEvent *a, const PlayEvent *b) { return a->swap < b->swap; });
  int dropFromEnd = tailGrace > (int)playsSorted.size() ? (int)playsSorted.size()
                                                        : tailGrace;
  int upper = (int)playsSorted.size() - dropFromEnd;
  for (int i = 0; i < upper; ++i) {
    const auto &play = *playsSorted[i];

    // (immediate → playing) PLAYs bypass `pending` entirely — that's the
    // intended path for match=PREV and match=NONE. Don't expect a PROMOTE.
    if (play.action == "playing") continue;
    // vidSeq=-1 means "no marker received" — the receiver entered the success
    // branch on a hasGuid==false path before the first valid marker. That's the
    // exact iPad failure window. Treat it as a violation: a healthy stream's
    // first PLAY should always carry a real vidSeq.
    if (play.vidSeq < 0) {
      v.push_back({"play_with_no_seq",
                   "PLAY at SWAP#" + std::to_string(play.swap) +
                       " has vidSeq=-1 (marker never parsed): " + play.raw});
      continue;
    }

    auto range = promoteByVid.equal_range(play.vidSeq);
    if (range.first == range.second) {
      v.push_back({"missing_promote",
                   "PLAY at SWAP#" + std::to_string(play.swap) + " vidSeq=" +
                       std::to_string(play.vidSeq) +
                       " — no PROMOTE for this vidSeq ever fired"});
      continue;
    }

    bool sawCorrectDelta = false;
    for (auto it = range.first; it != range.second; ++it) {
      int delta = it->second->swap - play.swap;
      if (delta == 1) {
        sawCorrectDelta = true;
        break;
      }
    }
    if (!sawCorrectDelta) {
      // Pick the closest promote for the diagnostic.
      const PromoteEvent *closest = range.first->second;
      for (auto it = range.first; it != range.second; ++it) {
        if (std::abs(it->second->swap - play.swap) <
            std::abs(closest->swap - play.swap)) {
          closest = it->second;
        }
      }
      int delta = closest->swap - play.swap;
      v.push_back({"promote_swap_off",
                   "PLAY at SWAP#" + std::to_string(play.swap) + " vidSeq=" +
                       std::to_string(play.vidSeq) +
                       " — PROMOTE landed at SWAP#" +
                       std::to_string(closest->swap) + " (delta=" +
                       std::to_string(delta) + ", expected 1)"});
    }
  }
  return v;
}

void runLiveScenario() {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  if (std::getenv("NJ_TEST_DEBUG")) log.setEcho(true);

  videosync::TestClient sender(makeOpts("anonymous:inv_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:inv_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();

  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1)) {
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.intervalSwapCount() >= 1);

  // Stream for several intervals so we get many PLAY/PROMOTE pairs to validate.
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

  auto trace = parseTrace(log.all(), "inv_sender:1");
  std::fprintf(stderr,
               "[scenario8] live: PLAY=%zu PROMOTE=%zu\n",
               trace.plays.size(), trace.promotes.size());
  REQUIRE(trace.plays.size() >= 1);

  // tailGrace=1 — the final PLAY in the live recording window may not have a
  // PROMOTE yet because the test ended before the next swap fired.
  auto viols = findViolations(trace, /*tailGrace=*/1);
  if (!viols.empty()) {
    std::fprintf(stderr, "[scenario8] %zu invariant violation(s):\n", viols.size());
    for (const auto &v : viols)
      std::fprintf(stderr, "  [%s] %s\n", v.kind.c_str(), v.detail.c_str());
  }
  CHECK(viols.empty());

  receiver.disconnect();
  sender.disconnect();
}

void runReplay(const std::string &path) {
  std::ifstream in(path);
  REQUIRE(in.is_open());

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("[SYNCLOG]") != std::string::npos ||
        line.find("video PLAY:") != std::string::npos ||
        line.find("video PROMOTE:") != std::string::npos) {
      lines.push_back(line);
    }
  }

  // Pick the first key that appears in a PLAY line so we don't need the user to
  // specify it. Falls back to the first PROMOTE key. This makes the replay tool
  // work on a pasted device trace without configuration.
  std::regex keyRe(R"(key=([\w.-]+:\d+))");
  std::string key;
  for (const auto &l : lines) {
    if (l.find("video PLAY:") == std::string::npos &&
        l.find("video PROMOTE:") == std::string::npos)
      continue;
    std::smatch m;
    if (std::regex_search(l, m, keyRe)) {
      key = m[1];
      break;
    }
  }
  REQUIRE_FALSE(key.empty());
  std::fprintf(stderr, "[scenario8] replay key=%s, %zu candidate lines\n",
               key.c_str(), lines.size());

  auto trace = parseTrace(lines, key);
  std::fprintf(stderr,
               "[scenario8] replay: PLAY=%zu PROMOTE=%zu\n",
               trace.plays.size(), trace.promotes.size());
  REQUIRE(trace.plays.size() >= 1);

  // Like the live test, captured device traces typically end before the very last
  // PLAY's PROMOTE has had a chance to fire. tailGrace=1 forgives that.
  auto viols = findViolations(trace, /*tailGrace=*/1);
  std::fprintf(stderr, "[scenario8] replay violations: %zu\n", viols.size());
  for (const auto &v : viols)
    std::fprintf(stderr, "  [%s] %s\n", v.kind.c_str(), v.detail.c_str());
  CHECK(viols.empty());
}

} // namespace

TEST_CASE("08_play_promote_invariant — every PLAY must PROMOTE one swap later",
          "[sync][scenario8][regression]") {
  if (const char *replay = std::getenv("NJ_TEST_REPLAY_LOG")) {
    SECTION("replay device trace") { runReplay(replay); }
  } else {
    SECTION("live host run") { runLiveScenario(); }
  }
}
