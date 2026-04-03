#include "NinjamClientJNI.h"
#include "NinjamClientBridge.h"
#include "OboeEngine.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "NinjamJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================================
// Global state for JNI callbacks
// ============================================================================
static JavaVM* g_jvm = nullptr;

// Helper: get JNIEnv for current thread (attaches if needed)
struct JNIEnvGuard {
    JNIEnv* env = nullptr;
    bool attached = false;

    JNIEnvGuard() {
        if (!g_jvm) return;
        jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (result == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
    }

    ~JNIEnvGuard() {
        if (attached && g_jvm) {
            g_jvm->DetachCurrentThread();
        }
    }
};

// Per-client callback context (stored alongside the NinjamClientRef)
struct JNICallbackContext {
    jweak callbackRef = nullptr;  // Weak global ref to Kotlin callback object

    // Cached method IDs (looked up once, reused)
    jmethodID onConnectedMethod = nullptr;
    jmethodID onDisconnectedMethod = nullptr;
    jmethodID onChatMessageMethod = nullptr;
    jmethodID onLicenseMethod = nullptr;
    jmethodID onIntervalMethod = nullptr;
};

// Map client pointer -> callback context
// In production, use a proper concurrent map. For now, simple static for single-client use.
static JNICallbackContext g_callbackCtx;

extern "C" {

// ============================================================================
// JNI Lifecycle
// ============================================================================
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("NinjamZap JNI loaded");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved) {
    g_jvm = nullptr;
    LOGI("NinjamZap JNI unloaded");
}

// ============================================================================
// C++ Callback Trampolines (called from C++ threads)
// ============================================================================
static void jni_onConnected() {
    JNIEnvGuard guard;
    if (!guard.env || !g_callbackCtx.callbackRef) return;

    jobject callback = guard.env->NewLocalRef(g_callbackCtx.callbackRef);
    if (!callback) return;

    if (g_callbackCtx.onConnectedMethod) {
        guard.env->CallVoidMethod(callback, g_callbackCtx.onConnectedMethod);
    }
    guard.env->DeleteLocalRef(callback);
}

static void jni_onDisconnected(int32_t reason) {
    JNIEnvGuard guard;
    if (!guard.env || !g_callbackCtx.callbackRef) return;

    jobject callback = guard.env->NewLocalRef(g_callbackCtx.callbackRef);
    if (!callback) return;

    if (g_callbackCtx.onDisconnectedMethod) {
        guard.env->CallVoidMethod(callback, g_callbackCtx.onDisconnectedMethod, (jint)reason);
    }
    guard.env->DeleteLocalRef(callback);
}

static void jni_onChatMessage(const char* username, const char* message) {
    JNIEnvGuard guard;
    if (!guard.env || !g_callbackCtx.callbackRef) return;

    jobject callback = guard.env->NewLocalRef(g_callbackCtx.callbackRef);
    if (!callback) return;

    if (g_callbackCtx.onChatMessageMethod) {
        jstring jUser = guard.env->NewStringUTF(username ? username : "");
        jstring jMsg = guard.env->NewStringUTF(message ? message : "");
        guard.env->CallVoidMethod(callback, g_callbackCtx.onChatMessageMethod, jUser, jMsg);
        guard.env->DeleteLocalRef(jUser);
        guard.env->DeleteLocalRef(jMsg);
    }
    guard.env->DeleteLocalRef(callback);
}

static int32_t jni_onLicense(const char* text) {
    JNIEnvGuard guard;
    if (!guard.env || !g_callbackCtx.callbackRef) return 0;

    jobject callback = guard.env->NewLocalRef(g_callbackCtx.callbackRef);
    if (!callback) return 0;

    jint result = 0;
    if (g_callbackCtx.onLicenseMethod) {
        jstring jText = guard.env->NewStringUTF(text ? text : "");
        result = guard.env->CallIntMethod(callback, g_callbackCtx.onLicenseMethod, jText);
        guard.env->DeleteLocalRef(jText);
    }
    guard.env->DeleteLocalRef(callback);
    return result;
}

static void jni_onInterval(int32_t bpm, int32_t bpi) {
    JNIEnvGuard guard;
    if (!guard.env || !g_callbackCtx.callbackRef) return;

    jobject callback = guard.env->NewLocalRef(g_callbackCtx.callbackRef);
    if (!callback) return;

    if (g_callbackCtx.onIntervalMethod) {
        guard.env->CallVoidMethod(callback, g_callbackCtx.onIntervalMethod, (jint)bpm, (jint)bpi);
    }
    guard.env->DeleteLocalRef(callback);
}

// ============================================================================
// Client Lifecycle
// ============================================================================
JNIEXPORT jlong JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeCreate(JNIEnv* env, jobject thiz) {
    NinjamClientRef* client = NinjamClient_create();
    LOGI("NinjamClient created: %p", client);
    return reinterpret_cast<jlong>(client);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeDestroy(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    if (client) {
        NinjamClient_destroy(client);
        LOGI("NinjamClient destroyed");
    }

    // Clean up callback ref
    if (g_callbackCtx.callbackRef) {
        env->DeleteWeakGlobalRef(g_callbackCtx.callbackRef);
        g_callbackCtx.callbackRef = nullptr;
    }
}

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetVersion(JNIEnv* env, jobject thiz) {
    const char* version = NinjamClient_getVersion();
    return env->NewStringUTF(version ? version : "unknown");
}

// ============================================================================
// Connection Management
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetUser(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jstring password) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);
    const char* pass = env->GetStringUTFChars(password, nullptr);

