# Synchronized Video over NINJAM — Technical Overview

*How NinjamZap carries live video inside the NINJAM protocol and keeps it
aligned with audio. Written for NINJAM client developers and other engineers
familiar with the interval model.*

> **Repo scope.** The interval-sync engine described here lives in this
> repository: the C++ NINJAM core (`core/ninjamclient/libninjamcore/`) and the
> iOS bridge (`platforms/ios/NativeAudioModule/`). The camera-capture, H.264
> encode/decode, and playback-pacing layer is platform app code and is *not*
> in this repo — it is described conceptually below. End-to-end integration
> tests for the sync engine are in `tests/video-sync/`.

---

## 1. The problem

NINJAM solves real-time musical collaboration by **giving up on real-time**.
Musicians record during interval *N* and everyone hears interval *N‑1*. The
latency stops being a glitch and becomes part of the musical form.

We wanted to add live video of the players to a NINJAM session. The constraint:
**video must obey the exact same interval model as audio.** If you hear my
guitar from the previous interval, you must see me playing it in that same
previous interval — not earlier, not later. A talking head that drifts a beat
away from the audio is worse than no video at all.

The naïve approaches (WebRTC, a side channel, a separate socket) all fail the
same way: video and audio travel on different transports with different
latencies, so they drift. Our design goal was to make drift *structurally
impossible* by putting video on the **same transport, same clock, and same
interval pipeline** as the audio.

---

## 2. Key insight: the NINJAM server is codec-agnostic

Every interval upload in NINJAM is tagged with a 4-byte `fourcc` codec
identifier. Standard clients hardcode this to `OGGv` (Ogg Vorbis). But the
server never inspects it — it stores and relays interval data as **opaque
bytes**, regardless of `fourcc`.

That means a NINJAM server already is, unmodified, a generic interval-synced
data relay. We don't need a custom server. We open a second channel on the
local user, mark it as a video channel, and push H.264 through the existing
interval upload/download machinery. Third-party clients (ReaNINJAM, Jamtaba)
see the channel in the user list, try to subscribe, fail to decode a non-audio
`fourcc`, and produce silence — no crashes, full backward compatibility.

> **Server changes required: zero.** Everything described below lives in the
> client. (NinjamZap *also* runs a server fork — `ninjamzap-server` — but that
> adds per-room threading and congestion control for scale, not protocol
> changes. Video sync works against a stock NINJAM server.)

---

## 3. Architecture at a glance

```
SENDER                                              RECEIVER
───────                                             ────────
camera ──▶ H.264 encoder (app code)                 C++ Run() thread
                  │                                   receives raw data
                  ▼                                        │
        QueueVideoFrame()  ──┐                              ▼
                             │                    decode H.264 incrementally
   on_new_interval() ────────┤  NINJAM              into per-user buffers
   [C++ audio thread]        │  protocol                    │
     • END prev interval     ├──▶ server ──────────▶         ▼
     • BEGIN new interval    │  (opaque relay)     on_new_interval()
     • send 24-byte marker   │                     [receiver's local clock]
     • send cached SPS/PPS ──┘                       • GUID-match video↔audio
                                                     • swap buffer → playback
```

```mermaid
flowchart LR
    subgraph SENDER
        cam[Camera] --> enc[H.264 encoder<br/>app code]
        enc --> qf["QueueVideoFrame()"]
        oni["on_new_interval()<br/>C++ audio thread"]
        oni -. "END prev / BEGIN new<br/>24-byte marker + SPS/PPS" .-> qf
    end

    subgraph SERVER["NINJAM server"]
        relay[Opaque byte relay<br/>codec-agnostic]
    end

    subgraph RECEIVER
        run["C++ Run() thread<br/>receives raw data"]
        dec[Decode H.264 incrementally<br/>per-user buffers]
        roni["on_new_interval()<br/>receiver's local clock"]
        play[GUID-match video↔audio<br/>swap buffer → playback]
        run --> dec --> roni --> play
    end

    qf --> relay
    oni --> relay
    relay --> run
```

The two load-bearing decisions:

1. **Interval BEGIN/END are emitted from C++ `on_new_interval()`** — the *same
   thread, same instant* as the audio interval boundary. Video and audio
   intervals are literally created by the same function call.
