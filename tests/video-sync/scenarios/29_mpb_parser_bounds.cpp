// Scenario 29 — mpb parsers stay inside the message and read pan as signed.
//
// Pan travels as one byte holding -128..127 (builders clamp to that range and
// write (unsigned char)pan), but the parsers read it back unsigned, so a pan of
// -64 came out as 192. The server stores that value and rebuilds the message
// with build_add_rec, which clamps 192 to 127: a channel panned left reached the
// other clients as hard right.
//
// Names and chat parameters are NUL-terminated strings inside the message. A peer
// can send one without the terminator: the name loops checked *p before the
// remaining length (one byte read past the end), set_channel_info measured the
// remaining length without its 2-byte header (two bytes past the end, then field
// reads beyond it, so a record whose fields were cut short was accepted with
// pan/flags taken from past the end), and chat parse returned success with a parameter that is not
// a C string. Those messages must be refused.
//
// Same fix as upstream NINJAM pull request #25 (justinfrankel/ninjam).
// No server needed.
#include <cstring>
#include <string>

#include "catch_amalgamated.hpp"

#include "mpb.h"
#include "netmsg.h"

namespace {
// The bytes after the message are non-zero (the buffer keeps its allocation when
// it shrinks), so a parser that reads past the end finds no terminator there.
Net_Message *rawMessage(int type, const void *data, int size) {
  Net_Message *m = new Net_Message;
  m->set_type(type);
  m->set_size(size + 64);
  memset(m->get_data(), 'X', size + 64);
  memcpy(m->get_data(), data, size);
  m->set_size(size);
  return m;
}
} // namespace

TEST_CASE("29_mpb_parser_bounds — userinfo_change_notify pan is signed", "[scenario29]") {
  mpb_server_userinfo_change_notify out;
  out.build_add_rec(1, 0, 0, -64, 0, "alice", "guitar");
  Net_Message *m = out.build();
  REQUIRE(m);

  mpb_server_userinfo_change_notify in;
  REQUIRE(in.parse(m) == 0);
  int active = 0, channel = 0, pan = 0, flags = 0;
  short volume = 0;
  const char *user = nullptr, *chname = nullptr;
  REQUIRE(in.parse_get_rec(0, &active, &channel, &volume, &pan, &flags, &user, &chname) > 0);
  CHECK(pan == -64);
  CHECK(std::string(user) == "alice");
  CHECK(std::string(chname) == "guitar");
  delete m;
}

TEST_CASE("29_mpb_parser_bounds — set_channel_info pan is signed, records chain", "[scenario29]") {
  mpb_client_set_channel_info out;
  out.build_add_rec("left", 0, -64, 0);
  out.build_add_rec("right", 0, 64, 0);
  Net_Message *m = out.build();
  REQUIRE(m);

  mpb_client_set_channel_info in;
  REQUIRE(in.parse(m) == 0);
  const char *name = nullptr;
  short volume = 0;
  int pan = 0, flags = 0;
  int offs = in.parse_get_rec(0, &name, &volume, &pan, &flags);
  REQUIRE(offs > 0);
  CHECK(std::string(name) == "left");
  CHECK(pan == -64);
  offs = in.parse_get_rec(offs, &name, &volume, &pan, &flags);
  REQUIRE(offs > 0);
  CHECK(std::string(name) == "right");
  CHECK(pan == 64);
  CHECK(in.parse_get_rec(offs, &name, &volume, &pan, &flags) <= 0);
  delete m;
}

TEST_CASE("29_mpb_parser_bounds — set_channel_info refuses an unterminated name", "[scenario29]") {
  // 2-byte mpisize header (4), then a name with no NUL and no fields
  const unsigned char data[] = {4, 0, 'a', 'b', 'c'};
  Net_Message *m = rawMessage(MESSAGE_CLIENT_SET_CHANNEL_INFO, data, sizeof(data));
  mpb_client_set_channel_info in;
  REQUIRE(in.parse(m) == 0);
  const char *name = nullptr;
  short volume = 0;
  int pan = 0, flags = 0;
  CHECK(in.parse_get_rec(0, &name, &volume, &pan, &flags) <= 0);
  delete m;
}

TEST_CASE("29_mpb_parser_bounds — set_channel_info refuses truncated fields", "[scenario29]") {
  // mpisize 4 (volume, pan, flags) but only the 2 volume bytes follow the name:
  // pan and flags would come from past the end of the message
  const unsigned char data[] = {4, 0, 'a', 0, 0, 0};
  Net_Message *m = rawMessage(MESSAGE_CLIENT_SET_CHANNEL_INFO, data, sizeof(data));
  mpb_client_set_channel_info in;
  REQUIRE(in.parse(m) == 0);
  const char *name = nullptr;
  short volume = 0;
  int pan = 0, flags = 0;
  CHECK(in.parse_get_rec(0, &name, &volume, &pan, &flags) <= 0);
  delete m;
}

TEST_CASE("29_mpb_parser_bounds — set_channel_info refuses offsets past the end", "[scenario29]") {
  mpb_client_set_channel_info out;
  out.build_add_rec("ch", 0, 0, 0);
  Net_Message *m = out.build();
  REQUIRE(m);
  mpb_client_set_channel_info in;
  REQUIRE(in.parse(m) == 0);
  const char *name = nullptr;
  short volume = 0;
  int pan = 0, flags = 0;
  CHECK(in.parse_get_rec(m->get_size(), &name, &volume, &pan, &flags) <= 0);
  CHECK(in.parse_get_rec(-1, &name, &volume, &pan, &flags) <= 0);
  delete m;
}

TEST_CASE("29_mpb_parser_bounds — set_usermask refuses an unterminated name", "[scenario29]") {
  const unsigned char data[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  Net_Message *m = rawMessage(MESSAGE_CLIENT_SET_USERMASK, data, sizeof(data));
  mpb_client_set_usermask in;
  REQUIRE(in.parse(m) == 0);
  const char *user = nullptr;
  unsigned int chflags = 0;
  CHECK(in.parse_get_rec(0, &user, &chflags) <= 0);
  delete m;
}

TEST_CASE("29_mpb_parser_bounds — chat refuses an unterminated parameter", "[scenario29]") {
  mpb_chat_message in;

  const char ok[] = "MSG\0hello";  // sizeof includes the final NUL
  Net_Message *m = rawMessage(MESSAGE_CHAT_MESSAGE, ok, sizeof(ok));
  REQUIRE(in.parse(m) == 0);
  CHECK(std::string(in.parms[0]) == "MSG");
  CHECK(std::string(in.parms[1]) == "hello");
  delete m;

  m = rawMessage(MESSAGE_CHAT_MESSAGE, ok, sizeof(ok) - 1);  // "hello" without its NUL
  CHECK(in.parse(m) != 0);
  delete m;
}
