// NJConfig.h
#ifndef NJ_CONFIG_H
#define NJ_CONFIG_H

// NINJAM client configuration
#define NJ_SOURCE_PORT 0  // Let OS choose port
#define NJCLIENT_NO_XMIT_SUPPORT 0  // Enable transmit
#define NJCLIENT_SUPPORT_AUDIOIN 1  // Enable audio input
#define ENABLE_CLIENT_SYSLOG 0     // Disable syslog
#define INVALID_HWND nullptr

// Optionally disable features you don't need
// #define NJCLIENT_NO_RECORDER
// #define NJCLIENT_NO_LOCAL_BROADCAST

#endif
