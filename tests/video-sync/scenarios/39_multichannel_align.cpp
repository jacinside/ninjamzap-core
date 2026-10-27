// Scenario 39 — a user's channels play the interval they were recorded in, together.
//
// The sender has N interval-mode audio channels (default 8, e.g. a multitrack drum kit)
// and video anchored to ch0. Each channel tags the interval it records with a tone in
// its own band (ToneTag.h toneFreqFor), so the receiver's mixed output tells, for every
// displayed video frame, which sender interval each channel is playing. All channels
// and the video must play the same interval, whatever the sender does to any channel:
// mute it / fader to 0 (the app sends digital silence), make it very quiet, turn voice
// chat on or off.
//
// Reported 2026-10-26 (1.0.10, Mac + iPad): with channel 1 muted or at volume 0, after
// a while one channel (and the video) fell one interval behind the other and stayed
// there. A muted channel's encoded data is tiny, so the receiver only gets enough of it
// to start playing late in the interval; when that crosses the receiver's swap the
// channel misses the swap and its queue keeps it one interval behind for good.
//
// Besides the interval each channel plays, the check counts frames where a channel
// that should sound is missing: aligning must never silence a channel.
//
// Real-interval server only (docker/test-multi.cfg lets anonymous users claim up to
// 32 channels): `make test-real`. Env: NJ_TEST_INTERVAL_MS, NJ_MC_PLAN
// (mute|vc|vcmute|vcloud|mix|fill|fillvc), NJ_MC_CHANNELS,
// NJ_REAL_PHASE_MS, NJ_REAL_PREFIX, NJ_MC_UNTAGGED, NJ_MC_SYNCLOG.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "catch_amalgamated.hpp"

#include "FakeFrame.h"
#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"
#include "ToneTag.h"
#include "njclient.h"

using namespace std::chrono_literals;

namespace {

const int kI = std::getenv("NJ_TEST_INTERVAL_MS") ? std::atoi(std::getenv("NJ_TEST_INTERVAL_MS")) : 1000;

enum Act { MUTE, UNMUTE, VC_ON, VC_OFF, GAIN, GATE };

struct McStep {
  int atMs;
  int ch;
  Act act;
  float gain = 1.f; // GAIN: tone level; GATE: silent for this fraction of each interval
};

struct Pushed {
  uint32_t seq;
  int tag;
};

const char *actName(Act a) {
  switch (a) {
  case MUTE: return "MUTE";
  case UNMUTE: return "unmute";
  case VC_ON: return "VC ON";
  case VC_OFF: return "VC off";
  case GATE: return "gate";
  default: return "gain";
  }
}

// at(k, f): fraction f of the k-th interval from the start of the plan
int at(int k, double f) { return (int)((k + f) * kI); }

std::vector<McStep> makePlan(const std::string &name, int nch, int &intervals) {
  std::vector<McStep> p;
  if (name == "mute") {
    // Mute / unmute each channel in turn at different points of the interval.
    const double fr[] = {0.10, 0.50, 0.90, 0.30, 0.98, 0.02, 0.70, 0.60};
    int k = 2;
    for (int c = 0; c < nch; ++c) {
      p.push_back({at(k, fr[c % 8]), c, MUTE});
      p.push_back({at(k + 2, fr[(c + 3) % 8]), c, UNMUTE});
      k += 3;
    }
    intervals = k + 3;
  } else if (name == "vc") {
    // Voice chat on/off on ch0, on a middle and on the last channel, then on two at once.
    const int mid = nch / 2, last = nch - 1;
    p.push_back({at(2, 0.3), 0, VC_ON});
    p.push_back({at(5, 0.6), 0, VC_OFF});
    p.push_back({at(8, 0.8), mid, VC_ON});
    p.push_back({at(11, 0.2), mid, VC_OFF});
    p.push_back({at(14, 0.5), last, VC_ON});
    p.push_back({at(17, 0.9), last, VC_OFF});
    p.push_back({at(20, 0.4), 0, VC_ON});
    p.push_back({at(20, 0.45), last, VC_ON});
    p.push_back({at(23, 0.1), 0, VC_OFF});
    p.push_back({at(23, 0.7), last, VC_OFF});
    intervals = 27;
  } else if (name == "vcmute" || name == "vcloud") {
    // Voice chat on/off on ch1, a middle and the last channel; vcmute: with ch0 muted
    // throughout (the app's M button / fader at 0 on channel 1), vcloud: ch0 playing.
    if (name == "vcmute") p.push_back({0, 0, MUTE});
    const int chs[] = {1, nch / 2, nch - 1};
    int k = 2;
    for (int c : chs) {
      p.push_back({at(k, 0.3), c, VC_ON});
      p.push_back({at(k + 2, 0.6), c, VC_OFF});
      k += 5;
    }
    intervals = k + 6; // video checked against the audio again after the last voice chat
  } else if (name == "fill" || name == "fillvc") {
    // A channel that only sounds in the last 15% of every interval (a gated tom, a
    // late phrase): its download becomes playable late in the interval, after the
    // other channels'. It must play with them, and be heard. fillvc: ch0 first
    // leaves voice chat, which places the user's channels on the swap right after
    // their downloads become playable, so the late channel can't make that swap.
    int k = 0;
    if (name == "fillvc") {
      p.push_back({at(2, 0.3), 0, VC_ON});
      p.push_back({at(4, 0.6), 0, VC_OFF});
      k = 8;
    }
    p.push_back({at(k, 0.0), 3, GATE, 0.85f});
    intervals = k + 14;
  } else { // mix: ch0 muted throughout, a near-silent channel, mutes and voice chat elsewhere
    p.push_back({0, 0, MUTE});
    p.push_back({0, nch - 2, GAIN, 0.05f}); // a mic left almost closed (below the detector: not checked)
    p.push_back({at(3, 0.4), 1, MUTE});
    p.push_back({at(5, 0.95), 1, UNMUTE});
    p.push_back({at(7, 0.2), nch - 1, VC_ON});
    p.push_back({at(10, 0.5), nch - 1, VC_OFF});
    p.push_back({at(12, 0.05), 0, UNMUTE});
    p.push_back({at(14, 0.6), 0, MUTE});
    p.push_back({at(16, 0.99), 0, UNMUTE});
    intervals = 20;
  }
  return p;
}

} // namespace

