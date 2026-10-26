// Scenario 34 — video re-syncs after the sender turns voice chat off.
//
// The sender has audio on ch0 and ch1 (interval mode) and video anchored to ch0's
// interval GUID. While any of its audio channels is in NINJAM voice-chat mode
// (flags & 2) the receiver plays its video on the live path (frame on arrival +
// VIDEO_LIVE_DELAY_MS); otherwise on the interval path (marker GUID matched to the
// audio at each swap). Turning voice chat on or off may cost one transition interval;
// after that the video must be matched to the playing audio every interval, with
// every frame delivered once, in order, paced across its interval (no bursts).
//
// Reported on Macs with a poor connection: after voice chat was turned off the
// video stayed out of sync and never recovered; around the toggle the app received
// two bursts of ~300 frames in 5 s and ~120 frame indices were skipped.
//
// 34a: voice chat ON for 4 intervals, then OFF.
// 34b: OFF -> ON -> OFF with audio stalls on both ends (unstable connection).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "catch_amalgamated.hpp"

#include "FakeFrame.h"
#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"
#include "ToneTag.h"

using namespace std::chrono_literals;

namespace {

// test.cfg: 240 BPM / 4 BPI = 1 s; NJ_TEST_INTERVAL_MS for a server with real intervals.
const int kIntervalMs = std::getenv("NJ_TEST_INTERVAL_MS") ? std::atoi(std::getenv("NJ_TEST_INTERVAL_MS")) : 1000;

// 34e/34f: the sender's ch0 is silent and its music is on ch1 (talkback + drums).
bool gSilentCh0 = false;
bool gWholeRun = false; // 36: content report for every interval from the first OFF
bool gCh0Stereo = false;
float gCh0Gain = 1.f; // sender ch0 tone level relative to ch1 (a mic that is mostly quiet)
int gRecvPhaseMs = kIntervalMs / 2; // how far the receiver's swap lags the sender's

videosync::TestClient::Options makeOpts(const std::string &user, bool sender) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = true;
  o.sendVideo = sender;
  o.toneInput = sender;
  o.recordOutput = !sender;
  if (sender) {
    o.extraAudioChannel = 1; // ch1: interval-mode audio next to the voice-chat ch0
    o.videoChannel = 2;
    o.ch0Gain = gCh0Gain;
    o.ch0Stereo = gCh0Stereo;
    if (gSilentCh0) {
      o.silentCh0 = true;
      // A loud channel reaches the encoder's first-block threshold early in the
      // interval and a silent one never does; at 1 s intervals that needs a
      // higher bitrate than the default to show the same arrival order.
      o.audioBitrate = 192;
    }
  }
  return o;
}

struct Pushed {
  uint32_t seq;
  int tag; // sender swap count when the frame was queued
};

struct Step {
  int atMs;           // from the start of streaming
  int voiceChat;      // -1 = unchanged, 0 = off, 1 = on, 2 = mark only (control run)
  int stallSenderMs;  // 0 = none
  int stallRecvMs;    // 0 = none
  int netSenderMs = 0; // network (Run thread) stall on the sender
  int netRecvMs = 0;   // network (Run thread) stall on the receiver
  // Tell the server about the flag change only after the sender's next interval
  // boundary + 200 ms: the first interval-mode upload of ch0 (BEGIN) reaches the
  // receiver before the user-info change that turns ch0's live mode off.
  bool notifyAfterBoundary = false;
  int subscribeCh1 = -1; // receiver: 0 = unsubscribe sender ch1, 1 = subscribe again
};

struct RunResult {
  std::vector<Pushed> pushed;
  std::vector<videosync::VideoFrameRecord> delivered;
  int offTag = -1;       // sender swap at the last voice-chat OFF
  int firstOffTag = -1;  // sender swap at the first voice-chat OFF
  int lastEventTag = -1; // sender swap when the last toggle/stall is over
};

