// Scenario 42 — the session archive keeps raw video next to the audio (config_savevideo).
//
// An offline consumer (the PRO bot) archives a room with config_savelocalaudio=1 and
// config_savevideo=1 and pairs each camera interval to its audio by the sync marker's GUID,
// like the receiver does. This checks the files it gets:
//
//   * clipsort.log has a `video <guid> "<user>" <ch>` line per video interval.
//   * <workdir>/<hex0>/<guid>.H264 holds the download as received: frame 0 is the 24-byte sync
//     marker, frame 1 the cached SPS/PPS.
//   * The marker's audio GUID is an interval of the same user that the archive kept as .OGG
//     (every video interval after the first one the receiver saw whole).
//   * A receiver archiving with config_savevideo=0 writes no .H264 and no `video` lines.
//   * A sender that leaves mid-interval (no END) leaves no open file: the tracker is released
//     with the user and its file closed, so what was received is on disk (the fake frames are
//     far below the stdio buffer, so an unclosed file would still read as empty).
//   * Same for a receiver that disconnects mid-interval: Disconnect() closes its open files.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "catch_amalgamated.hpp"

#include "njclient.h"
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

void waitIntervals(videosync::TestClient &c, int count) {
  for (int i = 0; i < count; ++i) {
    int base = c.intervalSwapCount();
    auto until = std::chrono::steady_clock::now() + 10s;
    while (c.intervalSwapCount() == base && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(20ms);
  }
}

std::string makeWorkDir(const char *tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/nj42-%s-XXXXXX", tag);
  char *d = mkdtemp(tmpl);
  REQUIRE(d != nullptr);
  return std::string(d) + "/";
}

void startArchive(videosync::TestClient &c, const std::string &dir, bool video) {
  NJClient *nj = c.raw();
  nj->SetWorkDir(const_cast<char *>(dir.c_str()));
  nj->SetLogFile(const_cast<char *>("clipsort.log"));
  nj->config_savevideo = video ? 1 : 0;
  nj->config_savelocalaudio = 1;
}

std::vector<std::string> readLines(const std::string &path) {
  std::vector<std::string> out;
  std::ifstream f(path);
  for (std::string l; std::getline(f, l);) out.push_back(l);
  return out;
}

std::vector<uint8_t> readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

std::string hex(const uint8_t *p, int n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}

void streamIntervals(videosync::TestClient &sender, uint32_t &seq, int count) {
  for (int i = 0; i < count; ++i) {
    int swapAtStart = sender.intervalSwapCount();
    for (int f = 0; f < 3; ++f) {
      auto frame = videosync::makeFakeFrame(seq++, (uint32_t)swapAtStart, 256);
      sender.sendVideoFrame(frame.data(), (int)frame.size());
      std::this_thread::sleep_for(120ms);
    }
    auto until = std::chrono::steady_clock::now() + 10s;
    while (sender.intervalSwapCount() == swapAtStart && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(20ms);
  }
}
} // namespace

TEST_CASE("42_session_archive_video — raw video archived with its sync marker",
          "[archive][scenario42]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient sender(makeOpts("anonymous:arch_sender", /*sendVideo=*/true));
  videosync::TestClient recv(makeOpts("anonymous:arch_recv", false));
  videosync::TestClient recvAudioOnly(makeOpts("anonymous:arch_recv_a", false));
  videosync::TestClient recvLeaver(makeOpts("anonymous:arch_recv_l", false));
  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(recv.connectAndJoin(8s));
  REQUIRE(recvAudioOnly.connectAndJoin(8s));
  REQUIRE(recvLeaver.connectAndJoin(8s));

  if (sender.getBPI() != 4 || sender.getBPM() != 240) {
    sender.sendChatMessage("MSG", "!vote bpi 4");
    sender.sendChatMessage("MSG", "!vote bpm 240");
    auto until = std::chrono::steady_clock::now() + 12s;
    while (std::chrono::steady_clock::now() < until && (sender.getBPM() != 240 || sender.getBPI() != 4))
      std::this_thread::sleep_for(50ms);
  }

  const std::string dir = makeWorkDir("av"), dirA = makeWorkDir("a"), dirL = makeWorkDir("l");
  startArchive(recv, dir, /*video=*/true);
  startArchive(recvAudioOnly, dirA, /*video=*/false);
  startArchive(recvLeaver, dirL, /*video=*/true);

  sender.sendFakeSPSPPS();
  waitIntervals(sender, 2);
  uint32_t seq = 0;
  streamIntervals(sender, seq, 3);

  // recvLeaver disconnects mid-interval, while a download is open: Disconnect() must close it.
  std::this_thread::sleep_for(150ms);
  {
    auto f = videosync::makeFakeFrame(seq++, (uint32_t)sender.intervalSwapCount(), 256);
    sender.sendVideoFrame(f.data(), (int)f.size());
  }
  std::this_thread::sleep_for(300ms);
  recvLeaver.disconnect();
  recvLeaver.raw()->SetLogFile(nullptr);
  streamIntervals(sender, seq, 2);

  // Leave mid-interval: one frame of a new interval, then disconnect (no END for it).
  waitIntervals(sender, 1);
  std::this_thread::sleep_for(150ms);
  auto last = videosync::makeFakeFrame(seq++, (uint32_t)sender.intervalSwapCount(), 256);
  sender.sendVideoFrame(last.data(), (int)last.size());
  std::this_thread::sleep_for(300ms);
  sender.disconnect();
  std::this_thread::sleep_for(2s); // receivers see the user leave

  // Stop archiving (like the adapter's stopSessionArchive) so clipsort.log is flushed and closed.
  // Video files are NOT closed by this: those must already be, by the user leaving.
  for (auto *c : {&recv, &recvAudioOnly}) {
    c->raw()->config_savelocalaudio = 0;
    c->raw()->SetLogFile(nullptr);
  }

  // --- the archive with video
  std::set<std::string> audio; // GUIDs of the sender's archived audio intervals (.OGG on disk)
  std::vector<std::string> video;
  const std::regex userRe(R"re(^user ([0-9A-F]{32}) "arch_sender[^"]*" 0 )re", std::regex::icase);
  const std::regex videoRe(R"re(^video ([0-9A-F]{32}) "arch_sender[^"]*" (\d+)$)re", std::regex::icase);
  for (const auto &l : readLines(dir + "clipsort.log")) {
    std::smatch m;
    if (std::regex_search(l, m, userRe) && exists(dir + m[1].str().substr(0, 1) + "/" + m[1].str() + ".OGG"))
      audio.insert(m[1]);
    if (std::regex_search(l, m, videoRe)) video.push_back(m[1]);
  }
  std::fprintf(stderr, "[scenario42] archived audio=%zu video=%zu\n", audio.size(), video.size());
  REQUIRE(video.size() >= 4);

  int withMarker = 0, paired = 0, unclosed = 0;
  for (size_t i = 0; i < video.size(); i++) {
    const std::string g = video[i];
    const std::string path = dir + g.substr(0, 1) + "/" + g + ".H264";
    REQUIRE(exists(path));
    const auto b = readFile(path);
    if (b.size() < 24) { unclosed++; std::fprintf(stderr, "[scenario42] %s: %zu bytes\n", g.c_str(), b.size()); continue; }
    // frame 0: the sync marker [len=20][sender swap][16B audio GUID]
    CHECK(be32(&b[0]) == 20);
    if (be32(&b[0]) != 20) continue;
    withMarker++;
    // frame 1: the cached SPS/PPS (2-byte NAL lengths: 12-byte SPS 0x67, then the PPS)
    if (b.size() >= 24 + 4 + 4) {
      const uint32_t spsLen = be32(&b[24]);
      CHECK(spsLen == 22);
      CHECK(b[28] == 0x00); CHECK(b[29] == 0x0c); CHECK(b[30] == 0x67);
    }
    std::string ag = hex(&b[8], 16);
    if (!audio.count(ag)) for (auto &c : ag) c = (char)toupper(c); // guidtostr's case
    const bool inArchive = audio.count(ag) > 0;
    // Exempt: the first interval (may name audio from before the receiver joined) and the last
    // (cut off by the sender leaving: its audio was never uploaded).
    if (inArchive || i == 0 || i + 1 == video.size()) paired++;
    if (!inArchive) std::fprintf(stderr, "[scenario42] video %zu %s: marker audio %s not archived\n", i, g.c_str(), ag.c_str());
  }
  // Every interval is closed — including the one cut off by the sender leaving.
  CHECK(unclosed == 0);
  CHECK(withMarker == (int)video.size());
  // Every other video interval names an audio interval of the same user that the archive kept.
  CHECK(paired == (int)video.size());

  // --- the receiver that disconnected mid-interval: every archived video file is closed
  int leaverVideo = 0, leaverUnclosed = 0;
  for (const auto &l : readLines(dirL + "clipsort.log")) {
    std::smatch m;
    if (!std::regex_search(l, m, videoRe)) continue;
    leaverVideo++;
    const std::string g = m[1];
    const auto b = readFile(dirL + g.substr(0, 1) + "/" + g + ".H264");
    if (b.size() < 24) leaverUnclosed++;
  }
  std::fprintf(stderr, "[scenario42] leaver: video=%d unclosed=%d\n", leaverVideo, leaverUnclosed);
  CHECK(leaverVideo >= 2);
  CHECK(leaverUnclosed == 0);

  // --- the archive without video
  int videoLinesA = 0;
  for (const auto &l : readLines(dirA + "clipsort.log"))
    if (l.rfind("video ", 0) == 0) videoLinesA++;
  int h264A = 0;
  for (const auto &g : video)
    if (exists(dirA + g.substr(0, 1) + "/" + g + ".H264")) h264A++;
  CHECK(videoLinesA == 0);
  CHECK(h264A == 0);

  recvAudioOnly.disconnect();
  recv.disconnect();
}
