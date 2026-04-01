// vorbisencdec_android.h
// Wrapper that provides vorbis includes for Android builds.
// The original vorbisencdec.h has hardcoded iOS xcframework paths.
// On Android, we include from the NDK-built or system vorbis headers instead.
//
// For initial builds WITHOUT vorbis: define NINJAM_NO_VORBIS to stub out
// the encoder/decoder. The app will connect and play metronome but won't
// encode/decode audio intervals.

#ifndef VORBISENCDEC_ANDROID_H
#define VORBISENCDEC_ANDROID_H

#ifdef NINJAM_NO_VORBIS

// Stub out vorbis types so njclient.cpp compiles without vorbis libraries.
// Audio encode/decode will be non-functional.
#include "WDL/queue.h"
#include "WDL/assocarray.h"

// Minimal stub classes matching what njclient.cpp expects
class VorbisEncoder {
public:
    VorbisEncoder(int srate, int nch, int serial, float qv, int maxbr = -1, int minbr = -1) {}
    void Encode(float *inbuf, int insamples, int advance = 1, int isinterleaved = 1) {}
    WDL_Queue m_outqueue;
};

class VorbisDecoder {
public:
    VorbisDecoder() {}
    void DecodeStuff(const char *buf, int len) {}
    void Reset() {}
    WDL_Queue m_samples;
    int GetSampleRate() { return 48000; }
    int GetNumChannels() { return 1; }
};

#else
// Real vorbis — include from system/NDK paths (must be on include path)
#include <vorbis/vorbisenc.h>
#include <vorbis/codec.h>
#include "WDL/queue.h"
#include "WDL/assocarray.h"

// Include the original implementation inline (it's a header-only impl)
// TODO: When vorbis libs are cross-compiled, include the real vorbisencdec.h
// with fixed paths. For now this will use the system headers.

#endif // NINJAM_NO_VORBIS

#endif // VORBISENCDEC_ANDROID_H
