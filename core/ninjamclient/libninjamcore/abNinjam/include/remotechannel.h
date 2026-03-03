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
};

} // namespace Common
} // namespace AbNinjam

#endif // REMOTECHANNEL_H
