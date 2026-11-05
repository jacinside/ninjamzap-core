// Scenario 43 — a video interval is not thrown away because the next one's BEGIN came first.
//
// The receiver expects one video BEGIN per interval. When two reach it before its next swap,
// the BEGIN handler took it as the sender running ahead ("burst", e.g. a camera toggled off
// and on) and discarded the interval waiting in `next`. Two BEGINs in one receiver interval
// also happen without any burst: the receiver's network stalls (Wi-Fi, a busy machine) and
// the intervals queued in the socket arrive together, or the sender's boundary sits within
// a few ms of the receiver's swap and jitter puts one BEGIN just after a swap and the next
// just before the following one. The discarded interval's audio still plays, so its video
// is lost for nothing — a whole interval missing (scenario 34g on a slower Mac, 6 of 12 runs).
//
// Here the receiver's network thread stalls for 2.2 intervals while its audio keeps
// running, so two or three video BEGINs are handled back to back.
//
// Verify: every video interval whose audio the receiver plays is delivered (whole), in order,
// and played with that audio; nothing is discarded as a burst while its audio is still to come.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
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
videosync::TestClient::Options makeOpts(const std::string &user, bool sendVideo) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = true;
  o.sendVideo = sendVideo;
  o.toneInput = sendVideo; // the sender's audio carries its interval tag
  o.recordOutput = !sendVideo;
  return o;
}
} // namespace

TEST_CASE("43_video_burst_after_net_stall — video BEGINs handled back to back keep every interval",
          "[sync][scenario43]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient sender(makeOpts("anonymous:vb_sender", /*sendVideo=*/true));
  videosync::TestClient receiver(makeOpts("anonymous:vb_recv", /*sendVideo=*/false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  // 1 s intervals: an earlier scenario (40) may have left the shared server at 8 s.
  if (sender.getBPI() != 4 || sender.getBPM() != 240) {
    sender.sendChatMessage("MSG", "!vote bpi 4");
    sender.sendChatMessage("MSG", "!vote bpm 240");
    auto until = std::chrono::steady_clock::now() + 12s;
    while (std::chrono::steady_clock::now() < until && (sender.getBPM() != 240 || sender.getBPI() != 4))
      std::this_thread::sleep_for(50ms);
  }
  REQUIRE(sender.getBPM() == 240);
  REQUIRE(sender.getBPI() == 4);
  sender.sendFakeSPSPPS();
  for (int i = 0; i < 150 && (sender.intervalSwapCount() < 1 || receiver.intervalSwapCount() < 1); ++i)
    std::this_thread::sleep_for(20ms);
  REQUIRE(receiver.intervalSwapCount() >= 1);

  // 1 s intervals (240 BPM / 4 BPI). Video every 80 ms, tagged with the sender's interval.
  std::vector<std::pair<uint32_t, int>> pushed; // seq, tag
  uint32_t seq = 0;
  int stallTag = -1;
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < 12s) {
    const int tag = sender.currentSwap();
    auto f = videosync::makeFakeFrame(seq, (uint32_t)tag, 256);
    sender.sendVideoFrame(f.data(), (int)f.size());
    pushed.push_back({seq++, tag});
    if (stallTag < 0 && std::chrono::steady_clock::now() - t0 >= 4s) {
      stallTag = tag;
      receiver.stallNetwork(2200);
    }
    std::this_thread::sleep_for(80ms);
  }
  std::this_thread::sleep_for(3s);

  std::map<int, int> pushedPerTag, gotPerTag;
  for (const auto &p : pushed) pushedPerTag[p.second]++;
  std::vector<float> L, R;
  receiver.outputSnapshot(L, R);
  int misaligned = 0, checked = 0, lastTagSeen = -1, reorder = 0;
  for (const auto &f : receiver.drainVideoFrames()) {
    videosync::FakeFramePayload p;
    if (!videosync::parseFakeFrame(f.data.data(), (int)f.data.size(), p)) continue;
    const int tag = (int)p.intervalIdx;
    gotPerTag[tag]++;
    if (tag < lastTagSeen) ++reorder;
    lastTagSeen = std::max(lastTagSeen, tag);
    if (f.outputSample >= 0 && f.outputSample + 4096 <= (int64_t)L.size()) {
      const int a = videosync::detectToneTag(L.data() + f.outputSample, 4096, 48000);
      if (a >= 0) {
        ++checked;
        if (a != tag % videosync::kToneTags) ++misaligned;
      }
    }
  }

  // Intervals whose audio the receiver played (its output, tone-tagged): those must have
  // their video. The stall can make the receiver skip an interval's audio; that interval's
  // video has nothing to play with and may go.
  std::map<int, int> heard;
  for (int64_t at = 0; at + 4096 <= (int64_t)L.size(); at += 2400) {
    const int a = videosync::detectToneTag(L.data() + at, 4096, 48000);
    if (a >= 0) heard[a]++;
  }
  const int lastTag = pushed.back().second;
  const auto bursts = log.match(R"(video BURST: discarding next)");
  const auto awaitDrops = log.match(R"(video AWAIT-DROP: key=vb_sender)");
  std::vector<int> lostTags;
  for (int t = stallTag + 1; t <= lastTag - 2; ++t)
    if (gotPerTag[t] == 0 && heard[t % videosync::kToneTags] >= 5) lostTags.push_back(t);
  std::fprintf(stderr, "[scenario43] stall at tag %d; tags %d..%d: lost=%zu", stallTag, stallTag + 1,
               lastTag - 2, lostTags.size());
  for (int t : lostTags) std::fprintf(stderr, " %d", t);
  std::fprintf(stderr, "; BURST=%zu AWAIT-DROP=%zu; frames checked=%d misaligned=%d reorder=%d\n",
               bursts.size(), awaitDrops.size(), checked, misaligned, reorder);
  for (int t = stallTag - 1; t <= lastTag; ++t)
    std::fprintf(stderr, "[scenario43]   tag %d: pushed %d got %d audio heard %d\n", t, pushedPerTag[t],
                 gotPerTag[t], heard[t % videosync::kToneTags]);

  // The stall must hand the receiver two video BEGINs within one of its intervals, or this
  // tests nothing: two "video BEGIN" lines with the same local interval.
  std::map<int, int> beginsPerLocal;
  for (const auto &l : log.match(R"(video BEGIN: key=vb_sender:1 interval=\d+)")) {
    const auto at = l.find("interval=");
    beginsPerLocal[std::atoi(l.c_str() + at + 9)]++;
  }
  int doubleBegins = 0;
  for (const auto &kv : beginsPerLocal) doubleBegins += kv.second >= 2;
  std::fprintf(stderr, "[scenario43] receiver intervals with two or more video BEGINs: %d\n", doubleBegins);
  REQUIRE(doubleBegins >= 1);
  CHECK(lostTags.empty());
  CHECK(reorder == 0);
  CHECK(checked >= 40);
  CHECK(misaligned == 0);
}
