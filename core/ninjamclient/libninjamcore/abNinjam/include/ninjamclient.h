#ifndef NINJAMCLIENT_H
#define NINJAMCLIENT_H

#pragma once

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__)
#ifdef ABNINJAM_USE_LIBLO
// Prevent windows.h including winsock.h
#include <winsock2.h>
#endif
#endif

#include "../../njclient.h"
#include "connectionproperties.h"
#include "log.h"
#include "ninjamclientstatus.h"
#include "remotechannel.h"
#include "remoteuser.h"
#include <mutex>
#include <thread>
#include <vector>
#include <map>

#define ADJUST_VOLUME 10

// Bridge license callback — set by NinjamClientBridge to route license prompts to Swift
extern "C" void NinjamClient_setBridgeLicenseCallback(int32_t (*cb)(const char*));
extern "C" void NinjamClient_respondToLicense(int accepted);
extern "C" void NinjamClient_cancelPendingLicense();

namespace AbNinjam {
namespace Common {

class NinjamClient {

public:
  NinjamClient();
  ~NinjamClient();
  NinjamClientStatus connect(ConnectionProperties *connectionProperties);
  void disconnect();
  void audiostreamOnSamples(float **inbuf, int innch, float **outbuf,
                            int outnch, int len, int srate);
  void audiostreamForSync(float **inbuf, int innch, float **outbuf, int outnch,
                          int len, int srate);
  auto &gsNjClient() { return njClient; }
  auto &gsStopConnectionThread() { return stopConnectionThread; }
  auto &gsMtx() { return mtx; }
  bool connected = false;
  void clearBuffers(float **buf, int nch, int len);
  void adjustVolume();
  void setBpm(int bpm);
  std::vector<RemoteUser> getRemoteUsers();
  void setUserChannelVolume(int userId, int channelId, float volume);
  void setUserChannelState(int userId, int channelId,bool setsub, bool sub, bool setvol, float vol, bool setpan, float pan, bool setmute, bool mute, bool setsolo, bool solo, bool setoutch, int outchannel);
  void setLocalChannelVolume(int channelId, float volume);
  void sendChatMessage(std::string message);

  // Raw data channel passthrough
  void setRawDataCallback(NJClient::RawDataCallback cb, void *userData);
  void rawDataSendBegin(unsigned char outGuid[16], unsigned int fourcc, int chidx, int estsize);
  void rawDataSendWrite(const unsigned char guid[16], const void *data, int dataLen, bool isEnd);

private:
  std::thread *connectionThread;
  NJClient *njClient = new NJClient;
  bool stopConnectionThread, autoRemoteVolume;
  std::mutex mtx;
};

} // namespace Common
} // namespace AbNinjam

#endif // NINJAMCLIENT_H