    NinjamClient_setUser(client, user, pass);

    env->ReleaseStringUTFChars(username, user);
    env->ReleaseStringUTFChars(password, pass);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeConnect(JNIEnv* env, jobject thiz, jlong clientPtr, jstring hostname, jint port) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* host = env->GetStringUTFChars(hostname, nullptr);

    NinjamClient_connect(client, host, port);

    env->ReleaseStringUTFChars(hostname, host);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeDisconnect(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_disconnect(client);
}

JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeIsConnected(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    return NinjamClient_isConnected(client) != 0;
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeProcess(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_process(client);
}

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetServerStatus(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* status = NinjamClient_getServerStatus(client);
    return env->NewStringUTF(status ? status : "");
}

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetLatency(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    return NinjamClient_getLatency(client);
}

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetServerUptime(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    return NinjamClient_getServerUptime(client);
}

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetErrorString(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* err = NinjamClient_getErrorString(client);
    return env->NewStringUTF(err ? err : "");
}

JNIEXPORT jfloat JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetOutputPeak(JNIEnv* env, jobject thiz, jlong clientPtr, jint channel) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    return NinjamClient_getOutputPeak(client, channel);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativePlayMetronomeTick(JNIEnv* env, jobject thiz, jlong clientPtr, jint isDownbeat) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_playMetronomeTick(client, isDownbeat);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeRemoveLocalChannel(JNIEnv* env, jobject thiz, jlong clientPtr, jint channelIndex) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_removeLocalChannel(client, channelIndex);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetLocalChannelVolume(JNIEnv* env, jobject thiz, jlong clientPtr, jint channelIndex, jfloat volume) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_setLocalChannelVolume(client, channelIndex, volume);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSyncWithServerClock(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_syncWithServerClock(client);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSubmitAudioData(JNIEnv* env, jobject thiz, jlong clientPtr, jint channelIndex, jfloatArray data, jint numFrames) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    jfloat* buf = env->GetFloatArrayElements(data, nullptr);
    NinjamClient_submitAudioData(client, channelIndex, buf, numFrames);
    env->ReleaseFloatArrayElements(data, buf, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSubmitAudioDataForSync(JNIEnv* env, jobject thiz, jlong clientPtr, jint channelIndex, jfloatArray data, jint numFrames) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    jfloat* buf = env->GetFloatArrayElements(data, nullptr);
    NinjamClient_submitAudioDataForSync(client, channelIndex, buf, numFrames);
    env->ReleaseFloatArrayElements(data, buf, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSubscribeToAllChannels(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_subscribeToAllChannel(client);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetRemoteChannelVolume(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jint channelIndex, jfloat volume) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);
    NinjamClient_setRemoteChannelVolume(client, user, channelIndex, volume);
    env->ReleaseStringUTFChars(username, user);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSendAdminMessage(JNIEnv* env, jobject thiz, jlong clientPtr, jstring message) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* msg = env->GetStringUTFChars(message, nullptr);
    NinjamClient_sendAdminMessage(client, msg);
    env->ReleaseStringUTFChars(message, msg);
}

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetUserName(JNIEnv* env, jobject thiz, jlong clientPtr, jint index) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* name = NinjamClient_getUserName(client, index);
    return env->NewStringUTF(name ? name : "");
}

// ============================================================================
// Audio Processing
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetAudioConfig(JNIEnv* env, jobject thiz, jlong clientPtr, jint sampleRate, jint channels) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_setAudioConfig(client, sampleRate, channels);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeProcessAudio(JNIEnv* env, jobject thiz, jlong clientPtr,
    jfloatArray inLeft, jfloatArray inRight, jfloatArray outLeft, jfloatArray outRight, jint numFrames) {

    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);

    jfloat* inL = env->GetFloatArrayElements(inLeft, nullptr);
    jfloat* inR = env->GetFloatArrayElements(inRight, nullptr);
    jfloat* outL = env->GetFloatArrayElements(outLeft, nullptr);
    jfloat* outR = env->GetFloatArrayElements(outRight, nullptr);

    NinjamClient_processAudio(client, inL, inR, outL, outR, numFrames);

    env->ReleaseFloatArrayElements(inLeft, inL, 0);
    env->ReleaseFloatArrayElements(inRight, inR, 0);
    env->ReleaseFloatArrayElements(outLeft, outL, 0);
    env->ReleaseFloatArrayElements(outRight, outR, 0);
}

// Direct buffer version for Oboe callback (no JNI array copy overhead)
JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeProcessAudioDirect(JNIEnv* env, jobject thiz, jlong clientPtr,
    jlong inLeftPtr, jlong inRightPtr, jlong outLeftPtr, jlong outRightPtr, jint numFrames) {

    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    auto* inL = reinterpret_cast<float*>(inLeftPtr);
    auto* inR = reinterpret_cast<float*>(inRightPtr);
    auto* outL = reinterpret_cast<float*>(outLeftPtr);
    auto* outR = reinterpret_cast<float*>(outRightPtr);

    return NinjamClient_processAudioSIMD(client, inL, inR, outL, outR, numFrames);
}

// ============================================================================
// Channel Management
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetLocalChannelState(JNIEnv* env, jobject thiz, jlong clientPtr,
    jint index, jfloat volume, jfloat pan, jboolean mute, jboolean solo) {

    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_setLocalChannelState(client, index, volume, pan, mute ? 1 : 0, solo ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetLocalChannelInfo(JNIEnv* env, jobject thiz, jlong clientPtr,
    jint channelIndex, jstring name, jint setsrcch, jint srcch, jint setxmit, jint xmit, jint setflags, jint flags) {

    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* nameStr = env->GetStringUTFChars(name, nullptr);
    NinjamClient_setLocalChannelInfo(client, channelIndex, nameStr, setsrcch, srcch, setxmit, xmit, setflags, flags);
    env->ReleaseStringUTFChars(name, nameStr);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetUserChannelState(JNIEnv* env, jobject thiz, jlong clientPtr,
    jstring username, jint channelIndex, jfloat volume, jfloat pan, jboolean mute, jboolean subscribed) {

    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);

    float vol = volume;
    float p = pan;
    int32_t m = mute ? 1 : 0;
    int32_t s = subscribed ? 1 : 0;
    NinjamClient_setUserChannelState(client, user, channelIndex, &vol, &p, &m, &s);

    env->ReleaseStringUTFChars(username, user);
}

// ============================================================================
// Volume Controls
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetMasterVolume(JNIEnv* env, jobject thiz, jlong clientPtr,
    jfloat volume, jfloat pan, jboolean mute) {

    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_setMasterVolume(client, volume, pan, mute ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetMetronome(JNIEnv* env, jobject thiz, jlong clientPtr,
    jfloat volume, jboolean mute, jfloat pan, jint channelIndex) {

    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_setMetronome(client, volume, mute ? 1 : 0, pan, channelIndex);
}

// ============================================================================
// Chat
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSendChatMessage(JNIEnv* env, jobject thiz, jlong clientPtr, jstring message) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* msg = env->GetStringUTFChars(message, nullptr);
    NinjamClient_sendChatMessage(client, msg);
    env->ReleaseStringUTFChars(message, msg);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSendPrivateMessage(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jstring message) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);
    const char* msg = env->GetStringUTFChars(message, nullptr);
    NinjamClient_sendPrivateMessage(client, user, msg);
    env->ReleaseStringUTFChars(username, user);
    env->ReleaseStringUTFChars(message, msg);
}

// ============================================================================
// Session Info
// ============================================================================
JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetBPM(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    return NinjamClient_getBPM(client);
}

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetBPI(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    return NinjamClient_getBPI(client);
}

JNIEXPORT jdouble JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetIntervalPosition(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    return NinjamClient_getIntervalPosition(client);
}

// ============================================================================
// Remote Users
// ============================================================================
JNIEXPORT jobjectArray JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetRemoteUserNames(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);

    int count = 0;
    const char** names = NinjamClient_getRemoteUserNames(client, &count);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(count, stringClass, nullptr);

    for (int i = 0; i < count; i++) {
        jstring jName = env->NewStringUTF(names[i] ? names[i] : "");
        env->SetObjectArrayElement(result, i, jName);
        env->DeleteLocalRef(jName);
    }

    NinjamClient_freeRemoteUserNames(names, count);
    return result;
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeInvalidateUsersCache(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    NinjamClient_invalidateUsersCache(client);
}

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetUserChannelCount(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);
    int count = NinjamClient_getUserChannelCount(client, user);
    env->ReleaseStringUTFChars(username, user);
    return count;
}

// ============================================================================
// Peak Meters
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetOutputPeaks(JNIEnv* env, jobject thiz, jlong clientPtr, jfloatArray peaks) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    jfloat* p = env->GetFloatArrayElements(peaks, nullptr);
    NinjamClient_getOutputPeaks(client, &p[0], &p[1]);
    env->ReleaseFloatArrayElements(peaks, p, 0);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetLocalChannelPeaks(JNIEnv* env, jobject thiz, jlong clientPtr, jint channelIndex, jfloatArray peaks) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    jfloat* p = env->GetFloatArrayElements(peaks, nullptr);
    NinjamClient_getLocalChannelPeaks(client, channelIndex, &p[0], &p[1]);
    env->ReleaseFloatArrayElements(peaks, p, 0);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetUserChannelPeaks(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jint channelIndex, jfloatArray peaks) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);
    jfloat* p = env->GetFloatArrayElements(peaks, nullptr);
    NinjamClient_getUserChannelPeaks(client, user, channelIndex, &p[0], &p[1]);
    env->ReleaseFloatArrayElements(peaks, p, 0);
    env->ReleaseStringUTFChars(username, user);
}

// ============================================================================
// User Channel Queries
// ============================================================================
JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetUserChannelName(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jint channelIndex) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);
    const char* name = NinjamClient_getUserChannelName(client, user, channelIndex);
    env->ReleaseStringUTFChars(username, user);
    return env->NewStringUTF(name ? name : "");
}