2. **The receiver swaps video on its own local clock**, not when the sender's
   END marker arrives over the network. This mirrors how NINJAM audio already
   works and is the single most important reason the latency is low.

---

## 4. Sender side

All interval lifecycle lives in C++, on the audio thread
(`core/ninjamclient/libninjamcore/njclient.cpp`):

```cpp
// Inside on_new_interval(), before the audio interval-swap callback:
if (m_video_active) {
    if (m_video_interval_open)
        RawDataSendWrite(m_video_guid, NULL, 0, true);          // END previous
    RawDataSendBegin(m_video_guid, m_video_fourcc, m_video_chidx, 0); // BEGIN new
    m_video_interval_open = true;

    // First chunk of every interval: the 20-byte sync marker (see §6)
    // Second chunk: cached SPS/PPS so the decoder can be (re)built mid-stream
    RawDataSendWrite(m_video_guid, m_video_spspps.Get(), m_video_spspps.GetSize(), false);
}
```

The camera runs on its own capture queue (app code). Encoded frames are handed
to `QueueVideoFrame()`, which appends them to the open interval — but **only if
`m_video_interval_open` is true**. Frames that arrive between intervals are
dropped rather than misattributed.

Encoding parameters (low preset): 320×240, 10 fps, ~50 kbps H.264 baseline, no
B-frames, at least one keyframe per interval. At 10 fps an 8-second interval is
~80 frames — small enough to relay comfortably within typical NINJAM server
buffer limits. (NinjamZap's app layer also offers VGA and HD presets; the sync
engine is resolution-agnostic.)

---

## 5. Receiver side

Two threads cooperate:

**C++ `Run()` thread** receives raw data callbacks and decodes H.264
*incrementally* as bytes arrive — it does not wait for the interval's END
marker. Decoded frames accumulate in a per-user buffer.

**The receiver's own `on_new_interval()`** fires the buffer swap. At that
moment it harvests whatever frames have been decoded so far — exactly as NINJAM
audio takes whatever PCM has accumulated — and promotes them to the playback
buffer. A playback timer (app code) then paces those frames evenly across the
next interval.

Because the swap is driven by the local clock and harvests in-progress
buffers, the receiver never pays a full network round-trip of latency waiting
for an END marker.

The per-interval timing — and the 1-interval delay that makes a `prev` GUID
match the *expected* case — looks like this:

```mermaid
sequenceDiagram
    participant S as Sender on_new_interval()
    participant Sv as NINJAM server
    participant R as Receiver Run() thread
    participant Rs as Receiver on_new_interval()

    Note over S: Interval N begins
    S->>Sv: END interval N-1
    S->>Sv: BEGIN N + marker(GUID of N-1) + SPS/PPS
    loop frames during interval N
        S->>Sv: H.264 frame
        Sv->>R: relay frame
        R->>R: decode incrementally
    end
    Note over Rs: Interval N begins (local clock)
    Rs->>Rs: audio swap: ds = N-1, prev_ds_guid = N-2
    Rs->>Rs: video marker GUID == prev_ds_guid → PLAY
    Note over Rs: plays interval N-1 video + audio together
```

---

## 6. The sync mechanism: GUID matching

Driving video and audio off the same interval clock gets them *close*. To make
them *exact*, every video interval carries a pointer back to the audio interval
it was recorded alongside.

NINJAM already generates a fresh 16-byte GUID for each audio interval (when the
encoder writes a new interval header). We piggyback on it. The **first chunk**
of every video interval is a 24-byte sync marker (a 20-byte inner payload
wrapped in the standard 4-byte BE length prefix that every chunk uses — see
§7):

```
Bytes 0–3   : 0x00 00 00 14         ← outer length prefix (20, big-endian)
Bytes 4–7   : interval counter      ← uint32 big-endian
Bytes 8–23  : audio channel-0 GUID  ← the GUID of the audio interval
                                      recorded at the same instant
```

On the receiver, audio decode state already tracks two GUIDs: the currently
playing interval (`ds`) and the previously played one (`prev_ds_guid`). At each
`on_new_interval()`, the receiver compares the video marker's audio GUID
against both:

