// ToneTag — marks audio with the sender interval it was recorded in.
//
// A sender with toneInput plays a sine on each input whose frequency encodes its
// interval number (k mod kToneTags), switching exactly at its interval boundary.
// After Vorbis and the receiver's mix, detectToneTag() reads the tag back from a
// short window of output, so a test can compare the interval a video frame came
// from with the interval of the audio playing at the moment it is shown.
#pragma once

#include <cmath>

namespace videosync {

constexpr int kToneTags = 16;

inline double toneFreqForTag(int tag) {
  int t = tag % kToneTags;
  if (t < 0) t += kToneTags;
  return 400.0 + 100.0 * t; // 400 .. 1900 Hz, 100 Hz apart
}

// Returns tag mod kToneTags, or -1 when the window holds no clear tone (silence).
inline int detectToneTag(const float *x, int n, int srate) {
  double energy = 0.0;
  for (int i = 0; i < n; ++i) energy += (double)x[i] * x[i];
  if (n <= 0 || energy / n < 1e-4) return -1;
  int best = -1;
  double bestP = 0.0, total = 0.0;
  for (int t = 0; t < kToneTags; ++t) {
    const double w = 2.0 * M_PI * toneFreqForTag(t) / srate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
      // Hann window keeps the 100 Hz neighbours apart
      const double win = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1));
      const double s0 = x[i] * win + c * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    total += p;
    if (p > bestP) { bestP = p; best = t; }
  }
  // a tag boundary inside the window splits the power between two tones
  return (total > 0.0 && bestP / total > 0.6) ? best : -1;
}

} // namespace videosync
