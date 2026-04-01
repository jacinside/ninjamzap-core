// LoggerBridge.h — Android version
// Same interface as ios/NativeAudioModule/LoggerBridge.h
// IOSLogger.h includes "../LoggerBridge.h" — on Android we provide this via include path.
#ifndef LoggerBridge_h
#define LoggerBridge_h

#ifdef __cplusplus
extern "C" {
#endif

void LogMessage(const char* message, int level);
void SetLogLevel(int level);

#ifdef __cplusplus
}
#endif

#endif /* LoggerBridge_h */
