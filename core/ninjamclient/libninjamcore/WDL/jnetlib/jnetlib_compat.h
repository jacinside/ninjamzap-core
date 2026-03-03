// WDL/jnetlib/jnetlib_compat.h
#ifndef JNETLIB_COMPAT_H
#define JNETLIB_COMPAT_H

#include "jnetlib.h"

#ifdef __cplusplus

// Forward declare both connection types
class JNL_Connection;
class JNL_IConnection;

// Type-safe connection creation function
inline JNL_Connection* createConnection() {
    // Create a JNL_Connection
    return new JNL_Connection();
}

// Conversion function - only use when necessary
inline JNL_Connection* connectionFromIConnection(JNL_IConnection* icon) {
    // This is a safe cast only if one class is derived from the other
    // or if they share the same memory layout
    return reinterpret_cast<JNL_Connection*>(icon);
}

// Helper macro for connection assignments with type conversion
#define SAFE_CONNECTION_ASSIGN(dest, src) \
    dest = (decltype(dest))(src)

#endif // __cplusplus
// Platform-specific compatibility
// Cross-platform type definitions
#ifdef _WIN32
  // On Windows, use the standard Windows types
  #include <windows.h>
#else
  // On non-Windows platforms, define Windows types
  typedef unsigned short WCHAR;
  typedef int BOOL;
  #ifndef FALSE
    #define FALSE 0
  #endif
  #ifndef TRUE
    #define TRUE 1
  #endif
#endif

// Platform-specific compatibility
#ifndef WDL_UTF8_FUNCTIONS_DEFINED
#define WDL_UTF8_FUNCTIONS_DEFINED

#ifdef _WIN32
  // On Windows, include the real implementation
  #include "win32_utf8.h"
#else
// On macOS/iOS, provide stub implementations
#define WDL_HasUTF8 0
static inline int WDL_UTF8_GetCharLength(const char* str) { return 1; }
static inline WCHAR *WDL_UTF8ToWC(const char* str, BOOL* err) {
  if (err) *err = FALSE;
  return NULL;
}
static inline char *WDL_WCToUTF8(const WCHAR* wc) { return NULL; }
#endif

#endif // WDL_UTF8_FUNCTIONS_DEFINED
#endif // JNETLIB_COMPAT_H
