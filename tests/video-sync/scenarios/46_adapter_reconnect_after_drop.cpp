// Scenario 46 — after a connection drops by itself, a new connect() reports its own result.
//
// An app reconnecting after a lost connection (another Wi-Fi) calls connect() again on the same
// adapter. The previous connection never went through disconnect(): its thread had left its loop
// unjoined and NinjamClient still said "connected", so connect() returned "ok" with no server —
// the app went on as if logged in and gave up when the real failure surfaced (-1 / -2).
//
// Setup: the adapter connects through a local TCP relay to the test server; the relay drops the
// connection (as a network change does) and stops listening.
// Verify: the drop is seen (process(), as the app's update timer calls it); a connect() with
// nothing listening fails; once the relay listens again a connect() logs in. The send backlog
// the stuck socket built up before the drop (the relay stops reading, as a dead Wi-Fi does) is
// not reported for the new connection (TakeSendBacklogMax): the app lowered its video for it
// right after every reconnect.
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "catch_amalgamated.hpp"

#include "TestEnv.h"
#include "abNinjam/ninjamclientAdapter.h"

using namespace std::chrono_literals;

extern "C" void NinjamClient_setBridgeLicenseCallback(int32_t (*cb)(const char *));
extern "C" void NinjamClient_respondToLicense(int accepted);

// The app's bridge defines the adapter's callback setters (platforms/ios NinjamClientBridge.cpp);
// the same one-liner here. process() reports the drop through it.
void NinjamClientAdapter::setOnDisconnect(OnDisconnectCallback callback) { onDisconnectCallback = callback; }

namespace {
// One connection at a time from 127.0.0.1:<port> to the test server; drop() cuts it and stops
// listening, listen() starts again on the same port. While paused it reads nothing from the
// client, whose socket then fills up.
class Relay {
public:
  int port = 0;
  std::atomic<bool> paused{false};
  ~Relay() { drop(); }

  bool listen() {
    lfd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(lfd_, (sockaddr *)&a, sizeof(a)) || ::listen(lfd_, 1)) return false;
    socklen_t len = sizeof(a);
    getsockname(lfd_, (sockaddr *)&a, &len);
    port = ntohs(a.sin_port);
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void drop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (lfd_ >= 0) close(lfd_);
    lfd_ = -1;
  }

private:
  int lfd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;

  void run() {
    while (!stop_) {
      pollfd p{lfd_, POLLIN, 0};
      if (poll(&p, 1, 50) <= 0) continue;
      int c = accept(lfd_, nullptr, nullptr);
      if (c < 0) continue;
      int s = connectUpstream();
      if (s < 0) { close(c); continue; }
      pump(c, s);
      close(c);
      close(s);
    }
  }

  static int connectUpstream() {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    const std::string portStr = std::to_string(videosync::testenv::port);
    if (getaddrinfo(videosync::testenv::host.c_str(), portStr.c_str(), &hints, &res)) return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(s, res->ai_addr, res->ai_addrlen)) { close(s); s = -1; }
    freeaddrinfo(res);
    return s;
  }

  void pump(int a, int b) {
    std::vector<char> buf(65536);
    while (!stop_) {
      if (paused) { std::this_thread::sleep_for(20ms); continue; }
      pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
      if (poll(p, 2, 50) <= 0) continue;
      for (int i = 0; i < 2; i++) {
        if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        const ssize_t n = read(p[i].fd, buf.data(), buf.size());
        if (n <= 0) return;
        if (write(i == 0 ? b : a, buf.data(), (size_t)n) != n) return;
      }
    }
  }
};
} // namespace

TEST_CASE("46_adapter_reconnect_after_drop — a reconnect after a dropped connection reports its own result",
          "[scenario46]") {
  NinjamClient_setBridgeLicenseCallback([](const char *) -> int32_t { NinjamClient_respondToLicense(1); return 0; });
  Relay relay;
  REQUIRE(relay.listen());

  NinjamClientAdapter adapter;
  std::atomic<int> lastDisconnect{1};
  adapter.setOnDisconnect([&](int reason) { lastDisconnect = reason; });
  adapter.setCredentials("relayed", "");
  adapter.setAudioConfig(48000, 2);
  adapter.connect("127.0.0.1", relay.port);
  REQUIRE(adapter.isConnected());

  // The network stalls first: a burst of video backs up the send queue (measured).
  NJClient *nj = adapter.njClientForTest();
  relay.paused = true;
  auto burst = [&] {
    unsigned char guid[16];
    nj->RawDataSendBegin(guid, 0x4E5A5644, 1, 0);
    std::vector<char> frame(8000, 1);
    for (int i = 0; i < 400; i++) nj->RawDataSendWrite(guid, frame.data(), (int)frame.size(), i == 399);
    std::this_thread::sleep_for(500ms);
  };
  burst();
  int bytes = 0, msgs = 0;
  nj->TakeSendBacklogMax(&bytes, &msgs);
  std::fprintf(stderr, "[scenario46] backlog on the stuck socket: %d bytes, %d messages\n", bytes, msgs);
  REQUIRE(msgs > 0); // or the check after the reconnect tests nothing
  burst();

  // The network goes away: the app's update timer (process) sees the connection drop.
  relay.drop();
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline && adapter.isConnected()) {
    adapter.process();
    std::this_thread::sleep_for(50ms);
  }
  REQUIRE(!adapter.isConnected());
  CHECK(lastDisconnect == -3);

  // Nothing listening: this attempt fails.
  adapter.connect("127.0.0.1", relay.port);
  std::fprintf(stderr, "[scenario46] connect with no server: connected=%d\n", adapter.isConnected() ? 1 : 0);
  CHECK(!adapter.isConnected());

  // The network is back: a reconnect logs in, without the old socket's backlog.
  relay.paused = false;
  REQUIRE(relay.listen());
  adapter.connect("127.0.0.1", relay.port);
  CHECK(adapter.isConnected());
  nj->TakeSendBacklogMax(&bytes, &msgs);
  std::fprintf(stderr, "[scenario46] backlog reported after reconnecting: %d bytes, %d messages\n", bytes, msgs);
  CHECK(msgs < 8);
  adapter.disconnect();
}
