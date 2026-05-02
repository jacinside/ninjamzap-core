// FakeFrame — synthetic video payload that encodes seq, sender's interval index,
// and the steady_clock micro-timestamp at the moment the sender called QueueVideoFrame.
//
// Layout: [4B 'TEST'][4B BE seq][4B BE intervalIdx][8B BE sendTimestampUs][N bytes padding]
// Tests use sendTimestampUs to compute the actual per-frame send→deliver latency on the
// receiver and assert it is at least one interval duration (NINJAM's design).
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

namespace videosync {

constexpr uint32_t kFakeFrameMagic = 0x54455354; // 'TEST'
constexpr int kFakeFrameHeaderBytes = 4 + 4 + 4 + 8; // magic + seq + intervalIdx + tsUs

struct FakeFramePayload {
  uint32_t seq = 0;
  uint32_t intervalIdx = 0;
  uint64_t sendTimestampUs = 0;
};

inline uint64_t nowMicros() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline std::vector<uint8_t> makeFakeFrame(uint32_t seq, uint32_t intervalIdx,
                                          int paddingBytes = 256,
                                          uint64_t sendTimestampUs = 0) {
  std::vector<uint8_t> out(kFakeFrameHeaderBytes + paddingBytes, 0);
  // magic
  out[0] = (kFakeFrameMagic >> 24) & 0xff;
  out[1] = (kFakeFrameMagic >> 16) & 0xff;
  out[2] = (kFakeFrameMagic >> 8) & 0xff;
  out[3] = kFakeFrameMagic & 0xff;
  // seq
  out[4] = (seq >> 24) & 0xff;
  out[5] = (seq >> 16) & 0xff;
  out[6] = (seq >> 8) & 0xff;
  out[7] = seq & 0xff;
  // intervalIdx
  out[8]  = (intervalIdx >> 24) & 0xff;
  out[9]  = (intervalIdx >> 16) & 0xff;
  out[10] = (intervalIdx >> 8) & 0xff;
  out[11] = intervalIdx & 0xff;
  // sendTimestampUs (BE u64)
  if (sendTimestampUs == 0) sendTimestampUs = nowMicros();
  for (int i = 0; i < 8; ++i) {
    out[12 + i] = (uint8_t)((sendTimestampUs >> (56 - i * 8)) & 0xff);
  }
  return out;
}

inline bool parseFakeFrame(const uint8_t *data, int len, FakeFramePayload &out) {
  if (len < kFakeFrameHeaderBytes) return false;
  uint32_t magic = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] << 8) | (uint32_t)data[3];
  if (magic != kFakeFrameMagic) return false;
  out.seq = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
            ((uint32_t)data[6] << 8) | (uint32_t)data[7];
  out.intervalIdx = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) |
                    ((uint32_t)data[10] << 8) | (uint32_t)data[11];
  uint64_t ts = 0;
  for (int i = 0; i < 8; ++i) {
    ts = (ts << 8) | (uint64_t)data[12 + i];
  }
  out.sendTimestampUs = ts;
  return true;
}

} // namespace videosync