// Returns channel state as float array: [volume, pan, muted, subscribed, isStereo, channelPairIndex]
JNIEXPORT jfloatArray JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetUserChannelState(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username, jint channelIndex) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);

    float volume = 1.0f, pan = 0.0f;
    int32_t muted = 0, subscribed = 1;
    bool isStereo = false;
    int32_t channelPairIndex = -1;

    int32_t result = NinjamClient_getUserChannelState(client, user, channelIndex,
        &volume, &pan, &muted, &subscribed, &isStereo, &channelPairIndex);

    env->ReleaseStringUTFChars(username, user);

    jfloatArray state = env->NewFloatArray(7);
    float data[7] = {
        (float)result,           // [0] success (1) or not (0)
        volume,                  // [1]
        pan,                     // [2]
        (float)muted,            // [3]
        (float)subscribed,       // [4]
        isStereo ? 1.0f : 0.0f, // [5]
        (float)channelPairIndex  // [6]
    };
    env->SetFloatArrayRegion(state, 0, 7, data);
    return state;
}

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeIsUserSoloed(JNIEnv* env, jobject thiz, jlong clientPtr, jstring username) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* user = env->GetStringUTFChars(username, nullptr);
    int result = NinjamClient_isUserSoloed(client, user);
    env->ReleaseStringUTFChars(username, user);
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetLocalChannelName(JNIEnv* env, jobject thiz, jlong clientPtr, jint channelIndex) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* name = NinjamClient_getLocalChannelName(client, channelIndex);
    return env->NewStringUTF(name ? name : "");
}