```mermaid
flowchart TD
    start([on_new_interval:<br/>video marker GUID]) --> hasaudio{audio data<br/>available?}
    hasaudio -- no --> hold1[HOLD<br/>reset hold counter]
    hasaudio -- yes --> match{GUID matches?}
    match -- "matches prev_ds_guid" --> playprev[PLAY immediately<br/>covers SWAP..SWAP+1]
    match -- "matches ds" --> playds[DEFER 1 swap → pending<br/>PLAY at SWAP+1<br/>covers SWAP+1..SWAP+2]
    match -- "no match" --> count{holdCount &lt; 3?}
    count -- yes --> hold2[HOLD<br/>wait for audio to catch up]
    count -- no --> fallback[PLAY-FALLBACK<br/>force, don't freeze forever]
```

**The adaptive defer — why both `PREV` and `DS` are handled, not just one:**
NINJAM's 1-interval delay means that when the receiver swaps at interval *N*,
the audio it begins playing was recorded at *N‑1* (`ds` after swap) and the
audio just finishing in the output buffer was recorded at *N‑2*
(`prev_ds_guid`). The video marker's GUID can land on either, and the receiver
adapts:

- **`PREV` match** — the marker references the audio that was already audible
  during `[SWAP-1, SWAP]` and is finishing playback through the output buffer
  during `[SWAP, SWAP+1]`. Play video **immediately** so it covers the same
  `[SWAP, SWAP+1]` window. Aligned.
- **`DS` match** — the marker references the audio that is *in the decoder
  pipeline now* but only becomes audible during `[SWAP+1, SWAP+2]` due to the
  output buffer. Stage video into `pending` and play it on the **next** swap,
  so video covers the same `[SWAP+1, SWAP+2]` window. Aligned.

Either branch ends up perfectly synchronised — the receiver doesn't need to
care which branch fired, only that the GUID matched something. The HOLD /
FALLBACK ladder underneath absorbs transient network jitter without ever
letting video freeze permanently.

---

## 7. Wire format

Each video interval is an ordered series of **chunks**, each sent as one (or
more — see fragmentation note) NINJAM `UPLOAD_INTERVAL_WRITE` messages. **Every
chunk's payload begins with a 4-byte big-endian length prefix** giving the
size of the inner payload that follows. The receiver's frame reassembler reads
this prefix to identify logical chunk boundaries regardless of how NINJAM's
transport fragmented the WRITE.

The chunks, in order per interval:

```
Chunk 1 — Sync marker (24 bytes total on the wire):
  [4B BE = 20]                   ← inner payload length
  [4B BE interval counter]       ← uint32, sender's interval index
  [16B audio channel-0 GUID]     ← the 16 bytes from the audio interval
                                   being recorded right now on this client

Chunk 2 — SPS/PPS block:
  [4B BE = inner length]
  [2B BE SPS len][SPS NAL bytes]
  [2B BE PPS len][PPS NAL bytes]

Chunk 3..N — H.264 frame (one per encoded frame):
  [4B BE = frame length]
  [AVCC bytes from the encoder]  ← standard H.264 with its own 4-byte
                                   NAL length prefixes inside
```

> **Two layers of length prefixes in frame chunks** — the **outer** 4-byte BE
> length is the chunk wrapper (required by our receiver). The **inner** 4-byte
> BE prefixes (one per NAL unit) are standard H.264 AVCC format, which the
> encoder produces and the decoder consumes verbatim. Pass AVCC bytes through
> unmodified and add only the outer length wrapper.

Channel is flagged video-only (`flags & 0x10` in `SET_CHANNEL_INFO`) so other
clients' audio pipelines skip it. `fourcc` is `H264` for this design; the
relay also accepts `VP8 ` and `MJPG` — the sync mechanism is codec-agnostic.

---

## 8. Results

- **End-to-end video latency vs. audio: ~0.5–1 beat.** Residual delay comes
  from main-thread dispatch hops and the playback timer's first tick, not from
  the sync design itself.
- **Drift: none observed.** Because both ends run off the server's BPM/BPI and
  every interval is GUID-stamped, video cannot slowly walk away from audio.
- **Backward compatible:** standard NINJAM clients on the same server are
  unaffected — they see an undecodable channel and ignore it.
- **No server modification, no second transport, no signaling server.**

---

## 9. Things that did *not* work (and why)

These were tried and rejected — included so others don't repeat them:

