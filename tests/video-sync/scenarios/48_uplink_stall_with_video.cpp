// Scenario 48 — an uplink that slows down for a while drops video, not the connection.
//
// A Wi-Fi uplink can fall well below what audio + video need for 10-20 s while the downlink
// keeps working. The camera and the audio keep producing, so the connection's send queue grows;
// at NET_CON_MAX_MESSAGES (512) the next Send fails the connection. Seen on a Mac sending 720p
// video over a struggling Wi-Fi: the queue reached 470-500 messages and the session dropped,
// 7 times in 15 minutes. Audio alone still fits such an uplink; video is the part that can go.
// (An uplink that stops completely for ~9 s is dropped by the server — keepalive 3 s × 3 —
// whatever the client does; that's not this case.)
//
// Setup: a sender with audio and fast video connects through a local proxy that forwards the
// sender's bytes at kSlowBytesPerSec only (its downlink keeps flowing) until the video backs off
// (kHoldAfterBackoffMs later) or the sender drops, at most kMaxSlowMs — the kernel's socket
// buffers absorb a different amount first on each OS (a few hundred KB on macOS, several MB on
// Linux). Then full speed again. A receiver is connected directly.
// Verify: the sender stays connected, its video was dropped while the uplink was slow (the
// queue never reached the limit), and video reaches the receiver again afterwards.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "catch_amalgamated.hpp"

#include "FakeFrame.h"
#include "SyncLogCapture.h"
#include "TestClient.h"
#include "TestEnv.h"
#include "njclient.h"

using namespace std::chrono_literals;

namespace {

// One-connection TCP proxy on 127.0.0.1 whose client→server direction can be slowed to a byte
// rate: it reads the client's socket no faster than that, so the client's TCP buffers fill as on
// a slow uplink. Server→client keeps flowing at full speed.
class SlowUplinkProxy {
public:
  SlowUplinkProxy(const std::string &host, int port) : host_(host), port_(port) {
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // Small receive window for the accepted socket (set before accept, so it applies), so a
    // slow uplink backs up into the client quickly instead of into loopback buffers.
    int rcv = 16 * 1024;
    setsockopt(listen_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listen_, (sockaddr *)&a, sizeof(a));
    listen(listen_, 1);
    socklen_t len = sizeof(a);
    getsockname(listen_, (sockaddr *)&a, &len);
    localPort_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~SlowUplinkProxy() {
    stop_ = true;
    shutdown(listen_, SHUT_RDWR);
    close(listen_);
    if (thread_.joinable()) thread_.join();
  }
  int port() const { return localPort_; }
  // 0 = full speed.
  void setUplinkRate(int bytesPerSec) { rate_ = bytesPerSec; }

private:
  void run() {
    int cl = accept(listen_, nullptr, nullptr);
    if (cl < 0) return;
    int sv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port_);
    inet_pton(AF_INET, host_.c_str(), &a.sin_addr);
    if (connect(sv, (sockaddr *)&a, sizeof(a)) != 0) {
      close(cl);
      close(sv);
      return;
    }
    char buf[65536];
    auto lastTick = std::chrono::steady_clock::now();
    double budget = 0; // bytes the slow uplink may forward now
    while (!stop_) {
      const int rate = rate_.load();
      const auto now = std::chrono::steady_clock::now();
      budget += rate * std::chrono::duration<double>(now - lastTick).count();
      lastTick = now;
      if (budget > rate / 10.0) budget = rate / 10.0; // no bursts after an idle moment
      const bool mayRead = rate == 0 || budget >= 1;
      fd_set rd;
      FD_ZERO(&rd);
      if (mayRead) FD_SET(cl, &rd);
      FD_SET(sv, &rd);
      timeval tv{0, 10 * 1000};
      if (select(std::max(cl, sv) + 1, &rd, nullptr, nullptr, &tv) < 0) break;
      if (mayRead && FD_ISSET(cl, &rd)) {
        size_t want = rate == 0 ? sizeof(buf) : std::min(sizeof(buf), (size_t)budget);
        ssize_t n = recv(cl, buf, want, 0);
        if (n <= 0 || send(sv, buf, (size_t)n, 0) != n) break;
        if (rate) budget -= (double)n;
      }
      if (FD_ISSET(sv, &rd)) {
        ssize_t n = recv(sv, buf, sizeof(buf), 0);
        if (n <= 0 || send(cl, buf, (size_t)n, 0) != n) break;
      }
    }
    close(cl);
    close(sv);
  }