RunResult runPlan(videosync::TestClient &sender, videosync::TestClient &receiver,
                  const std::vector<Step> &plan, int totalMs, const std::string &senderName) {
  RunResult r;
  uint32_t seq = 0;
  size_t next = 0;
  int pendingNotifySwap = -1;
  std::chrono::steady_clock::time_point pendingNotifyAt{};
  const auto t0 = std::chrono::steady_clock::now();
  const auto end = t0 + std::chrono::milliseconds(totalMs);
  while (std::chrono::steady_clock::now() < end) {
    const int nowMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
    while (next < plan.size() && nowMs >= plan[next].atMs) {
      const Step &s = plan[next++];
      if (s.voiceChat == 2) {
        r.offTag = sender.currentSwap();
        std::fprintf(stderr, "[scenario34] t=%dms mark at sender swap %d\n", nowMs, r.offTag);
      } else if (s.voiceChat >= 0) {
        sender.setVoiceChat(0, s.voiceChat == 1, !s.notifyAfterBoundary);
        if (s.notifyAfterBoundary) pendingNotifySwap = sender.currentSwap();
        std::fprintf(stderr, "[scenario34] t=%dms voice chat %s at sender swap %d%s\n",
                     nowMs, s.voiceChat ? "ON" : "OFF", sender.currentSwap(),
                     s.notifyAfterBoundary ? " (server told after the next boundary)" : "");
        if (s.voiceChat == 0) { r.offTag = sender.currentSwap(); if (r.firstOffTag < 0) r.firstOffTag = r.offTag; }
      }
      if (s.stallSenderMs > 0) sender.stallAudio(s.stallSenderMs);
      if (s.stallRecvMs > 0) receiver.stallAudio(s.stallRecvMs);
      if (s.netSenderMs > 0) sender.stallNetwork(s.netSenderMs);
      if (s.netRecvMs > 0) receiver.stallNetwork(s.netRecvMs);
      if (s.subscribeCh1 >= 0) {
        receiver.subscribeRemoteChannel(senderName, 1, s.subscribeCh1 == 1);
        std::fprintf(stderr, "[scenario34] t=%dms receiver %ssubscribes ch1 at sender swap %d\n",
                     nowMs, s.subscribeCh1 ? "re-" : "un", sender.currentSwap());
      }
      const int longest = std::max({s.stallSenderMs, s.stallRecvMs, s.netSenderMs, s.netRecvMs});
      r.lastEventTag = std::max(r.lastEventTag, sender.currentSwap() +
                                (longest + kIntervalMs - 1) / kIntervalMs +
                                (s.notifyAfterBoundary ? 1 : 0));
    }
    if (pendingNotifySwap >= 0 && sender.currentSwap() > pendingNotifySwap) {
      if (pendingNotifyAt == std::chrono::steady_clock::time_point{})
        pendingNotifyAt = std::chrono::steady_clock::now() + 200ms;
      else if (std::chrono::steady_clock::now() >= pendingNotifyAt) {
        sender.notifyChannels();
        std::fprintf(stderr, "[scenario34] server told of the flag change at sender swap %d\n",
                     sender.currentSwap());
        pendingNotifySwap = -1;
        pendingNotifyAt = {};
      }
    }
    const int tag = sender.currentSwap();
    auto f = videosync::makeFakeFrame(seq, (uint32_t)tag, 256);
    sender.sendVideoFrame(f.data(), (int)f.size());
    r.pushed.push_back({seq, tag});
    ++seq;
    std::this_thread::sleep_for(80ms);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(3 * kIntervalMs));
  r.delivered = receiver.drainVideoFrames();
  return r;
}

// Checks the intervals recorded from `firstTag` to `lastTag` (sender swaps).
// Interval video arrives paced across its interval even right after a stall, when
// intervals reach the receiver late, early or through the awaiting path: at most 4
// frames inside any 150 ms window (13 frames per 1 s interval) from `firstTag` on.
void checkPacing(const RunResult &r, int firstTag, int lastTag, const char *name) {
  std::vector<uint64_t> us;
  for (const auto &f : r.delivered) {
    videosync::FakeFramePayload p;
    if (!videosync::parseFakeFrame(f.data.data(), (int)f.data.size(), p)) continue;
    if ((int)p.intervalIdx >= firstTag && (int)p.intervalIdx <= lastTag) us.push_back(f.deliverTimestampUs);
  }
  std::sort(us.begin(), us.end());
  int maxIn150 = 0;
  for (size_t i = 0, j = 0; i < us.size(); ++i) {
    while (us[i] - us[j] > 150000) ++j;
    maxIn150 = std::max(maxIn150, (int)(i - j + 1));
  }
  std::fprintf(stderr, "[%s] pacing tags %d..%d: frames=%zu maxFramesIn150ms=%d\n", name, firstTag,
               lastTag, us.size(), maxIn150);
  CHECK(maxIn150 <= 4);
}