TEST_CASE("39_multichannel_align", "[.][scenario39]") {
  const int nch = std::getenv("NJ_MC_CHANNELS") ? std::atoi(std::getenv("NJ_MC_CHANNELS")) : 8;
  REQUIRE(nch >= 2);
  REQUIRE(nch <= videosync::kMaxToneChannels);
  const std::string planName = std::getenv("NJ_MC_PLAN") ? std::getenv("NJ_MC_PLAN") : "mute";
  const int phaseMs = std::getenv("NJ_REAL_PHASE_MS") ? std::atoi(std::getenv("NJ_REAL_PHASE_MS")) : kI / 2;
  const std::string prefix = std::getenv("NJ_REAL_PREFIX") ? std::getenv("NJ_REAL_PREFIX") : "m39";
  const std::string senderName = prefix + "_sender";
  int intervals = 0;
  const std::vector<McStep> plan = makePlan(planName, nch, intervals);

  videosync::TestClient::Options so;
  so.host = videosync::testenv::host;
  so.port = videosync::testenv::port;
  so.user = "anonymous:" + senderName;
  so.sendAudio = false;
  so.toneChannels = nch;
  so.sendVideo = true;
  so.videoChannel = nch;
  videosync::TestClient::Options ro;
  ro.host = so.host;
  ro.port = so.port;
  ro.user = "anonymous:" + prefix + "_recv";
  ro.sendAudio = true;
  ro.recordOutput = true;
  videosync::SyncLogCapture::instance().clear();
  videosync::TestClient sender(so);
  videosync::TestClient receiver(ro);
  // NJ_MC_UNTAGGED: the sender sends random interval GUIDs (a client without interval
  // tags); the receiver keeps its per-channel queues for it.
  if (std::getenv("NJ_MC_UNTAGGED")) sender.raw()->TestUntaggedGuids(true);
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  sender.sendFakeSPSPPS();
  for (int i = 0; i < 150 && (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1); ++i)
    std::this_thread::sleep_for(20ms);
  REQUIRE(receiver.intervalSwapCount() >= 1);
  receiver.stallAudio(phaseMs); // the receiver's swap lags the sender's by phaseMs
  for (int i = 0; i < 300 && !receiver.panRemoteChannel(senderName, nch - 1, 0.f); ++i)
    std::this_thread::sleep_for(100ms);
  REQUIRE(receiver.panRemoteChannel(senderName, nch - 1, 0.f));
  std::this_thread::sleep_for(std::chrono::milliseconds(2 * kI));
  std::fprintf(stderr, "[%s] plan=%s channels=%d interval=%dms phase=%dms\n", prefix.c_str(),
               planName.c_str(), nch, kI, phaseMs);

  // Run the plan, pushing a video frame tagged with the sender's interval every 80 ms.
  std::vector<Pushed> pushed;
  std::map<int, int> vcTags; // sender intervals touched by a voice-chat toggle
  // Voice chat: the channel plays live (not on the interval grid); its checks skip the
  // video intervals shown from the ON until two intervals after the OFF.
  std::map<int, std::vector<std::pair<int, int>>> liveSpan; // ch -> [from, to] sender intervals
  std::map<int, int> vcOnAt;
  // Sender intervals in which each channel was muted (part of or the whole interval).
  std::map<int, std::vector<std::pair<int, int>>> mutedSpan;
  std::map<int, int> mutedAt;
  std::map<int, int> gatedFrom; // sender interval a channel's gate started in
  uint32_t seq = 0;
  size_t next = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const int totalMs = intervals * kI;
  const int firstTag = sender.currentSwap() + 1;
  for (;;) {
    const int nowMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
    if (nowMs >= totalMs) break;
    while (next < plan.size() && nowMs >= plan[next].atMs) {
      const McStep &s = plan[next++];
      const int tag = sender.currentSwap();
      switch (s.act) {
      case MUTE: sender.setChannelSilent(s.ch, true); mutedAt[s.ch] = tag; break;
      case UNMUTE:
        sender.setChannelSilent(s.ch, false);
        mutedSpan[s.ch].push_back({mutedAt.count(s.ch) ? mutedAt[s.ch] : 0, tag});
        mutedAt.erase(s.ch);
        break;
      case GAIN: sender.setChannelGain(s.ch, s.gain); break;
      case GATE: sender.setChannelGate(s.ch, s.gain); gatedFrom[s.ch] = tag; break;
      case VC_ON:
      case VC_OFF:
        sender.setVoiceChat(s.ch, s.act == VC_ON, true);
        for (int d = 0; d <= 2; ++d) vcTags[tag + d] = 1;
        if (s.act == VC_ON) vcOnAt[s.ch] = tag;
        else liveSpan[s.ch].push_back({vcOnAt[s.ch] - 2, tag + 2}); // shown video lags the sender up to 2 intervals
        break;
      }
      std::fprintf(stderr, "[%s] t=%dms ch%d %s at sender interval %d\n", prefix.c_str(), nowMs,
                   s.ch, actName(s.act), tag);
    }
    const int tag = sender.currentSwap();
    auto f = videosync::makeFakeFrame(seq, (uint32_t)tag, 256);
    sender.sendVideoFrame(f.data(), (int)f.size());
    pushed.push_back({seq++, tag});
    std::this_thread::sleep_for(80ms);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(3 * kI));
  const auto delivered = receiver.drainVideoFrames();
  const int lastTag = pushed.back().tag - 1;

  auto isLive = [&](int ch, int tag) {
    for (const auto &sp : liveSpan[ch])
      if (tag >= sp.first && tag <= sp.second) return true;
    return false;
  };

  for (const auto &m : mutedAt) mutedSpan[m.first].push_back({m.second, lastTag + 1}); // never unmuted
  // Channels whose tone the detector can't be expected to hear in a whole interval.
  std::vector<bool> quiet(nch, false);
  for (const auto &st : plan)
    if (st.act == GAIN && st.gain < 0.25f) quiet[st.ch] = true;
  auto gatedAt = [&](int ch, int tag) { return gatedFrom.count(ch) && tag >= gatedFrom[ch]; };
  auto shouldSound = [&](int ch, int tag) {
    if (quiet[ch] || gatedAt(ch, tag) || isLive(ch, tag)) return false;
    for (const auto &sp : mutedSpan[ch])
      if (tag >= sp.first && tag <= sp.second) return false;
    return true;
  };

  std::vector<float> L, R;
  receiver.outputSnapshot(L, R);
  const int kWin = 4096;
  auto signedOff = [](int a, int v) {
    int d = ((a - v) % videosync::kToneTags + videosync::kToneTags) % videosync::kToneTags;
    return d >= videosync::kToneTags / 2 ? d - videosync::kToneTags : d;
  };
  int checked = 0, bad = 0;
  std::vector<int> badCh(nch, 0), seenCh(nch, 0), expectCh(nch, 0), missingCh(nch, 0);
  std::vector<int> gatedFrames(nch, 0), gatedHeard(nch, 0); // after the gate (+1: the interval it starts in)
  std::map<int, std::map<std::string, int>> perTag;
  for (const auto &f : delivered) {
    videosync::FakeFramePayload p;
    if (!videosync::parseFakeFrame(f.data.data(), (int)f.data.size(), p)) continue;
    const int tag = (int)p.intervalIdx;
    if (tag < firstTag || tag > lastTag || f.outputSample < 0 ||
        f.outputSample + kWin > (int64_t)L.size()) continue;
    // While any channel is in voice chat the receiver plays the video live, so the
    // interval channels are checked against each other instead (the first one heard).
    bool anyLive = false;
    for (int c = 0; c < nch; ++c) anyLive |= isLive(c, tag);
    int v = tag % videosync::kToneTags;
    if (anyLive) {
      v = -1;
      for (int c = 0; c < nch && v < 0; ++c)
        if (!isLive(c, tag)) v = videosync::detectToneTagFor(L.data() + f.outputSample, kWin, 48000, c);
      if (v < 0) continue;
    }
    ++checked;
    std::string key;
    bool frameBad = false;
    // Channels compared with each other (voice chat): a window over an interval boundary
    // hears some channels' tones split between two tags (no tone). Such a frame is a
    // transition: it still counts for missing audio, not for alignment.
    bool transition = false;
    if (anyLive)
      for (int c = 0; c < nch && !transition; ++c)
        transition = shouldSound(c, tag) &&
                     videosync::detectToneTagFor(L.data() + f.outputSample, kWin, 48000, c) < 0;
    for (int c = 0; c < nch; ++c) {
      const int a = videosync::detectToneTagFor(L.data() + f.outputSample, kWin, 48000, c);
      char k[16];
      if (a < 0) std::snprintf(k, sizeof k, " %d:-", c);
      else if (anyLive) std::snprintf(k, sizeof k, " %d:%+d~", c, signedOff(a, v));
      else std::snprintf(k, sizeof k, " %d:%+d", c, signedOff(a, v));
      key += k;
      if (gatedFrom.count(c) && tag > gatedFrom[c] && !isLive(c, tag)) {
        ++gatedFrames[c];
        if (a >= 0) ++gatedHeard[c];
      }
      if (shouldSound(c, tag)) {
        ++expectCh[c];
        if (a < 0) ++missingCh[c];
      }
      if (a < 0 || isLive(c, tag) || transition) continue;
      ++seenCh[c];
      if (a != v) { ++badCh[c]; frameBad = true; }
    }
    bad += frameBad;
    perTag[tag][key]++;
  }
  for (const auto &kv : perTag) {
    std::fprintf(stderr, "[%s]   video=%d%s:", prefix.c_str(), kv.first, vcTags.count(kv.first) ? "*" : "");
    for (const auto &c : kv.second) std::fprintf(stderr, " [%s ]x%d", c.first.c_str(), c.second);
    std::fprintf(stderr, "\n");
  }
  // NJ_MC_SYNCLOG: dump the core's sync log (alignment, video matching) for the sender.
  if (std::getenv("NJ_MC_SYNCLOG"))
    for (const auto &l : videosync::SyncLogCapture::instance().all())
      if (l.find(senderName) != std::string::npos || l.find(prefix + "_sender") != std::string::npos)
        std::fprintf(stderr, "%s\n", l.c_str());
  std::fprintf(stderr, "[%s] intervals %d..%d: frames=%d misaligned=%d per channel:", prefix.c_str(),
               firstTag, lastTag, checked, bad);
  for (int c = 0; c < nch; ++c) std::fprintf(stderr, " ch%d=%d/%d", c, badCh[c], seenCh[c]);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "[%s] missing where it should sound (frames):", prefix.c_str());
  for (int c = 0; c < nch; ++c) std::fprintf(stderr, " ch%d=%d/%d", c, missingCh[c], expectCh[c]);
  std::fprintf(stderr, "\n");
  CHECK(checked >= 30);
  for (int c = 0; c < nch; ++c) {
    // A correction costs a channel at most one interval of silence (~150 frames);
    // a channel silenced for good misses nearly all of its frames.
    if (expectCh[c] > 0) CHECK(missingCh[c] <= expectCh[c] / 10);
    // a gated channel sounds in the last 15% of each interval
    if (gatedFrames[c] > 0) {
      std::fprintf(stderr, "[%s] ch%d heard in %d of %d frames after its gate\n", prefix.c_str(), c,
                   gatedHeard[c], gatedFrames[c]);
      CHECK(gatedHeard[c] >= gatedFrames[c] / 20);
    }
  }
  CHECK(bad == 0);
}