JNIEXPORT jstring JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetLocalUserName(JNIEnv* env, jobject thiz, jlong clientPtr) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    const char* name = NinjamClient_getLocalUserName(client);
    return env->NewStringUTF(name ? name : "");
}

// ============================================================================
// Callback Setup
// ============================================================================
JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetCallbackTarget(JNIEnv* env, jobject thiz, jlong clientPtr, jobject callbackTarget) {
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);

    // Clean up previous callback ref
    if (g_callbackCtx.callbackRef) {
        env->DeleteWeakGlobalRef(g_callbackCtx.callbackRef);
        g_callbackCtx.callbackRef = nullptr;
    }

    if (!callbackTarget) return;

    // Create weak global ref to callback object
    g_callbackCtx.callbackRef = env->NewWeakGlobalRef(callbackTarget);

    // Cache method IDs
    jclass cls = env->GetObjectClass(callbackTarget);
    g_callbackCtx.onConnectedMethod = env->GetMethodID(cls, "onConnected", "()V");
    g_callbackCtx.onDisconnectedMethod = env->GetMethodID(cls, "onDisconnected", "(I)V");
    g_callbackCtx.onChatMessageMethod = env->GetMethodID(cls, "onChatMessage", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_callbackCtx.onLicenseMethod = env->GetMethodID(cls, "onLicense", "(Ljava/lang/String;)I");
    g_callbackCtx.onIntervalMethod = env->GetMethodID(cls, "onInterval", "(II)V");
    env->DeleteLocalRef(cls);

    // Set C callbacks on the NinjamClient
    NinjamClient_setOnConnected(client, jni_onConnected);
    NinjamClient_setOnDisconnected(client, jni_onDisconnected);
    NinjamClient_setChatCallback(client, jni_onChatMessage);
    NinjamClient_setLicenseCallback(client, jni_onLicense);
    NinjamClient_setIntervalCallback(client, jni_onInterval);

    LOGI("JNI callback target set, method IDs cached");
}