void checkSettled(const RunResult &r, int firstTag, int lastTag, const char *name) {
  std::map<int, std::vector<uint32_t>> pushedByTag;
  for (const auto &p : r.pushed)
    if (p.tag >= firstTag && p.tag <= lastTag) pushedByTag[p.tag].push_back(p.seq);

  struct D { uint32_t seq; int tag; int rswap; uint64_t us; };
  std::vector<D> got;
  const bool dump = std::getenv("NJ_TEST_DUMP_FRAMES") != nullptr;
  for (const auto &f : r.delivered) {
    videosync::FakeFramePayload p;
    if (!videosync::parseFakeFrame(f.data.data(), (int)f.data.size(), p)) continue;
    got.push_back({p.seq, (int)p.intervalIdx, f.receivedAtSwap, f.deliverTimestampUs});
    if (dump)
      std::fprintf(stderr, "[%s]   frame seq=%u tag=%d recvSwap=%d idx=%d/%d t=%llu\n", name,
                   p.seq, (int)p.intervalIdx, f.receivedAtSwap, f.frameIndex, f.totalFrames,
                   (unsigned long long)((f.deliverTimestampUs / 1000) % 100000));
  }

  // 1) every frame of the settled intervals delivered exactly once
  std::map<uint32_t, int> times;
  for (const auto &d : got) times[d.seq]++;
  int missing = 0, dup = 0, total = 0;
  for (const auto &kv : pushedByTag)
    for (uint32_t s : kv.second) {
      ++total;
      if (times[s] == 0) {
        ++missing;
        std::fprintf(stderr, "[%s]   missing seq %u (tag %d)\n", name, s, kv.first);
      }
      if (times[s] > 1) ++dup;
    }

  // 2) in order; 3) each interval in one receiver swap, at one fixed offset;
  // 4) paced: no more than 4 frames inside any 150 ms window
  int reorder = 0;
  uint32_t lastSeq = 0;
  bool haveLast = false;
  std::map<int, std::set<int>> rswapsByTag;
  std::vector<uint64_t> settledUs;
  for (const auto &d : got) {
    if (d.tag < firstTag || d.tag > lastTag) continue;
    if (haveLast && d.seq <= lastSeq) ++reorder;
    lastSeq = d.seq;
    haveLast = true;
    rswapsByTag[d.tag].insert(d.rswap);
    settledUs.push_back(d.us);
  }
  std::set<int> offsets;
  int splitTags = 0;
  for (const auto &kv : rswapsByTag) {
    if (kv.second.size() > 1) ++splitTags;
    for (int rs : kv.second) offsets.insert(rs - kv.first);
  }
  std::sort(settledUs.begin(), settledUs.end());
  int maxIn150 = 0;
  for (size_t i = 0, j = 0; i < settledUs.size(); ++i) {
    while (settledUs[i] - settledUs[j] > 150000) ++j;
    maxIn150 = std::max(maxIn150, (int)(i - j + 1));
  }
  // Where the densest delivery happened (seq/tag/recv swap of each frame in it).
  {
    std::vector<D> st;
    for (const auto &d : got) if (d.tag >= firstTag && d.tag <= lastTag) st.push_back(d);
    std::sort(st.begin(), st.end(), [](const D &a, const D &b) { return a.us < b.us; });
    for (size_t i = 0, j = 0; i < st.size(); ++i) {
      while (st[i].us - st[j].us > 150000) ++j;
      if ((int)(i - j + 1) == maxIn150 && maxIn150 > 4) {
        for (size_t k = j; k <= i; ++k)
          std::fprintf(stderr, "[%s]   burst: seq %u tag %d recvSwap %d t=%llums\n", name,
                       st[k].seq, st[k].tag, st[k].rswap,
                       (unsigned long long)((st[k].us - st[j].us) / 1000));
        break;
      }
    }
  }

  std::fprintf(stderr,
               "[%s] settled tags %d..%d: pushed=%d missing=%d dup=%d reorder=%d "
               "splitTags=%d offsets=%zu maxFramesIn150ms=%d\n",
               name, firstTag, lastTag, total, missing, dup, reorder, splitTags,
               offsets.size(), maxIn150);
  for (const auto &kv : rswapsByTag) {
    std::fprintf(stderr, "[%s]   tag %d -> recv swaps", name, kv.first);
    for (int rs : kv.second) std::fprintf(stderr, " %d", rs);
    std::fprintf(stderr, "\n");
  }

  CHECK(total > 0);
  CHECK(missing == 0);
  CHECK(dup == 0);
  CHECK(reorder == 0);
  CHECK(splitTags == 0);
  CHECK(offsets.size() == 1);
  CHECK(maxIn150 <= 4);
}

