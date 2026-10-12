// Scenario 33 — copying a video buffer while a frame is still arriving keeps that
// frame's start offset.
//
// A frame larger than one upload write arrives in several WRITEs; until the last one,
// pending_remaining > 0 and frameOffsets holds frameCount + 1 entries (the last is
// the in-progress frame's start). Buffers are copied at interval swaps (next ->
// pending when the video matches its audio), which is often mid-frame with HD
// video. copyFrom copied only frameCount offsets, so when
// the frame completed in the copy its start was read past the copied offsets: a
// garbage frame size, and the frame callback read out of bounds (EXC_BAD_ACCESS in
// memmove inside VideoFrameReady_Callback). No server needed.
#include <cstring>

#include "catch_amalgamated.hpp"

#include "njclient.h"

namespace {
struct Access : NJClient {
  using Buf = NJClient::VideoRecvBuffer;
};
} // namespace

TEST_CASE("33_video_buffer_copy_mid_frame — copyFrom keeps the in-progress frame's offset",
          "[scenario33]") {
  Access::Buf src;
  // One complete 10-byte frame at offset 0, and a second frame started at offset 10
  // with 30 of its bytes still to arrive.
  src.active = true;
  src.data.Resize(10 + 6, false);
  memset(src.data.Get(), 1, src.data.GetSize());
  src.frameOffsets.Resize(2, false);
  src.frameOffsets.Get()[0] = 0;
  src.frameOffsets.Get()[1] = 10;
  src.frameCount = 1;
  src.pending_remaining = 30;

  // Copy into a buffer that held older, longer data: its offset storage has stale
  // values past the copied entries.
  Access::Buf dst;
  dst.frameOffsets.Resize(8, false);
  for (int i = 0; i < 8; i++) dst.frameOffsets.Get()[i] = 100000 + i;
  dst.copyFrom(src);

  REQUIRE(dst.frameCount == 1);
  REQUIRE(dst.pending_remaining == 30);
  REQUIRE(dst.frameOffsets.GetSize() >= 2);
  CHECK(dst.frameOffsets.Get()[1] == 10); // the in-progress frame still starts at 10
}