| Approach | Outcome |
|----------|---------|
| Beat-sync timer in app code driving BEGIN/END | 2–3 beats late — main-thread polling detects the boundary too slowly |
| Early END (hardcoded N beats / % before boundary) | Fragile; breaks at different BPM/BPI |
| Receiver plays only when the network END marker arrives | ~10 beats — pays a full interval of network latency |
| Synchronous H.264 decode on the C++ `Run()` thread | ~10 beats — blocks network I/O, data piles up |
| Tracking intervals with a sequence counter instead of GUIDs | False positives during bursts (e.g. after a video on/off toggle) |
| Recreating the decoder every interval | Visible stutter from 15×/sec session churn |

The throughline: **anything that introduces a second clock, a second
transport, or a network-dependent trigger reintroduces drift or latency.** The
working design keeps video on exactly the rails NINJAM already built for audio.

---

## 10. Implementer's guide

If you're building a NINJAM-compatible client and want to send or receive
synchronized video, here is the protocol-level recipe — everything you need
without having to read this codebase.

### Message mapping

| What                              | NINJAM message                          | ID   |
|-----------------------------------|-----------------------------------------|------|
| Announce a (video) channel        | `MESSAGE_CLIENT_SET_CHANNEL_INFO`       | 0x82 |
| Subscribe to a user's channels    | `MESSAGE_CLIENT_SET_USERMASK`           | 0x81 |
| Start a per-interval upload       | `MESSAGE_CLIENT_UPLOAD_INTERVAL_BEGIN`  | 0x83 |
| Send one chunk of interval data   | `MESSAGE_CLIENT_UPLOAD_INTERVAL_WRITE`  | 0x84 |
| End an interval                   | `UPLOAD_INTERVAL_WRITE` with `flags & 1`| 0x84 |

In our code these map to `RawDataSendBegin()` / `RawDataSendWrite()` in
`njclient.cpp` — but those are just wrappers over the standard NINJAM messages
above. The server (stock NINJAM, or our fork) doesn't need any custom support
to relay video.

### Advertising a video channel (sender)

Send `SET_CHANNEL_INFO` (0x82) with a record per local channel. Per-channel
format:

```
[name (null-terminated UTF-8)]
[2B volume LE]
[1B pan]
[1B flags]                  ← set bit 0x10 for video-only
```

Standard NINJAM clients treat `flags & 0x10` as "skip this channel for audio
mixing" — they will list the channel in the userlist but pass it over in the
audio pipeline. The 4CC for the interval uploads (next step) is `H264`,
`VP8 ` (note the trailing space), or `MJPG`.

### Subscribing to a video channel (receiver)

`SET_USERMASK` (0x81) per user carries a 4-byte LE bitmask of channel indices.
Bit *n* enables channel *n*. There is no automatic separation between audio
and video — you must set the bit for the video channel index **explicitly** to
receive its data.

### Per-interval send sequence (sender)

On every audio interval boundary, on the same thread that emits the audio
interval:

1. If a video interval is already open, send an **END** for it:
   `UPLOAD_INTERVAL_WRITE` with the old GUID, empty payload, `flags & 1` set.
2. Send `UPLOAD_INTERVAL_BEGIN` for the new interval:
   - 16 B GUID (fresh random — see GUID lifecycle below)
   - 4 B estimated size (can be 0)
   - 4 B `fourcc` (e.g. `H264`)
   - 1 B channel index
3. Send the **sync marker** as one `UPLOAD_INTERVAL_WRITE`. Payload (24 bytes):
   - 4 B BE `0x00 00 00 14` (= 20, the inner length)
   - 4 B BE interval counter
   - 16 B audio channel-0 GUID (see "Audio GUID source")
4. Send the cached **SPS/PPS block** as a second `UPLOAD_INTERVAL_WRITE`.
   Payload: `[4B BE inner length][2B BE SPS len][SPS NAL][2B BE PPS len][PPS NAL]`.
5. As the encoder produces frames during the interval, send each as its own
   `UPLOAD_INTERVAL_WRITE`. Payload: `[4B BE frame length][AVCC bytes]`.

**Every chunk payload begins with a 4-byte big-endian length prefix — this is
a hard requirement.** The receiver's frame reassembler reads the first 4
bytes of each logical chunk as a length to identify chunk boundaries. Forget
it and the receiver can't tell where one chunk ends and the next begins.

