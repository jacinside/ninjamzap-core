// Scenario 44 — the receiver counts, per musician, the audio that arrived too late.
//
// When playback reaches a remote download still streaming in, that stretch plays as
// silence (audio STARVE). NJClient::TakeUserLateMs reports it per user so the app can
// say whose intervals arrive late: their uplink, or our downlink, can't keep up.
//
// Setup: two senders on 8 s intervals (120 BPM / 16 BPI). One stops producing audio for
// longer than an interval, twice (its network stays up: a stalled network that long is
// dropped by the server), so the interval it was sending runs dry on the receiver
// whatever the phase between the clients; the other keeps sending.
// Checks: the stalled sender reads late about as much as the STARVE lines lost, the
// steady one reads 0, and a read resets the count. The steady sender plays the stalled
// one too, so the STARVE lines are both clients': they add up to both counts.
#include <chrono>
#include <cstdio>
#include <regex>
#include <string>
#include <thread>

#include "catch_amalgamated.hpp"

#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"
#include "njclient.h"

using namespace std::chrono_literals;

namespace {
videosync::TestClient::Options makeOpts(const std::string &user, bool send) {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = user;
  o.sendAudio = send;
  o.toneInput = send;
  o.audioBitrate = 128;
  return o;
}

// TakeUserLateMs for the remote user whose name starts with `who` ("name@ip"); -1 when absent.
double takeLate(videosync::TestClient &recv, const std::string &who) {
  NJClient *c = recv.raw();
  for (int i = 0; i < c->GetNumUsers(); i++) {
    const char *n = c->GetUserState(i);
    if (n && std::string(n).rfind(who + "@", 0) == 0) return c->TakeUserLateMs(i);
  }
  return -1.0;
}
} // namespace

TEST_CASE("44_late_audio_count — late audio is counted per musician",
          "[sync][scenario44]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  videosync::TestClient slow(makeOpts("anonymous:la_slow", true));
  videosync::TestClient steady(makeOpts("anonymous:la_steady", true));
  videosync::TestClient recv(makeOpts("anonymous:la_recv", false));
  REQUIRE(slow.connectAndJoin(8s));
  slow.sendChatMessage("MSG", "!vote bpm 120");
  slow.sendChatMessage("MSG", "!vote bpi 16");
  auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline && (slow.getBPM() != 120 || slow.getBPI() != 16))
    std::this_thread::sleep_for(50ms);
  REQUIRE(slow.getBPM() == 120);
  REQUIRE(steady.connectAndJoin(8s));
  REQUIRE(recv.connectAndJoin(8s));

  // Both senders playing on the receiver.
  const int swaps0 = recv.intervalSwapCount();
  deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline && recv.intervalSwapCount() < swaps0 + 3)
    std::this_thread::sleep_for(50ms);
  REQUIRE(!log.match(R"(SWAP#\d+ audio: user=la_steady ch=0 ds=Y)").empty());
  REQUIRE(!log.match(R"(SWAP#\d+ audio: user=la_slow ch=0 ds=Y)").empty());
  takeLate(recv, "la_slow"); // start counting from here
  takeLate(recv, "la_steady");
  takeLate(steady, "la_slow");
  log.clear();

  for (int i = 0; i < 2; i++) {
    slow.stallAudio(10000);
    std::this_thread::sleep_for(14s);
  }
  std::this_thread::sleep_for(10s); // the last late interval plays out

  const double slowMs = takeLate(recv, "la_slow");
  const double steadyMs = takeLate(recv, "la_steady");
  const double again = takeLate(recv, "la_slow");
  const double onSteadyMs = takeLate(steady, "la_slow");

  // What the STARVE lines say was lost; a stretch still running has no end line yet.
  double loggedMs = 0;
  const std::regex lost(R"(lost=(\d+)ms)");
  for (const auto &l : log.match(R"(audio STARVE end: user=la_slow ch=0)")) {
    std::smatch m;
    if (std::regex_search(l, m, lost)) loggedMs += std::stod(m[1].str());
  }
  std::fprintf(stderr, "[scenario44] late slow=%.0fms (on the other sender %.0fms, STARVE lost %.0fms) steady=%.0fms reread=%.0fms\n",
               slowMs, onSteadyMs, loggedMs, steadyMs, again);

  REQUIRE(loggedMs > 0); // the stalls did starve the receiver, or this tests nothing
  CHECK(slowMs > 0);
  // A stretch still running at the end has no end line yet: up to one interval more.
  CHECK(slowMs + onSteadyMs >= loggedMs - 50);
  CHECK(slowMs + onSteadyMs <= loggedMs + 2 * 8000);
  CHECK(steadyMs == 0);
  CHECK(again == 0);
}
