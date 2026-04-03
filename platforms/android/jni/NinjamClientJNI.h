#ifndef NINJAM_CLIENT_JNI_H
#define NINJAM_CLIENT_JNI_H

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

// JNI_OnLoad: cache JavaVM pointer, register native methods
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved);
JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved);

// ============================================================================
// Client Lifecycle
// ============================================================================
JNIEXPORT jlong JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeCreate(JNIEnv* env, jobject thiz);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeDestroy(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetVersion(JNIEnv* env, jobject thiz);

// ============================================================================
// Connection Management
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetUser(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jstring password);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeConnect(JNIEnv* env, jobject thiz, jlong clientPtr, jstring hostname, jint port);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeDisconnect(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeIsConnected(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeProcess(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetServerStatus(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetLatency(JNIEnv* env, jobject thiz, jlong clientPtr);

// ============================================================================
// Audio Processing
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetAudioConfig(JNIEnv* env, jobject thiz, jlong clientPtr, jint sampleRate, jint channels);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeProcessAudio(JNIEnv* env, jobject thiz, jlong clientPtr,
    jfloatArray inLeft, jfloatArray inRight, jfloatArray outLeft, jfloatArray outRight, jint numFrames);

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeProcessAudioDirect(JNIEnv* env, jobject thiz, jlong clientPtr,
    jlong inLeftPtr, jlong inRightPtr, jlong outLeftPtr, jlong outRightPtr, jint numFrames);

// ============================================================================
// Channel Management
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetLocalChannelState(JNIEnv* env, jobject thiz, jlong clientPtr,
    jint index, jfloat volume, jfloat pan, jboolean mute, jboolean solo);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetLocalChannelInfo(JNIEnv* env, jobject thiz, jlong clientPtr,
    jint channelIndex, jstring name, jint setsrcch, jint srcch, jint setxmit, jint xmit, jint setflags, jint flags);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetUserChannelState(JNIEnv* env, jobject thiz, jlong clientPtr,
    jstring username, jint channelIndex, jfloat volume, jfloat pan, jboolean mute, jboolean subscribed);

// ============================================================================
// Volume Controls
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetMasterVolume(JNIEnv* env, jobject thiz, jlong clientPtr,
    jfloat volume, jfloat pan, jboolean mute);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetMetronome(JNIEnv* env, jobject thiz, jlong clientPtr,
    jfloat volume, jboolean mute, jfloat pan, jint channelIndex);

// ============================================================================
// Chat
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSendChatMessage(JNIEnv* env, jobject thiz, jlong clientPtr, jstring message);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSendPrivateMessage(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jstring message);

// ============================================================================
// Session Info
// ============================================================================
JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetBPM(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetBPI(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT jdouble JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetIntervalPosition(JNIEnv* env, jobject thiz, jlong clientPtr);

// ============================================================================
// Remote Users
// ============================================================================
JNIEXPORT jobjectArray JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetRemoteUserNames(JNIEnv* env, jobject thiz, jlong clientPtr);

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetUserChannelCount(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username);

// ============================================================================
// Peak Meters
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetOutputPeaks(JNIEnv* env, jobject thiz, jlong clientPtr, jfloatArray peaks);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetLocalChannelPeaks(JNIEnv* env, jobject thiz, jlong clientPtr, jint channelIndex, jfloatArray peaks);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetUserChannelPeaks(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jint channelIndex, jfloatArray peaks);

// ============================================================================
// Callbacks (set from Kotlin, called from C++ threads)
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetCallbackTarget(JNIEnv* env, jobject thiz, jlong clientPtr, jobject callbackTarget);

// ============================================================================
// Session Recording
// ============================================================================
JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeStartRecording(JNIEnv* env, jobject thiz, jlong enginePtr,
    jstring recordingsDir, jstring roomName, jstring myUsername, jobjectArray participants);

JNIEXPORT jobjectArray JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeStopRecording(JNIEnv* env, jobject thiz, jlong enginePtr,
    jobjectArray currentParticipants);

JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSaveRecording(JNIEnv* env, jobject thiz, jlong enginePtr, jstring finalPath);

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeDiscardRecording(JNIEnv* env, jobject thiz, jlong enginePtr);

JNIEXPORT jdouble JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetRecordingElapsedTime(JNIEnv* env, jobject thiz, jlong enginePtr);

#ifdef __cplusplus
}
#endif

#endif // NINJAM_CLIENT_JNI_H