  std::string host_;
  int port_;
  int listen_ = -1;
  int localPort_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> rate_{0};
  std::thread thread_;
};

} // namespace

TEST_CASE("48_uplink_stall_with_video — a slow uplink drops video, not the connection",
          "[sync][scenario48]") {
  auto &log = videosync::SyncLogCapture::instance();
  log.clear();

  const int kMaxSlowMs = 60000;
  const int kHoldAfterBackoffMs = 3000; // still connected a while after backing off
  const int kSlowBytesPerSec = 16 * 1024; // audio fits, audio + video doesn't
  const int kFps = 60;
  const int kFrameBytes = 6000; // one upload message each (< MAX_ENC_BLOCKSIZE)

  SlowUplinkProxy proxy(videosync::testenv::host, videosync::testenv::port);

  videosync::TestClient::Options so;
  so.host = "127.0.0.1";
  so.port = proxy.port();
  so.user = "anonymous:stall_sender";
  so.sendAudio = true;
  so.sendVideo = true;
  videosync::TestClient sender(so);

  videosync::TestClient::Options ro;
  ro.host = videosync::testenv::host;
  ro.port = videosync::testenv::port;
  ro.user = "anonymous:stall_recv";
  ro.sendAudio = true;
  ro.recordOutput = true;
  videosync::TestClient receiver(ro);

  REQUIRE(sender.connectAndJoin(8s));
  REQUIRE(receiver.connectAndJoin(8s));
  if (sender.getBPI() != 4 || sender.getBPM() != 240) {
    sender.sendChatMessage("MSG", "!vote bpi 4");
    sender.sendChatMessage("MSG", "!vote bpm 240");
    receiver.sendChatMessage("MSG", "!vote bpi 4");
    receiver.sendChatMessage("MSG", "!vote bpm 240");
    auto deadline = std::chrono::steady_clock::now() + 8s;
    while ((sender.getBPI() != 4 || sender.getBPM() != 240) &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(50ms);
  }
  sender.sendFakeSPSPPS();

  std::atomic<bool> stop{false};
  std::thread camera([&] {
    uint32_t seq = 0;
    while (!stop.load()) {
      auto f = videosync::makeFakeFrame(seq++, 0, kFrameBytes);
      sender.sendVideoFrame(f.data(), (int)f.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kFps));
    }
  });

  std::this_thread::sleep_for(3s); // video flowing
  const size_t framesBeforeStall = receiver.drainVideoFrames().size();
  REQUIRE(framesBeforeStall > 0);

  int bytes = 0, worstQueue = 0;
  sender.raw()->TakeSendBacklogMax(&bytes, &worstQueue); // reset
  proxy.setUplinkRate(kSlowBytesPerSec);
  bool connectedThroughout = true;
  int backedOffAt = -1;
  for (int t = 0; t < kMaxSlowMs; t += 100) {
    std::this_thread::sleep_for(100ms);
    if (sender.raw()->GetStatus() != NJClient::NJC_STATUS_OK) { connectedThroughout = false; break; }
    if (backedOffAt < 0 && !log.match("video uplink busy").empty()) backedOffAt = t;
    if (backedOffAt >= 0 && t - backedOffAt >= kHoldAfterBackoffMs) break;
  }
  sender.raw()->TakeSendBacklogMax(&bytes, &worstQueue);
  proxy.setUplinkRate(0);

  receiver.drainVideoFrames(); // what arrived while slow doesn't count
  std::this_thread::sleep_for(6s); // the backlog drains, video comes back
  const size_t framesAfterStall = receiver.drainVideoFrames().size();
  stop.store(true);
  camera.join();

  INFO("sender status=" << sender.raw()->GetStatus() << " backed off after " << backedOffAt
                        << " ms, worst queue=" << worstQueue
                        << " msgs, " << bytes / 1024 << " KB; receiver frames: " << framesBeforeStall
                        << " in 3 s before, " << framesAfterStall << " in 6 s after");
  CHECK(connectedThroughout);
  CHECK(sender.raw()->GetStatus() == NJClient::NJC_STATUS_OK);
  CHECK(worstQueue < 512);
  CHECK(log.match("video uplink busy").size() > 0);
  CHECK(framesAfterStall > 0);
}
