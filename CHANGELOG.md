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