Each `UPLOAD_INTERVAL_WRITE` message carries `[16B GUID][1B flags][N bytes
data]` in its NINJAM payload; the N bytes are your chunk (already prefixed as
above). Writes larger than 9216 bytes (`MAX_ENC_BLOCKSIZE`) are automatically
fragmented into multiple messages by the protocol layer; only the last
fragment carries `flags & 1` (END). The receiver concatenates fragments by
GUID before applying the outer-length parser.

### GUID lifecycle

- **Video GUID** — fresh 16 random bytes per video interval, generated on the
  sender right before sending `UPLOAD_INTERVAL_BEGIN`. In our code we use
  `WDL_RNG_bytes(guid, 16)` (the same RNG NINJAM uses for audio).
- **Audio GUID source for the sync marker** — read the GUID that the local
  audio channel 0 has in its *current* upload-interval header. In our code:
  `m_curwritefile.guid` for audio channel 0. In protocol terms: it's the same
  16 bytes you passed to your most recent audio `UPLOAD_INTERVAL_BEGIN` for
  channel 0. Both ends of the connection see the same 16 bytes; that
  identity is what makes the GUID match work at the receiver.

### Decoder build / rebuild policy (receiver)

- **Build the decoder once** when the first SPS/PPS arrives.
- **Rebuild only when the SPS bytes change** (resolution change, encoder
  reset). The SPS/PPS is sent every interval as a recovery aid for
  late-joining receivers and for decoder loss, *not* as a signal to reset.
  Rebuilding every interval will cause visible stutter (we tried — see §9).
- **The first frame after BEGIN should be an IDR keyframe.** Force one on
  every interval boundary on the sender. If you receive only P-frames and
  have no SPS/PPS yet, hold playback for that user until the next interval
  delivers them.

### Common pitfalls

- **Forgetting the outer 4-byte BE length prefix on chunks.** Every chunk
  payload (marker, SPS/PPS, every frame) must start with a 4-byte big-endian
  length giving the bytes that follow. The receiver's reassembler reads this
  to identify logical chunk boundaries — without it, parsing breaks at the
  first chunk. The H.264 AVCC 4-byte NAL prefixes *inside* frames are codec
  format and live underneath this outer wrapper; they are not a substitute.
- **Output pixel format mismatch.** Hardware decoders frequently return NV12
  while a renderer expects BGRA / I420. This renders as a uniform color and
  looks exactly like decoder failure. Always check the `CVPixelBuffer` /
  surface format before debugging the decoder.
- **Forgetting `flags & 0x10`** on the local channel makes your own client
  try to mix video bytes as audio at playback (audible noise). On the
  receiving side, other clients without `0x10` awareness *also* try to mix
  the bytes — that's why standard NINJAM clients gracefully fall back to
  silence: their Vorbis decoder rejects the input.
- **Explicit USERMASK bit.** A client that subscribes to a user but doesn't
  set the bit for the video channel will simply never receive the data — no
  error, just silence.
- **Public-server channel caps.** Public NINJAM servers configure
  `MaxChannels <normal> <anon>` and most ship with `MaxChannels 32 2` (the
  example config default). Video is a full channel slot — stereo audio +
  video = 3 channels, which exceeds an anon cap of 2. NinjamZap's own server
  (`video.ninjamzap.com:2049` and `:2050`) has the anon cap raised to 8 so
  third-party clients can test video against it.

---

## Source map

| Component | Location |
|-----------|----------|
| Interval management, video GUID sync, `on_new_interval()` video block | `core/ninjamclient/libninjamcore/njclient.cpp` / `.h` |
| Adapter passthrough (`setVideoChannel`, `queueVideoFrame`, …) | `core/ninjamclient/libninjamcore/abNinjam/ninjamclientAdapter.cpp` / `.h` |
| iOS C bridge | `platforms/ios/NativeAudioModule/NinjamClientBridge.cpp` / `.h` |
| Camera capture, H.264 encode/decode, playback pacing | Platform app code — *not in this repo* |
| End-to-end sync tests (26 Catch2 scenarios, Docker NINJAM server) | `tests/video-sync/` |

*This repository is GPL v3.*
