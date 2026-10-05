// Scenario 27 — two client processes started in the same second get different
// interval GUIDs.
//
// Interval GUIDs come from the WDL RNG, whose state is per process and was seeded
// only with time(NULL) in the NJClient constructor. Two processes created in the
// same second therefore produced the same GUID sequence, and receivers (which key
// downloads by GUID) mixed up their streams.
//
// fork() gives the child an identical copy of the RNG state; parent and child then
// each construct an NJClient right away (same second) and draw 16 bytes. With
// time-only seeding those bytes match; with the per-process entropy they differ.
// No server needed.
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include "catch_amalgamated.hpp"

#include "njclient.h"
#include "WDL/rng.h"

namespace {
void drawAfterNewClient(unsigned char out[16]) {
  NJClient *c = new NJClient;
  WDL_RNG_bytes(out, 16);
  delete c;
}
} // namespace

TEST_CASE("27_guid_entropy_across_processes — same-second clients draw different GUIDs",
          "[scenario27]") {
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    unsigned char g[16];
    drawAfterNewClient(g);
    ssize_t n = write(fds[1], g, sizeof(g));
    _exit(n == (ssize_t)sizeof(g) ? 0 : 1);
  }
  unsigned char mine[16];
  drawAfterNewClient(mine);
  unsigned char theirs[16] = {};
  ssize_t got = read(fds[0], theirs, sizeof(theirs));
  int status = 0;
  waitpid(pid, &status, 0);
  close(fds[0]);
  close(fds[1]);
  REQUIRE(got == (ssize_t)sizeof(theirs));
  REQUIRE(WIFEXITED(status));
  CHECK(memcmp(mine, theirs, sizeof(mine)) != 0);
}
