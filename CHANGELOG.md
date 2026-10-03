# Changelog

All notable changes to NinjamZap Core are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning policy (while in `0.x`, the wire format may still change between
minor releases — once it stabilizes the project moves to `1.0.0`):

- **MAJOR** — wire format or sync semantics change in a way that breaks
  compatibility with prior implementations.
- **MINOR** — backward-compatible additions to the spec (new optional
  fields, new sections in `docs/VIDEO_SYNC.md`, new public API).
- **PATCH** — receiver/sender bug fixes that do not alter the wire format
  or spec.

## [0.3.2] — 2026-10-02

### Added

- **Session archive** — `NinjamClientAdapter::startSessionArchive(dir)` /
  `stopSessionArchive()` write the classic NINJAM / ReaNinjam session folder:
  every remote and local interval kept as `<dir>/<0-f>/<guid>.OGG`, indexed in
  `<dir>/clipsort.log` (closed with `end` on stop). Video channels are not
  archived and keep flowing while archiving.

## [0.3.1] — 2026-10-02

### Added

- **`NinjamClientAdapter::getMaxLocalChannels()`** — the number of local
  channels the server lets this user transmit (its `maxchan`, known after the
  auth reply). Channels whose index reaches it are silently not sent, so hosts
  can cap how many local channels the user may add.

## [0.3.0] — 2026-10-02

### Added

- **`processAudioOutN` — N inputs to N outputs.** Like `processAudioN` but the
  output is a caller-defined set of `outnch` deinterleaved channels instead of
  fixed stereo + metronome, so remote channels (`SetUserChannelState` with
  `setoutch`), local monitors and the metronome (`setMetronomeChannel`) land on
  whichever output channel or pair they are routed to. Typical layout: hardware
  outputs `0..H-1` plus the metronome alone on channel `H` (click-free
  recording, metronome placed by the host).

### Changed

- **Up to 64 input channels** in `processAudioN` / `processAudioOutN` (was 16) —
  large interfaces and macOS aggregates expose 18–32+ channels. Exposed as
  `NinjamClientAdapter::kMaxIOChannels`.

## [0.2.3] — 2026-10-01

### Fixed

- **Video tagged with the translated channel index.** Since 0.2.2 the adapter
  announces each local channel on a contiguous NINJAM `channel_idx`, but
  `setVideoChannel` / `rawDataSendBegin` still passed the caller-facing id. A
  sender with a second audio channel created before the camera announced its
  video channel (flags `0x10`) on slot 2 while tagging the video chunks with
  `chidx` 1, so receivers could not match the stream to its channel and showed
  no video. Both calls now go through the same id → `channel_idx` translation.

## [0.2.2] — 2026-08-08

### Fixed

- **Contiguous local channel indices.** The adapter now maps each caller-facing
  local channel id to a contiguous NINJAM `channel_idx` (0, 1, 2, …) instead of
  passing sparse ids straight through. NINJAM identifies local channels by
  `channel_idx` and rejects any whose index is `>= maxchan` advertised by the
  server, so a second local channel created with a non-contiguous id would be
  captured but never transmitted (its meter moved yet no audio was sent). Ids are
  translated on every per-channel call and the slot is freed on removal; a
  diagnostic warns when an allocated index would still exceed the server limit.

### Compatibility

- No wire-format change. Behavior fix only.

## [0.2.1] — 2026-08-04

### Fixed

- **Voice-chat (live) video A/V sync.** When a sender's audio channel runs in
  NINJAM voice-chat mode (`flags & 2`), its audio streams continuously behind a
  ~0.75 s receiver jitter buffer, so delivering video frames on arrival ran the
  picture ahead of the sound. Video frames now pass through a matching
  receiver-side delay line and are released in sync with that audio, without the
  interval quantization of the accumulate/SWAP path.

### Compatibility

- Receiver-side only. No wire-format or API change — source and binary
  compatible with `v0.2.0`.

## [0.2.0] — 2026-06-17

### Changed

- **Relicensed from GPL v3 to GPL v2** to match upstream Cockos NINJAM.
  The v3 designation was elective — none of the bundled components
  require it (NJClient is "v2 or later", WDL is Zlib-style permissive,
  Vorbis is BSD). Staying on v2 keeps the whole NINJAM ecosystem
  interoperable so downstream consumers can import or submodule this
  code without relicensing.

### Compatibility

- No wire-format or API change. Binary and source compatible with
  `v0.1.0` — implementers pinned to `v0.1.0` may bump directly with
  no integration changes.

## [0.1.0] — 2026-06-03

First tagged release. Snapshot of the public reference implementation and
spec for cross-client video over NINJAM, suitable for other clients to
pin and integrate against.

### Included

- C++ NJClient with NINJAM protocol, audio + video channel support.
- Per-user `VideoRecvState` machine: interval-aligned reassembly, GUID
  matching against the corresponding audio interval, drop-resync on
  sustained mismatch.
- Adapter layer (`abNinjam/ninjamclientAdapter.cpp`) exposing a thin C++
  surface for embedding.
- Pre-built Vorbis xcframeworks for iOS (BSD-licensed).
- `docs/VIDEO_SYNC.md` — full spec and implementer's guide for
  cross-client video, including wire format, sync mechanism, and §10
  common pitfalls observed during real interop testing.

### Receiver fixes folded in for this release

- Audio GUID lookup now scans all of a user's non-video channels rather
  than hardcoding channel 0. Required for interop with senders that
  place audio on non-zero channel indices (e.g. video on channel 1,
  audio on channels 2/3).

### Known limitations

- No top-level CMake build yet; consumers vendor the source tree.
- Wire format documented and stable across this 0.x line in intent, but
  not guaranteed stable until `1.0.0`.