// After the settle point every swap must play the next interval matched to its
// audio: no HOLD / DROP / FALLBACK / unmatched (NONE) plays, no live frames.
void checkSyncLogAfter(const std::vector<std::string> &lines, int fromRecvSwap,
                       const std::string &key, const char *name) {
  int plays = 0, bad = 0;
  for (const auto &l : lines) {
    int sw = -1;
    const size_t at = l.find("SWAP#");
    if (at == std::string::npos || std::sscanf(l.c_str() + at, "SWAP#%d", &sw) != 1 ||
        sw < fromRecvSwap) continue;
    if (l.find("key=" + key) == std::string::npos) continue;
    if (l.find("video PLAY:") != std::string::npos) {
      if (l.find("match=NONE") != std::string::npos) { ++bad; std::fprintf(stderr, "[%s] BAD %s\n", name, l.c_str()); }
      else ++plays;
    } else if (l.find("video HOLD") != std::string::npos ||
               l.find("DROP") != std::string::npos ||
               l.find("FALLBACK") != std::string::npos ||
               l.find("BURST") != std::string::npos) {
      ++bad;
      std::fprintf(stderr, "[%s] BAD %s\n", name, l.c_str());
    }
  }
  std::fprintf(stderr, "[%s] from recv swap %d: matched PLAY=%d bad=%d\n", name, fromRecvSwap, plays, bad);
  CHECK(plays >= 4);
  CHECK(bad == 0);
}

