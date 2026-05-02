// SyncLogCapture — intercepts every SYNCLOG line emitted by njclient.cpp.
//
// On Apple builds njclient.cpp routes SYNCLOG through synclog_emit_oslog. We provide
// that symbol from this translation unit, instead of LoggerBridge.mm. Lines are pushed
// to a thread-safe ring + waiters. Tests query: hasLine(regex) / waitForLine(regex, timeout).
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

namespace videosync {

class SyncLogCapture {
public:
  static SyncLogCapture &instance();

  void clear();

  // Returns true if any captured line matches `pattern` (extended POSIX regex).
  bool hasLine(const std::string &pattern) const;

  // Block up to `timeout` waiting for a line matching `pattern`. Returns the
  // matched line, or empty string on timeout.
  std::string waitForLine(const std::string &pattern,
                          std::chrono::milliseconds timeout);

  // Returns all captured lines matching `pattern`, in arrival order.
  std::vector<std::string> match(const std::string &pattern) const;

  // Snapshot of all captured lines.
  std::vector<std::string> all() const;

  // When echo is true, captured lines are also printed to stderr (default off
  // to keep test output clean). Useful when debugging a flaky scenario.
  void setEcho(bool on);

  // Internal: called from the synclog_emit_oslog C symbol.
  void push(const char *line);

private:
  SyncLogCapture() = default;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::string> lines_;
  bool echo_ = false;
};

} // namespace videosync
