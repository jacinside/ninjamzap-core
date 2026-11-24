// Scenario 49 — a login under a name already in the room drops the old connection, and the old
// client says the server closed it ("closed by server"), not that the connection was lost.
//
// The server disconnects any user by the same name when a new one logs in (usercon.cpp). With
// both devices of one musician reconnecting by themselves after a drop, each kept throwing the
// other out; the app reconnects only after a lost connection, so it needs to tell them apart.
//
// Setup: two clients log in with the same anonymous name from the same host (same "name@ip").
// Verify: the first one is disconnected with "closed by server"; the second stays connected.
#include <chrono>
#include <string>
#include <thread>

#include "catch_amalgamated.hpp"

#include "TestClient.h"
#include "TestEnv.h"
#include "njclient.h"

using namespace std::chrono_literals;

namespace {
videosync::TestClient::Options makeOpts() {
  videosync::TestClient::Options o;
  o.host = videosync::testenv::host;
  o.port = videosync::testenv::port;
  o.user = "anonymous:same_name";
  return o;
}
} // namespace

TEST_CASE("49_same_name_closed_by_server — the replaced login says the server closed it",
          "[scenario49]") {
  videosync::TestClient first(makeOpts());
  REQUIRE(first.connectAndJoin(8s));

  videosync::TestClient second(makeOpts());
  REQUIRE(second.connectAndJoin(8s));

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (first.raw()->GetStatus() != NJClient::NJC_STATUS_DISCONNECTED &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(20ms);

  REQUIRE(first.raw()->GetStatus() == NJClient::NJC_STATUS_DISCONNECTED);
  CHECK(std::string(first.raw()->GetErrorStr()) == "closed by server");
  CHECK(second.raw()->GetStatus() == NJClient::NJC_STATUS_OK);
}