// Content check: at the output sample where each video frame is shown, the
// receiver's left output (sender ch0, panned left) and right output (ch1, panned
// right) must carry the tone of the sender interval the frame was captured in.
void checkContent(videosync::TestClient &receiver, const RunResult &r, int firstTag,
                  int lastTag, const char *name) {
  std::vector<float> L, R;
  receiver.outputSnapshot(L, R);
  const int kWin = 2048;
  int checked = 0, vidCh0 = 0, vidCh1 = 0, ch0Ch1 = 0, silent = 0, printed = 0;
  std::map<int, int> offCh0, offCh1; // (audio tag - video tag) mod 16, signed
  std::map<int, std::map<std::string, int>> perTag; // video interval -> audio offsets (99 = no tone)
  auto signedOff = [](int a, int v) {
    int d = ((a - v) % videosync::kToneTags + videosync::kToneTags) % videosync::kToneTags;
    return d >= videosync::kToneTags / 2 ? d - videosync::kToneTags : d;
  };
  for (const auto &f : r.delivered) {
    videosync::FakeFramePayload p;
    if (!videosync::parseFakeFrame(f.data.data(), (int)f.data.size(), p)) continue;
    const int tag = (int)p.intervalIdx;
    if (tag < firstTag || tag > lastTag || f.outputSample < 0 ||
        f.outputSample + kWin > (int64_t)L.size()) continue;
    const int a0 = videosync::detectToneTag(L.data() + f.outputSample, kWin, 48000);
    const int a1 = videosync::detectToneTag(R.data() + f.outputSample, kWin, 48000);
    const int v = tag % videosync::kToneTags;
    ++checked;
    if ((a0 < 0 && !gSilentCh0) || a1 < 0) ++silent;
    if (a0 >= 0) offCh0[signedOff(a0, v)]++;
    if (a1 >= 0) offCh1[signedOff(a1, v)]++;
    {
      char k[48];
      std::snprintf(k, sizeof k, "ch0%+d ch1%+d", a0 < 0 ? 99 : signedOff(a0, v), a1 < 0 ? 99 : signedOff(a1, v));
      perTag[tag][k]++;
    }
    const bool bad0 = !gSilentCh0 && a0 != v, bad1 = a1 != v, bad01 = !gSilentCh0 && a0 != a1;
    vidCh0 += bad0;
    vidCh1 += bad1;
    ch0Ch1 += bad01;
    if ((bad0 || bad1) && printed++ < 12)
      std::fprintf(stderr, "[%s]   CONTENT seq=%u videoInterval=%d recvSwap=%d ch0=%s ch1=%s\n",
                   name, p.seq, tag, f.receivedAtSwap,
                   a0 < 0 ? "none" : std::to_string(tag + signedOff(a0, v)).c_str(),
                   a1 < 0 ? "none" : std::to_string(tag + signedOff(a1, v)).c_str());
  }
  if (std::getenv("NJ_REAL_PER_TAG")) {
    for (const auto &kv : perTag) {
      std::fprintf(stderr, "[%s]   PERTAG video=%d:", name, kv.first);
      for (const auto &c : kv.second) std::fprintf(stderr, " [%s]x%d", c.first.c_str(), c.second);
      std::fprintf(stderr, "\n");
    }
  }
  std::fprintf(stderr, "[%s] content tags %d..%d: frames=%d video!=ch0:%d video!=ch1:%d ch0!=ch1:%d silent:%d\n",
               name, firstTag, lastTag, checked, vidCh0, vidCh1, ch0Ch1, silent);
  for (int c = 0; c < 2; ++c) {
    std::fprintf(stderr, "[%s]   ch%d audio interval - video interval:", name, c);
    for (const auto &kv : (c ? offCh1 : offCh0)) std::fprintf(stderr, " %+d x%d", kv.first, kv.second);
    std::fprintf(stderr, "\n");
  }
  CHECK(checked >= 30);
  CHECK(vidCh0 == 0);
  CHECK(vidCh1 == 0);
  CHECK(ch0Ch1 == 0);
}

void scanOutput(videosync::TestClient &receiver, int64_t from, std::vector<int> &l,
                std::vector<int> &r);

void waitForSwaps(videosync::TestClient &a, videosync::TestClient &b) {
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         (a.intervalSwapCount() < 1 || b.intervalSwapCount() < 1))
    std::this_thread::sleep_for(20ms);
}

