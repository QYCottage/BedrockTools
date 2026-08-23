#pragma once
//
// Minimal host-side stand-in for the NDK's <android/log.h>.
//
// Paired with tests/fakejni/jni.h so host unit tests can compile the
// preloader headers (pl/Mod.hpp -> pl/Logger.hpp) without the Android
// toolchain. Logging is a no-op on the host. Host tests only.

#include <cstdarg>

enum {
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG = 3,
    ANDROID_LOG_INFO = 4,
    ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6,
    ANDROID_LOG_FATAL = 7,
    ANDROID_LOG_SILENT = 8,
};

inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
inline int __android_log_vprint(int, const char*, const char*, va_list) { return 0; }
inline int __android_log_write(int, const char*, const char*) { return 0; }
