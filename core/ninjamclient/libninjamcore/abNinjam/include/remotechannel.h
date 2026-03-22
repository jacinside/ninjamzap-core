#ifndef REMOTECHANNEL_H
#define REMOTECHANNEL_H

#pragma once

#include <string>

namespace AbNinjam {
namespace Common {

class RemoteChannel {
public:
  RemoteChannel();

  // private:
  int id;
  std::string name;
  float volume;
  bool isStereo = false; // Indicates if the channel is stereo
  int channelPairIndex = -1;
  int flags = 0; // Channel flags from NINJAM protocol (0x10 = video-only)
};

} // namespace Common
} // namespace AbNinjam

#endif // REMOTECHANNEL_H