void runCase(const char *name, const std::string &prefix, const std::vector<Step> &plan,
             int totalMs) {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient sender(makeOpts("anonymous:" + prefix + "_sender", true));
  videosync::TestClient receiver(makeOpts("anonymous:" + prefix + "_recv", false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();
  waitForSwaps(sender, receiver);
  REQUIRE(receiver.intervalSwapCount() >= 1);
  // On localhost both clients swap within a few ms of each other, so the video
  // BEGIN races the receiver's swap. Put the receiver half an interval behind,
  // as a real link would place it somewhere, so the baseline is deterministic.
  receiver.stallAudio(gRecvPhaseMs);
  std::this_thread::sleep_for(std::chrono::milliseconds(2 * kIntervalMs));
  // Sender ch0 to the left output, ch1 to the right, for the content check.
  for (int i = 0; i < 300 && !receiver.panRemoteChannel(prefix + "_sender", 0, -1.f); ++i)
    std::this_thread::sleep_for(100ms);
  REQUIRE(receiver.panRemoteChannel(prefix + "_sender", 0, -1.f));
  REQUIRE(receiver.panRemoteChannel(prefix + "_sender", 1, 1.f));

  const int senderSwap0 = sender.currentSwap();
  const int recvSwap0 = receiver.currentSwap();
  RunResult r = runPlan(sender, receiver, plan, totalMs, prefix + "_sender");
  REQUIRE(r.offTag >= 0);
  const int lastTag = r.pushed.back().tag - 1; // the last interval may still be in flight
  // Settled: from the second interval after the toggle (the one it lands in, then
  // one transition interval), or after the last stall, whichever comes later.
  const int firstTag = gWholeRun ? r.firstOffTag : std::max(r.offTag, r.lastEventTag) + 2;
  if (gWholeRun) { checkContent(receiver, r, firstTag, lastTag, name); return; }
  REQUIRE(lastTag - firstTag >= 4);
  checkSettled(r, firstTag, lastTag, name);
  checkPacing(r, r.offTag + 2, lastTag, name);
  // Receiver swap where the first settled interval plays (swap counters started
  // within one interval of each other; allow the 1-interval audio delay + 1).
  const int fromRecvSwap = recvSwap0 + (firstTag - senderSwap0) + 2;
  const auto lines = log.all();
  checkSyncLogAfter(lines, fromRecvSwap, prefix + "_sender:2", name);
  checkContent(receiver, r, firstTag, lastTag, name);
  // How long ch1 (right output) was silent during the run (receive switch cases).
  bool resub = false;
  for (const auto &st : plan) resub |= st.subscribeCh1 >= 0;
  if (resub) {
    std::vector<int> l, rr;
    scanOutput(receiver, 0, l, rr);
    int silentR = 0;
    for (size_t i = l.size() / 3; i < rr.size(); ++i) silentR += rr[i] < 0;
    std::fprintf(stderr, "[%s] ch1 silent for ~%d ms after the first third of the run\n", name, silentR * 50);
  }
}

} // namespace

TEST_CASE("34a_voice_chat_toggle — video re-syncs after voice chat off",
          "[sync][scenario34]") {
  // 3 s interval mode, 4 s voice chat, then 9 s interval mode.
  runCase("scenario34a", "vc34a",
          {{3000, 1, 0, 0}, {7000, 0, 0, 0}}, 16000);
}

TEST_CASE("34c_voice_chat_off_begin_first — ch0's first interval BEGIN before the flag change",
          "[sync][scenario34]") {
  Step off{7000, 0, 0, 0};
  off.notifyAfterBoundary = true;
  runCase("scenario34c", "vc34c", {{3000, 1, 0, 0}, off}, 16000);
}

TEST_CASE("34d_voice_chat_off_after_stall — ch0 rejoins ch1's audio depth",
          "[sync][scenario34]") {
  // The connection drops for 1.5 intervals while ch0 is in voice chat: ch1's
  // interval audio falls one interval behind for good (nds0=Y), ch0 is live and
  // isn't queued. Then voice chat goes off, with nothing else happening.
  runCase("scenario34d", "vc34d",
          {{3000, 1, 0, 0},
           {4500, -1, 0, 0, 0, 1500},
           {8000, 0, 0, 0}},
          17000);
}

TEST_CASE("34b_voice_chat_toggle_unstable — re-sync after OFF->ON->OFF with stalls",
          "[sync][scenario34]") {
  // The connection drops for 1.5 intervals during voice chat, and again around
  // the OFF (both directions), plus audio-clock hiccups on both ends.
  runCase("scenario34b", "vc34b",
          {{3000, 1, 0, 0},
           {4500, -1, 0, 0, 0, 1500},
           {5200, -1, 300, 0},
           {7000, 0, 0, 0},
           {7100, -1, 0, 0, 600, 0},
           {7600, -1, 0, 400, 0, 1300}},
          18000);
}

// Control for 34b (hidden; run with "[scenario34-control]"): the same stalls with
// voice chat never turned on, to tell what the stalls cost on their own.
TEST_CASE("34x_stalls_without_voice_chat — control for 34b", "[.][scenario34-control]") {
  runCase("scenario34x", "vc34x",
          {{4500, -1, 0, 0, 0, 1500},
           {5200, -1, 300, 0},
           {7000, 2, 0, 0},
           {7100, -1, 0, 0, 600, 0},
           {7600, -1, 0, 400, 0, 1300}},
          18000);
}

// ---------------------------------------------------------------------------
// Holding a channel's interval (the mechanism rejoining uses) never reorders or
// drops audio. The 2-slot queue keeps one interval beyond its natural depth, so
// a hold is capped: one swap at most, none when an interval is already queued.
// Forced through the host-test hook; checked on the receiver's output only.

namespace {

// Interval tags (mod 16) heard on the left (ch0) and right (ch1) output, sampled
// every 50 ms from `from` on.
void scanOutput(videosync::TestClient &receiver, int64_t from, std::vector<int> &l,
                std::vector<int> &r) {
  std::vector<float> L, R;
  receiver.outputSnapshot(L, R);
  const int kWin = 2048, kStep = 2400;
  for (int64_t at = from; at + kWin <= (int64_t)L.size(); at += kStep) {
    l.push_back(videosync::detectToneTag(L.data() + at, kWin, 48000));
    r.push_back(videosync::detectToneTag(R.data() + at, kWin, 48000));
  }
}

// Every change of tag moves to the next interval (no skipped or repeated one).
int badSteps(const std::vector<int> &seq) {
  int bad = 0, last = -1;
  for (int t : seq) {
    if (t < 0) continue;
    if (last >= 0 && t != last && t != (last + 1) % videosync::kToneTags) ++bad;
    last = t;
  }
  return bad;
}

void runHoldCase(const char *name, const std::string &prefix, int stallFirstMs, int request,
                 int expectHold) {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();
  videosync::TestClient sender(makeOpts("anonymous:" + prefix + "_sender", true));
  videosync::TestClient receiver(makeOpts("anonymous:" + prefix + "_recv", false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  waitForSwaps(sender, receiver);
  receiver.stallAudio(kIntervalMs / 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(2 * kIntervalMs));
  REQUIRE(receiver.panRemoteChannel(prefix + "_sender", 0, -1.f));
  REQUIRE(receiver.panRemoteChannel(prefix + "_sender", 1, 1.f));
  std::this_thread::sleep_for(std::chrono::milliseconds(2 * kIntervalMs));
  if (stallFirstMs > 0) {
    // both channels fall an interval behind: an interval is queued at every swap
    receiver.stallAudio(stallFirstMs);
    std::this_thread::sleep_for(std::chrono::milliseconds(3 * kIntervalMs));
  }
  std::vector<float> L0, R0;
  receiver.outputSnapshot(L0, R0);
  const int64_t from = (int64_t)L0.size();
  receiver.requestIntervalHold(prefix + "_sender", 0, request);
  std::this_thread::sleep_for(std::chrono::milliseconds(8 * kIntervalMs));

  const auto held = log.match(R"(audio TEST-HOLD: user=\S+ ch=0 wanted=\d+ hold=\d+)");
  REQUIRE(held.size() == 1);
  int wanted = -1, hold = -1;
  std::sscanf(held[0].c_str() + held[0].find("wanted="), "wanted=%d hold=%d", &wanted, &hold);

  std::vector<int> l, r;
  scanOutput(receiver, from, l, r);
  // ch0 - ch1 over the last 4 intervals (after the hold has played out)
  std::map<int, int> diff;
  const size_t tail = l.size() > 80 ? l.size() - 80 : 0;
  for (size_t i = tail; i < l.size(); ++i)
    if (l[i] >= 0 && r[i] >= 0)
      diff[((l[i] - r[i]) % videosync::kToneTags + videosync::kToneTags) % videosync::kToneTags]++;
  std::fprintf(stderr, "[%s] wanted=%d hold=%d ch0 badSteps=%d ch1 badSteps=%d (ch0-ch1):", name,
               wanted, hold, badSteps(l), badSteps(r));
  for (const auto &kv : diff) std::fprintf(stderr, " %d x%d", kv.first, kv.second);
  std::fprintf(stderr, "\n");

  CHECK(hold == expectHold);
  CHECK(badSteps(l) == 0);
  CHECK(badSteps(r) == 0);
  // after the hold, ch0 plays `hold` intervals behind ch1, steadily
  const int want = (videosync::kToneTags - hold) % videosync::kToneTags;
  CHECK(diff.size() == 1);
  CHECK(diff.count(want) == 1);
}

} // namespace

TEST_CASE("34e_hold_capped — a 2-swap hold is capped to 1, no reorder or drop",
          "[sync][scenario34]") {
  runHoldCase("scenario34e", "vc34e", 0, 2, 1);
}

TEST_CASE("34f_hold_with_queued_interval — no hold when an interval is already queued",
          "[sync][scenario34]") {
  runHoldCase("scenario34f", "vc34f", 1500, 1, 0);
}

// The receiver turns ch1's receive off and on again after 34d's voice-chat OFF
// (also the user-side workaround on cores without the rejoin): ch1's queue
// restarts from empty and must rejoin ch0's grid.
TEST_CASE("34w_resubscribe_ch1 — receive switch off/on after voice chat off",
          "[sync][scenario34]") {
  Step unsub{11000, -1, 0, 0};
  unsub.subscribeCh1 = 0;
  Step resub{12000, -1, 0, 0};
  resub.subscribeCh1 = 1;
  runCase("scenario34w", "vc34w",
          {{3000, 1, 0, 0}, {4500, -1, 0, 0, 0, 1500}, {8000, 0, 0, 0}, unsub, resub},
          21000);
}

// Same without voice chat: a stall leaves both channels an interval behind, then
// ch1's receive goes off and on. ch1 restarts from empty and must rejoin ch0.
TEST_CASE("34g_resubscribe_after_stall — receive switch off/on, no voice chat",
          "[sync][scenario34]") {
  Step unsub{6000, -1, 0, 0};
  unsub.subscribeCh1 = 0;
  Step resub{7000, -1, 0, 0};
  resub.subscribeCh1 = 1;
  runCase("scenario34g", "vc34g",
          {{3000, -1, 0, 1500}, {5000, 2, 0, 0}, unsub, resub}, 16000);
}

TEST_CASE("34s_silent_ch0 — video follows the sender's music when its ch0 is silent",
          "[sync][scenario34]") {
  // No voice chat. ch0 never fills the first upload block, so each of its
  // intervals reaches the receiver only at the sender's next boundary, together
  // with the video of that next interval, whose marker references it.
  gSilentCh0 = true;
  runCase("scenario34s", "vc34s", {{3000, 2, 0, 0}}, 14000);
  gSilentCh0 = false;
}

TEST_CASE("34t_silent_ch0_voice_chat_off — silent ch0, voice chat ON then OFF",
          "[sync][scenario34]") {
  gSilentCh0 = true;
  runCase("scenario34t", "vc34t", {{3000, 1, 0, 0}, {7000, 0, 0, 0}}, 16000);
  gSilentCh0 = false;
}

// 36: Javier's manual test. Two local channels; ch0 (mono or stereo) toggles voice
// chat ON for 2 intervals, OFF for 2, repeatedly, each toggle at a different point
// of the interval; ch1 never. Real interval (NJ_TEST_INTERVAL_MS). Content is checked
// for every interval from the first OFF on (NJ_REAL_PER_TAG=1 prints each one).
TEST_CASE("36_manual_toggles", "[.][scenario36]") {
  const int I = kIntervalMs;
  gCh0Stereo = std::getenv("NJ_CH0_STEREO") != nullptr;
  gRecvPhaseMs = std::getenv("NJ_REAL_PHASE_MS") ? std::atoi(std::getenv("NJ_REAL_PHASE_MS")) : I / 2;
  const char *pf = std::getenv("NJ_REAL_PREFIX");
  const std::string prefix = pf ? pf : "m36";
  gWholeRun = true;
  // NJ_CH0_MUTED: the voice-chat channel is muted (sends silence) while it toggles.
  gSilentCh0 = std::getenv("NJ_CH0_MUTED") != nullptr;
  const double at[] = {0.10, 0.45, 0.80, 0.25, 0.60, 0.95, 0.35, 0.70};
  std::vector<Step> plan;
  int t = 3 * I;
  for (int i = 0; i < 8; ++i) {
    plan.push_back({t - (t % I) + (int)(at[i] * I), (i % 2) ? 0 : 1, 0, 0});
    t += 2 * I;
  }
  std::fprintf(stderr, "[%s] interval=%dms ch0=%s phase=%dms\n", prefix.c_str(), I,
               gCh0Stereo ? "stereo" : "mono", gRecvPhaseMs);
  runCase(prefix.c_str(), prefix, plan, t + 3 * I);
}
