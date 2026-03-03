// ios/NativeAudioModule/NJClient/win32_utf8_compat.h
#ifndef WIN32_UTF8_COMPAT_H
#define WIN32_UTF8_COMPAT_H

// Platform detection
#ifdef _WIN32
  #include "../WDL/win32_utf8.h"  // Use original on Windows
#else
  // macOS/iOS compatible definitions
  #include <string>
  #include <cstdlib>
  
  // Common Windows types needed by WDL
  typedef unsigned short WCHAR;
  typedef int BOOL;
  typedef unsigned int UINT;
  typedef void* HANDLE;
  typedef unsigned long DWORD;
  typedef const char* LPCSTR;
  typedef char* LPSTR;
  
  #ifndef FALSE
    #define FALSE 0
  #endif
  
  #ifndef TRUE
    #define TRUE 1
  #endif
  
  // UTF8 helper functions that might be used by WDL
  static inline BOOL WDL_HasUTF8(const char* str) { return FALSE; }
  static inline int WDL_UTF8_GetCharLength(const char* str) { return 1; }
  static inline WCHAR* WDL_UTF8ToWC(const char* str, BOOL* err) {
    if (err) *err = FALSE;
    return NULL;
  }
  static inline char* WDL_WCToUTF8(const WCHAR* wc) { return NULL; }
  
  #define WDL_UTF8_FUNCTIONS_DEFINED
#endif

#endif // WIN32_UTF8_COMPAT_H