// ============================================================================
// Audio Engine (Oboe)
// ============================================================================

// Global engine instance (one per app, like iOS AudioSessionManager)
static OboeEngine* g_engine = nullptr;

JNIEXPORT jlong JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeCreateAudioEngine(JNIEnv* env, jobject thiz) {
    if (g_engine) {
        LOGI("Audio engine already exists, reusing");
        return reinterpret_cast<jlong>(g_engine);
    }
    g_engine = new OboeEngine();
    LOGI("Audio engine created: %p", g_engine);
    return reinterpret_cast<jlong>(g_engine);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeDestroyAudioEngine(JNIEnv* env, jobject thiz, jlong enginePtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (engine) {
        delete engine;
        if (engine == g_engine) g_engine = nullptr;
        LOGI("Audio engine destroyed");
    }
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeAudioEngineSetClient(JNIEnv* env, jobject thiz, jlong enginePtr, jlong clientPtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    auto* client = reinterpret_cast<NinjamClientRef*>(clientPtr);
    if (engine) engine->setClient(client);
}

JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeAudioEngineStart(JNIEnv* env, jobject thiz, jlong enginePtr, jint sampleRate, jint framesPerBuffer) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine) return JNI_FALSE;
    return engine->start(sampleRate, framesPerBuffer) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeAudioEngineStop(JNIEnv* env, jobject thiz, jlong enginePtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (engine) engine->stop();
}

JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeAudioEngineIsRunning(JNIEnv* env, jobject thiz, jlong enginePtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    return (engine && engine->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeAudioEngineGetSampleRate(JNIEnv* env, jobject thiz, jlong enginePtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    return engine ? engine->getSampleRate() : 0;
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeAudioEngineGetOutputPeaks(JNIEnv* env, jobject thiz, jlong enginePtr, jfloatArray peaks) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine) return;
    jfloat* p = env->GetFloatArrayElements(peaks, nullptr);
    engine->getOutputPeaks(&p[0], &p[1]);
    env->ReleaseFloatArrayElements(peaks, p, 0);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeAudioEngineGetInputPeaks(JNIEnv* env, jobject thiz, jlong enginePtr, jfloatArray peaks) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine) return;
    jfloat* p = env->GetFloatArrayElements(peaks, nullptr);
    engine->getInputPeaks(&p[0], &p[1]);
    env->ReleaseFloatArrayElements(peaks, p, 0);
}

// ============================================================================
// Session Recording
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeStartRecording(JNIEnv* env, jobject thiz, jlong enginePtr,
    jstring recordingsDir, jstring roomName, jstring myUsername, jobjectArray participants) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine || !engine->getRecorder()) return JNI_FALSE;

    const char* dir = env->GetStringUTFChars(recordingsDir, nullptr);
    const char* room = env->GetStringUTFChars(roomName, nullptr);
    const char* user = env->GetStringUTFChars(myUsername, nullptr);

    int count = env->GetArrayLength(participants);
    std::vector<const char*> parts(count);
    std::vector<jstring> jParts(count);
    for (int i = 0; i < count; i++) {
        jParts[i] = (jstring)env->GetObjectArrayElement(participants, i);
        parts[i] = env->GetStringUTFChars(jParts[i], nullptr);
    }

    bool result = engine->getRecorder()->start(dir, room, user, parts.data(), count);

    for (int i = 0; i < count; i++) {
        env->ReleaseStringUTFChars(jParts[i], parts[i]);
        env->DeleteLocalRef(jParts[i]);
    }
    env->ReleaseStringUTFChars(myUsername, user);
    env->ReleaseStringUTFChars(roomName, room);
    env->ReleaseStringUTFChars(recordingsDir, dir);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeStopRecording(JNIEnv* env, jobject thiz, jlong enginePtr,
    jobjectArray currentParticipants) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine || !engine->getRecorder()) return nullptr;

    int partCount = currentParticipants ? env->GetArrayLength(currentParticipants) : 0;
    std::vector<const char*> parts(partCount);
    std::vector<jstring> jParts(partCount);
    for (int i = 0; i < partCount; i++) {
        jParts[i] = (jstring)env->GetObjectArrayElement(currentParticipants, i);
        parts[i] = env->GetStringUTFChars(jParts[i], nullptr);
    }

    char tempPath[512] = {};
    char roomNameBuf[256] = {};
    char suggestedFilename[512] = {};
    double duration = 0;
    int64_t fileSize = 0;

    bool stopped = engine->getRecorder()->stop(
        partCount > 0 ? parts.data() : nullptr, partCount,
        tempPath, sizeof(tempPath),
        roomNameBuf, sizeof(roomNameBuf),
        suggestedFilename, sizeof(suggestedFilename),
        &duration, &fileSize);

    for (int i = 0; i < partCount; i++) {
        env->ReleaseStringUTFChars(jParts[i], parts[i]);
        env->DeleteLocalRef(jParts[i]);
    }

    if (!stopped) return nullptr;

    // Return as String array: [tempPath, roomName, suggestedFilename, duration, fileSize]
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(5, stringClass, nullptr);

    char durationStr[32], fileSizeStr[32];
    snprintf(durationStr, sizeof(durationStr), "%.3f", duration);
    snprintf(fileSizeStr, sizeof(fileSizeStr), "%lld", (long long)fileSize);

    env->SetObjectArrayElement(result, 0, env->NewStringUTF(tempPath));
    env->SetObjectArrayElement(result, 1, env->NewStringUTF(roomNameBuf));
    env->SetObjectArrayElement(result, 2, env->NewStringUTF(suggestedFilename));
    env->SetObjectArrayElement(result, 3, env->NewStringUTF(durationStr));
    env->SetObjectArrayElement(result, 4, env->NewStringUTF(fileSizeStr));

    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSaveRecording(JNIEnv* env, jobject thiz, jlong enginePtr, jstring finalPath) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine || !engine->getRecorder()) return JNI_FALSE;

    const char* path = env->GetStringUTFChars(finalPath, nullptr);
    bool result = engine->getRecorder()->save(path);
    env->ReleaseStringUTFChars(finalPath, path);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeDiscardRecording(JNIEnv* env, jobject thiz, jlong enginePtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (engine && engine->getRecorder()) engine->getRecorder()->discard();
}

