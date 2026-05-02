// Catch2 entry point. We keep `main` here so the harness can read environment
// variables (NJ_TEST_HOST, NJ_TEST_PORT) before letting Catch2 take over.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "catch_amalgamated.hpp"

#include "TestEnv.h"

int main(int argc, char **argv) {
  const char *host = std::getenv("NJ_TEST_HOST");
  const char *port = std::getenv("NJ_TEST_PORT");
  if (!host || !port) {
    std::fprintf(stderr,
                 "ERROR: NJ_TEST_HOST and NJ_TEST_PORT must be set "
                 "(use `make test` to spin up the server in Docker).\n");
    return 2;
  }
  videosync::testenv::host = host;
  videosync::testenv::port = std::atoi(port);
  if (videosync::testenv::port <= 0) {
    std::fprintf(stderr, "ERROR: invalid NJ_TEST_PORT=%s\n", port);
    return 2;
  }
  std::fprintf(stderr, "[harness] target server %s:%d\n",
               videosync::testenv::host.c_str(), videosync::testenv::port);

  return Catch::Session().run(argc, argv);
}
