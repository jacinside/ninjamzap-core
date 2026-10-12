// Scenario 30 — beat info from the server can't break the interval length.
//
// SERVER_CONFIG_CHANGE_NOTIFY carries BPM and BPI as uint16. The client turns
// them into the interval length in samples at the next interval boundary:
// srate * 60 * bpi / bpm. With bpm=0 that divides by zero (upstream NINJAM
// pull request #31); with large values (bpm=1, bpi=1000 at 48 kHz is 2.88e9
// samples) it no longer fits the int it is stored in. In both cases the
// client must keep the previous beat info.
//
// The interval engine runs in AudioProc, which only advances once the client
// is connected with audio enabled; the test subclass sets those two flags and
// calls updateBPMinfo directly. No server needed.
#include <vector>

#include "catch_amalgamated.hpp"

#include "njclient.h"

namespace {
const int kSrate = 48000;

class BeatClient : public NJClient {
public:
  BeatClient() {
    m_audio_enable = 1; // as if connected...
    m_max_localch = 1;  // ...to a server that allows a local channel
  }
  using NJClient::updateBPMinfo;

  // Run audio until the next interval boundary has been crossed, so pending
  // beat info is applied. A broken interval length (the bug under test) would
  // take hours of audio to reach: fail instead.
  void crossBoundary() {
    int pos = 0, len = 0;
    GetPosition(&pos, &len);
    REQUIRE(len > 0);
    REQUIRE(len - pos <= kSrate * 600);
    run(len - pos + 1);
  }

  int intervalLength() {
    int pos = 0, len = 0;
    GetPosition(&pos, &len);
    return len;
  }

private:
  void run(int samples) {
    const int block = 4096;
    std::vector<float> in(block, 0.0f), outL(block), outR(block);
    float *inbuf[1] = {in.data()};
    float *outbuf[2] = {outL.data(), outR.data()};
    while (samples > 0) {
      int n = samples < block ? samples : block;
      AudioProc(inbuf, 1, outbuf, 2, n, kSrate);
      samples -= n;
    }
  }
};
} // namespace

TEST_CASE("30_bpm_bpi_bounds — valid beat info sets the interval", "[scenario30]") {
  BeatClient c;
  c.updateBPMinfo(120, 8);
  c.crossBoundary();
  CHECK(c.intervalLength() == kSrate * 60 * 8 / 120);
  CHECK(c.GetBPI() == 8);
  CHECK((int)c.GetActualBPM() == 120);

  c.updateBPMinfo(240, 4);
  c.crossBoundary();
  CHECK(c.intervalLength() == kSrate * 60 * 4 / 240);
  CHECK(c.GetBPI() == 4);
}

TEST_CASE("30_bpm_bpi_bounds — zero bpm or bpi keeps the previous beat info", "[scenario30]") {
  BeatClient c;
  c.updateBPMinfo(120, 8);
  c.crossBoundary();
  const int before = c.intervalLength();

  c.updateBPMinfo(0, 8);
  c.crossBoundary();
  CHECK(c.intervalLength() == before);
  CHECK((int)c.GetActualBPM() == 120);

  c.updateBPMinfo(120, 0);
  c.crossBoundary();
  CHECK(c.intervalLength() == before);
  CHECK(c.GetBPI() == 8);
}

TEST_CASE("30_bpm_bpi_bounds — an interval too long for an int keeps the previous beat info",
          "[scenario30]") {
  BeatClient c;
  c.updateBPMinfo(120, 8);
  c.crossBoundary();
  const int before = c.intervalLength();

  c.updateBPMinfo(1, 1000); // 60000 s at 48 kHz = 2.88e9 samples
  c.crossBoundary();
  CHECK(c.intervalLength() == before);
  CHECK(c.GetBPI() == 8);
  CHECK((int)c.GetActualBPM() == 120);

  // and a valid update afterwards still applies
  c.updateBPMinfo(240, 4);
  c.crossBoundary();
  CHECK(c.intervalLength() == kSrate * 60 * 4 / 240);
}
