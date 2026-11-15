// Scenario 47 — a login challenge that arrives late (slow network) still logs in.
//
// The server takes the client's first message as its answer to the login challenge. A
// connection sends a keepalive after NET_CON_KEEPALIVE_RATE (3 s) without sending anything,
// and the client has nothing to send until the challenge arrives — so on a slow network
// (the challenge seen after more than ~3 s, e.g. a reconnect over a struggling Wi-Fi) the
// keepalive went out first and the server refused the login: "invalid authorization reply".
// Seen on a Mac reconnecting after a Wi-Fi stall: the challenge took 8 s, the reconnect was
// refused, and the app gave up.
//
// Setup: the client connects through a local proxy that holds the server's bytes for 5 s
// (the client's own bytes pass straight through), as a slow downlink does.
// Verify: the client logs in (status OK, no error string), and the server saw no keepalive
// before the login answer — i.e. the login was not refused.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "catch_amalgamated.hpp"

#include "TestClient.h"
#include "TestEnv.h"
#include "njclient.h"

using namespace std::chrono_literals;

namespace {

// One-connection TCP proxy on 127.0.0.1: client→server bytes pass at once; server→client bytes
// are held until `holdDownlink` has passed since the client connected.
class SlowDownlinkProxy {
public:
  SlowDownlinkProxy(const std::string &host, int port, std::chrono::milliseconds holdDownlink)
      : host_(host), port_(port), hold_(holdDownlink) {
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(listen_, (sockaddr *)&a, sizeof(a));
    listen(listen_, 1);
    socklen_t len = sizeof(a);
    getsockname(listen_, (sockaddr *)&a, &len);
    localPort_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~SlowDownlinkProxy() {
    stop_ = true;
    shutdown(listen_, SHUT_RDWR);
    close(listen_);
    if (thread_.joinable()) thread_.join();
  }
  int port() const { return localPort_; }

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
    const auto releaseAt = std::chrono::steady_clock::now() + hold_;
    char buf[65536];
    while (!stop_) {
      const bool held = std::chrono::steady_clock::now() < releaseAt;
      fd_set rd;
      FD_ZERO(&rd);
      FD_SET(cl, &rd);
      if (!held) FD_SET(sv, &rd); // the server's bytes wait in its socket while held
      timeval tv{0, 50 * 1000};
      if (select(std::max(cl, sv) + 1, &rd, nullptr, nullptr, &tv) < 0) break;
      if (FD_ISSET(cl, &rd)) {
        ssize_t n = recv(cl, buf, sizeof(buf), 0);
        if (n <= 0 || send(sv, buf, (size_t)n, 0) != n) break;
      }
      if (!held && FD_ISSET(sv, &rd)) {
        ssize_t n = recv(sv, buf, sizeof(buf), 0);
        if (n <= 0 || send(cl, buf, (size_t)n, 0) != n) break;
      }
    }
    close(cl);
    close(sv);
  }

  std::string host_;
  int port_;
  std::chrono::milliseconds hold_;
  int listen_ = -1;
  int localPort_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

} // namespace

TEST_CASE("47_slow_login_challenge — a challenge that arrives after the keepalive time logs in",
          "[sync][scenario47]") {
  // 5 s: past the 3 s keepalive, under the 9 s receive timeout (3 × keepalive).
  SlowDownlinkProxy proxy(videosync::testenv::host, videosync::testenv::port, 5000ms);

  videosync::TestClient::Options o;
  o.host = "127.0.0.1";
  o.port = proxy.port();
  o.user = "anonymous:slow_login";
  o.sendAudio = true;
  videosync::TestClient client(o);

  const bool ok = client.connectAndJoin(12s);
  const char *err = client.raw()->GetErrorStr();
  INFO("status=" << client.raw()->GetStatus() << " error=\"" << (err ? err : "") << "\"");
  REQUIRE(ok);
  CHECK(client.raw()->GetStatus() == NJClient::NJC_STATUS_OK);
  CHECK((err == nullptr || err[0] == 0));
  client.disconnect();
}
