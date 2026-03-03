#pragma once
#include <cstdint>

namespace AbNinjam {
namespace Common {

// Tipos de mensajes del protocolo NINJAM
enum NinjamMessageType : uint16_t {
  MSG_CONFIG_CHANGE_NOTIFY = 0xC500,
  MSG_AUTH_CHALLENGE       = 0xC001,
  MSG_AUTH_USER            = 0xC002,
  MSG_AUTH_REPLY           = 0xC003,
  MSG_SERVER_AUTH_USER     = 0xC004,
  MSG_CLIENT_HELLO         = 0xC00A,
  MSG_SERVER_HELLO        = 0xC00B,
  MSG_UPLOAD_INTERVAL      = 0xC009,
  MSG_INTERVAL_BEGIN       = 0xC351,
  MSG_USER_INFO_CHANGE     = 0xC510
};

} // namespace ab
}
