# NinjamZap Core

**A derivative of [Cockos NINJAM](https://ninjam.com) (GPL v2) — mobile-friendly C++ client core with cross-platform wrappers and a real-time video extension.**

The C++ NINJAM client (`core/ninjamclient/libninjamcore/njclient.{cpp,h}`,
WDL, mpb, netmsg) is taken from Cockos NINJAM by Justin Frankel. The iOS
bridge layer (`NinjamClientBridge`, `NinjamClientAdapter`) descends from
[abNinjam](https://github.com/antanasbruzas/abNinjam) by Antanas Bruzas.
Everything new — the video sync mechanism, the platform wrappers, the
multi-channel send pipeline, the docs — is added by Javier Cordero on
top under the same GPL v2 license, matching upstream so the whole
NINJAM ecosystem can interoperate without relicensing friction.
Full file-by-file lineage and licensing in [**NOTICE.md**](NOTICE.md).

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0)
[![Based on: Cockos NINJAM](https://img.shields.io/badge/based%20on-Cockos%20NINJAM-orange.svg)](https://ninjam.com)
[![Platform](https://img.shields.io/badge/Platform-iOS%20%7C%20Android-green.svg)]()

---

## What is NINJAM?

**NINJAM** (Novel Intervallic Network Audio Monitor) is a system for collaborative music-making over the internet. Musicians can jam together in real-time, with the system handling network latency through synchronized intervals.

Created by Justin Frankel and Cockos. Learn more: [ninjam.com](https://ninjam.com)

---

## Repository Structure

```
ninjamzap-core/
├── core/                          # Cross-platform C++ (GPL v2)
│   └── ninjamclient/
│       └── libninjamcore/         # NINJAM client core (njclient, WDL, Vorbis)
│
├── platforms/                     # Platform-specific wrappers (GPL v2)
│   ├── ios/
│   │   ├── NativeAudioModule/     # Obj-C++/Swift bridge
│   │   └── libs_compiled/         # Pre-built Vorbis xcframeworks
│   └── android/                   # Planned
│
├── LICENSE                        # GPL v2
└── NOTICE.md                      # Attributions
```

---

## Status

- **iOS**: Fully implemented (Obj-C++/Swift bridge, AVAudioEngine integration).
- **Android**: JNI bridge + Oboe full-duplex callback in place; cross-compiled
  Vorbis libraries are the remaining build dependency.

---

## Video Extension

NinjamZap adds **real-time video** alongside audio while staying on the standard
NINJAM interval-based protocol. Highlights:

- Video frames travel as additional channels carrying compressed payloads,
  identified by a four-character code (`H264`, `VP8`, `MJPG`).
- Audio is **never** disturbed: the matching server fork relays audio first
  in a two-pass loop and drops video frames for slow subscribers if needed.
- Any standard NINJAM client (Jamtaba, REAPER plugin, original NINJAM client)
  can join a NinjamZap room for **audio normally** — they simply don't see the
  video channels.
- The matching server fork (with two-pass audio prioritization, congestion
  control, and per-room threading) lives at
  [ninjamzap-server](https://github.com/jacinside/ninjamzap-server).

For the full technical reference — sync marker, GUID matching, adaptive
DS/PREV defer, threading model, wire format, and approaches that were
tried and rejected — see [`docs/VIDEO_SYNC.md`](docs/VIDEO_SYNC.md).

---

## Credits

- **NINJAM** by Justin Frankel and Cockos — [ninjam.com](https://ninjam.com) | [GitHub](https://github.com/justinfrankel/ninjam)
- **abNinjam** by Antanas Bruzas — [GitHub](https://github.com/antanasbruzas/abNinjam)
- **Vorbis/Ogg** by Xiph.Org Foundation — [xiph.org](https://xiph.org/vorbis/)

Full attributions in [NOTICE.md](NOTICE.md).

---

## License

GNU General Public License v2.0. See [LICENSE](LICENSE).

```
NinjamZap Core - GPL-licensed NINJAM Client
Copyright (C) 2025 Javier Alejandro Cordero
Based on NINJAM by Cockos/Justin Frankel
```
