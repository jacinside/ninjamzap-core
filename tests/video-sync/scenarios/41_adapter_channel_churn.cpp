// Scenario 41 — adding and removing a transmitting local channel through NinjamClientAdapter
// while the connection thread encodes.
//
// NJClient leaves it to its caller to serialize Run() with changes to the local channel list
// (Cockos's own client does it with one client mutex). The adapter runs Run() under gsMtx on its
// connection thread; removeLocalChannel() / SetLocalChannelInfo() must take it too, or the
// Local_Channel (and its Vorbis encoder) is deleted while Run() is encoding it — a crash in
// vorbis_bitrate_managed seen in the app's E2E E8 ("remove a local channel").
//
// The race is narrow, so a plain run rarely crashes: build with ThreadSanitizer to see it
// every time (make tsan TEST_ARGS='"[scenario41]"').
//
// Verify: the app's pattern — audio thread feeding a tone, the main thread adding / removing a
// second transmitting channel 40 times — runs to the end, connected.
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "catch_amalgamated.hpp"

#include "TestEnv.h"
#include "abNinjam/ninjamclientAdapter.h"

using namespace std::chrono_literals;

extern "C" void NinjamClient_setBridgeLicenseCallback(int32_t (*cb)(const char *));
extern "C" void NinjamClient_respondToLicense(int accepted);

TEST_CASE("41_adapter_channel_churn — add/remove a transmitting channel while Run() encodes", "[scenario41]") {
  NinjamClientAdapter adapter;
  adapter.setCredentials("churn", "");
  adapter.setAudioConfig(48000, 2);
  // The server's license: accepted at once, as the app's dialog would (the adapter waits for it).
  NinjamClient_setBridgeLicenseCallback([](const char *) -> int32_t { NinjamClient_respondToLicense(1); return 0; });
  adapter.connect(videosync::testenv::host, videosync::testenv::port);
  REQUIRE(adapter.isConnected());

  // Channel 0 transmits the whole time, as the primary channel does in the app.
  adapter.SetLocalChannelInfo(0, "main", true, 0, true, true, false, 0);

  // Audio thread: 128-frame blocks at real-time pace, a tone on both inputs.
  std::atomic<bool> running{true};
  std::thread audio([&] {
    const int frames = 128;
    std::vector<float> l(frames), r(frames), outL(frames), outR(frames), metro(frames);
    float *in[2] = {l.data(), r.data()};
    double phase = 0;
    auto next = std::chrono::steady_clock::now();
    while (running) {
      for (int i = 0; i < frames; ++i) {
        l[i] = r[i] = 0.3f * (float)std::sin(phase);
        phase += 2 * M_PI * 440.0 / 48000.0;
      }
      adapter.processAudioN(in, 2, outL.data(), outR.data(), metro.data(), frames);
      next += std::chrono::microseconds(frames * 1000000 / 48000);
      std::this_thread::sleep_until(next);
    }
  });

  // Main thread: a second transmitting channel comes and goes — E8's remove, many times.
  for (int i = 0; i < 40; ++i) {
    adapter.SetLocalChannelInfo(1, "churn", true, 1, true, true, false, 0);
    std::this_thread::sleep_for(60ms);
    adapter.removeLocalChannel(1);
    std::this_thread::sleep_for(40ms);
  }

  running = false;
  audio.join();
  CHECK(adapter.isConnected());
  adapter.disconnect();
}
