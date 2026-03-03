# NinjamZap Core

**GPL-licensed NINJAM Client Core for Mobile Platforms**

Cross-platform C++ NINJAM implementation with platform-specific wrappers for iOS (and future Android support).

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/Platform-iOS%20%7C%20Android-green.svg)]()

---

## What is NINJAM?

**NINJAM** (Novel Intervallic Network Audio Monitor) is a system for collaborative music-making over the internet. Musicians can jam together in real-time, with the system handling network latency through synchronized intervals.

Created by Justin Frankel and Cockos. Learn more: [ninjam.com](https://ninjam.com)

---

## Repository Structure

```
ninjamzap-core/
├── core/                          # Cross-platform C++ (GPL v3)
│   └── ninjamclient/
│       └── libninjamcore/         # NINJAM client core (njclient, WDL, Vorbis)
│
├── platforms/                     # Platform-specific wrappers (GPL v3)
│   ├── ios/
│   │   ├── NativeAudioModule/     # Obj-C++/Swift bridge
│   │   └── libs_compiled/         # Pre-built Vorbis xcframeworks
│   └── android/                   # Planned
│
├── LICENSE                        # GPL v3
└── NOTICE.md                      # Attributions
```

---

## Status

- **iOS**: Fully implemented
- **Android**: Planned (C++ core is ready, JNI wrapper needed)

---

## Credits

- **NINJAM** by Justin Frankel and Cockos — [ninjam.com](https://ninjam.com) | [GitHub](https://github.com/justinfrankel/ninjam)
- **abNinjam** by Antanas Bruzas — [GitHub](https://github.com/antanasbruzas/abNinjam)
- **Vorbis/Ogg** by Xiph.Org Foundation — [xiph.org](https://xiph.org/vorbis/)

Full attributions in [NOTICE.md](NOTICE.md).

---

## License

GNU General Public License v3.0. See [LICENSE](LICENSE).

```
NinjamZap Core - GPL-licensed NINJAM Client
Copyright (C) 2025 Javier Cordero
Based on NINJAM by Cockos/Justin Frankel
```
