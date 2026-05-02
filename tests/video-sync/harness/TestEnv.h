// Globals captured from the environment before Catch2 starts. Each scenario
// reads these to know which docker-hosted ninjamsrv to talk to.
#pragma once

#include <string>

namespace videosync::testenv {

inline std::string host;
inline int port = 0;

} // namespace videosync::testenv
