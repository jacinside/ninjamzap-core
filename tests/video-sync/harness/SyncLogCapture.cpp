#include "SyncLogCapture.h"

#include <cstdio>
#include <cstring>

namespace videosync {

SyncLogCapture &SyncLogCapture::instance() {
  static SyncLogCapture inst;
  return inst;
}

void SyncLogCapture::clear() {
  std::lock_guard<std::mutex> lk(m_);
  lines_.clear();
}

void SyncLogCapture::setEcho(bool on) {
  std::lock_guard<std::mutex> lk(m_);
  echo_ = on;
}

void SyncLogCapture::push(const char *line) {
  if (!line) return;
  std::string s(line);
  bool echoNow;
  {
    std::lock_guard<std::mutex> lk(m_);
    lines_.emplace_back(std::move(s));
    // Bound the ring to keep memory predictable across long test runs.
    while (lines_.size() > 8192) lines_.pop_front();
    echoNow = echo_;
  }
  if (echoNow) std::fprintf(stderr, "%s\n", line);
  cv_.notify_all();
}

bool SyncLogCapture::hasLine(const std::string &pattern) const {
  std::regex re(pattern);
  std::lock_guard<std::mutex> lk(m_);
  for (const auto &l : lines_) {
    if (std::regex_search(l, re)) return true;
  }
  return false;
}

std::vector<std::string> SyncLogCapture::match(const std::string &pattern) const {
  std::regex re(pattern);
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lk(m_);
  for (const auto &l : lines_) {
    if (std::regex_search(l, re)) out.push_back(l);
  }
  return out;
}

std::vector<std::string> SyncLogCapture::all() const {
  std::lock_guard<std::mutex> lk(m_);
  return std::vector<std::string>(lines_.begin(), lines_.end());
}

std::string SyncLogCapture::waitForLine(const std::string &pattern,
                                        std::chrono::milliseconds timeout) {
  std::regex re(pattern);
  std::unique_lock<std::mutex> lk(m_);
  size_t scanned = 0;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    for (; scanned < lines_.size(); ++scanned) {
      if (std::regex_search(lines_[scanned], re)) return lines_[scanned];
    }
    if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) return {};
  }
}

} // namespace videosync

// SYNCLOG dispatch hook — njclient.cpp on Apple calls this for every emitted
// line. The host-test build provides the symbol here instead of LoggerBridge.mm.
extern "C" void synclog_emit_oslog(const char *msg) {
  videosync::SyncLogCapture::instance().push(msg);
}