JNIEXPORT jdouble JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetRecordingElapsedTime(JNIEnv* env, jobject thiz, jlong enginePtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine || !engine->getRecorder()) return 0.0;
    return engine->getRecorder()->getElapsedTime();
}

// ============================================================================
// Audio FX Processing
// ============================================================================

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetFxEnabled(JNIEnv* env, jobject thiz, jlong enginePtr, jstring fxName, jboolean enabled) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine || !engine->getFXProcessor()) return;
    const char* name = env->GetStringUTFChars(fxName, nullptr);
    engine->getFXProcessor()->setEnabled(name, enabled);
    env->ReleaseStringUTFChars(fxName, name);
}

JNIEXPORT void JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeSetFxParameter(JNIEnv* env, jobject thiz, jlong enginePtr, jstring fxName, jstring param, jfloat value) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    if (!engine || !engine->getFXProcessor()) return;
    const char* name = env->GetStringUTFChars(fxName, nullptr);
    const char* p = env->GetStringUTFChars(param, nullptr);
    engine->getFXProcessor()->setParameter(name, p, value);
    env->ReleaseStringUTFChars(param, p);
    env->ReleaseStringUTFChars(fxName, name);
}

JNIEXPORT jfloatArray JNICALL
Java_com_ninjamzap_app_nativeaudio_NinjamClientBridge_nativeGetFxState(JNIEnv* env, jobject thiz, jlong enginePtr) {
    auto* engine = reinterpret_cast<OboeEngine*>(enginePtr);
    jfloatArray result = env->NewFloatArray(11);
    if (!engine || !engine->getFXProcessor()) return result;
    float state[11];
    engine->getFXProcessor()->getState(state);
    env->SetFloatArrayRegion(result, 0, 11, state);
    return result;
}

} // extern "C"
