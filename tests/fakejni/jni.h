#pragma once
//
// Minimal host-side stand-in for the Android NDK's <jni.h>.
//
// The unit tests in this directory exercise Android-only code paths
// (ExternalButtonRefresh, and pl/Mod.hpp which includes <jni.h>) on a desktop
// compiler. This header mirrors the small JNI API surface they use, with the
// same call syntax as the real header (variadic Call*Method members that
// forward to a *V hook, like jni.h's inline wrappers do).
//
// It is NOT a JVM: every entry point is a hook assigned by the test.
// Host tests only - never include this from product code.

#include <cstdarg>
#include <cstdint>

using jsize = std::int32_t;
using jint = std::int32_t;
using jlong = std::int64_t;
using jboolean = std::uint8_t;

constexpr jint JNI_OK = 0;
constexpr jint JNI_ERR = -1;
constexpr jint JNI_EDETACHED = -2;
constexpr jint JNI_EVERSION = -3;
constexpr jboolean JNI_TRUE = 1;
constexpr jboolean JNI_FALSE = 0;
constexpr jint JNI_VERSION_1_2 = 0x00010002;
constexpr jint JNI_VERSION_1_4 = 0x00010004;
constexpr jint JNI_VERSION_1_6 = 0x00010006;

struct _jobject {
    int serial = 0; // stable identity for pointer comparisons in tests
};
struct _jclass : _jobject {};
struct _jmethodID {};
struct _jfieldID {};

using jobject = _jobject *;
using jclass = _jclass *;
using jstring = _jobject *; // real JNI strings are opaque; pointer identity is enough here
using jmethodID = _jmethodID *;
using jfieldID = _jfieldID *;

struct JNIEnv {
    // Hooks assigned by the test (mirroring the JNINativeInterface table).
    jclass (*FindClassFn)(JNIEnv *, const char *) = nullptr;
    jmethodID (*GetStaticMethodIDFn)(JNIEnv *, jclass, const char *, const char *) = nullptr;
    jmethodID (*GetMethodIDFn)(JNIEnv *, jclass, const char *, const char *) = nullptr;
    jfieldID (*GetFieldIDFn)(JNIEnv *, jclass, const char *, const char *) = nullptr;
    jclass (*GetSuperclassFn)(JNIEnv *, jclass) = nullptr;
    jobject (*NewLocalRefFn)(JNIEnv *, jobject) = nullptr;
    jobject (*CallStaticObjectMethodVFn)(JNIEnv *, jclass, jmethodID, va_list) = nullptr;
    jint (*CallStaticIntMethodVFn)(JNIEnv *, jclass, jmethodID, va_list) = nullptr;
    jobject (*CallObjectMethodVFn)(JNIEnv *, jobject, jmethodID, va_list) = nullptr;
    void (*CallVoidMethodVFn)(JNIEnv *, jobject, jmethodID, va_list) = nullptr;
    jobject (*GetObjectFieldFn)(JNIEnv *, jobject, jfieldID) = nullptr;
    void (*SetObjectFieldFn)(JNIEnv *, jobject, jfieldID, jobject) = nullptr;
    jstring (*NewStringUTFFn)(JNIEnv *, const char *) = nullptr;
    jsize (*GetStringUTFLengthFn)(JNIEnv *, jstring) = nullptr;
    const char *(*GetStringUTFCharsFn)(JNIEnv *, jstring, jboolean *) = nullptr;
    void (*ReleaseStringUTFCharsFn)(JNIEnv *, jstring, const char *) = nullptr;
    jboolean (*ExceptionCheckFn)(JNIEnv *) = nullptr;
    void (*ExceptionClearFn)(JNIEnv *) = nullptr;
    void (*DeleteLocalRefFn)(JNIEnv *, jobject) = nullptr;
    jboolean (*IsSameObjectFn)(JNIEnv *, jobject, jobject) = nullptr;

    // Same call syntax as the real header.
    jclass FindClass(const char *name) { return FindClassFn(this, name); }
    jmethodID GetStaticMethodID(jclass c, const char *n, const char *s) {
        return GetStaticMethodIDFn(this, c, n, s);
    }
    jmethodID GetMethodID(jclass c, const char *n, const char *s) {
        return GetMethodIDFn(this, c, n, s);
    }
    jfieldID GetFieldID(jclass c, const char *n, const char *s) {
        return GetFieldIDFn(this, c, n, s);
    }
    jclass GetSuperclass(jclass c) { return GetSuperclassFn(this, c); }
    jobject NewLocalRef(jobject o) { return NewLocalRefFn(this, o); }
    jboolean ExceptionCheck() { return ExceptionCheckFn(this); }
    void ExceptionClear() { ExceptionClearFn(this); }
    void DeleteLocalRef(jobject o) { DeleteLocalRefFn(this, o); }
    jboolean IsSameObject(jobject a, jobject b) { return IsSameObjectFn(this, a, b); }
    jobject GetObjectField(jobject o, jfieldID f) { return GetObjectFieldFn(this, o, f); }
    void SetObjectField(jobject o, jfieldID f, jobject v) { return SetObjectFieldFn(this, o, f, v); }
    jstring NewStringUTF(const char *s) { return NewStringUTFFn(this, s); }
    jsize GetStringUTFLength(jstring s) { return GetStringUTFLengthFn(this, s); }
    const char *GetStringUTFChars(jstring s, jboolean *isCopy) {
        return GetStringUTFCharsFn(this, s, isCopy);
    }
    void ReleaseStringUTFChars(jstring s, const char *c) {
        ReleaseStringUTFCharsFn(this, s, c);
    }

    jobject CallStaticObjectMethod(jclass c, jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jobject r = CallStaticObjectMethodVFn(this, c, m, ap);
        va_end(ap);
        return r;
    }
    jint CallStaticIntMethod(jclass c, jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jint r = CallStaticIntMethodVFn(this, c, m, ap);
        va_end(ap);
        return r;
    }
    jobject CallObjectMethod(jobject o, jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jobject r = CallObjectMethodVFn(this, o, m, ap);
        va_end(ap);
        return r;
    }
    void CallVoidMethod(jobject o, jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        CallVoidMethodVFn(this, o, m, ap);
        va_end(ap);
    }
};

struct JavaVMAttachArgs {
    jint version;
    char *name;
    jobject group;
};

struct JavaVM {
    jint (*GetEnvFn)(JavaVM *, void **, jint) = nullptr;
    jint (*AttachCurrentThreadFn)(JavaVM *, JNIEnv **, void *) = nullptr;
    jint (*DetachCurrentThreadFn)(JavaVM *) = nullptr;

    jint GetEnv(void **env, jint version) { return GetEnvFn(this, env, version); }
    jint AttachCurrentThread(JNIEnv **env, void *args) {
        return AttachCurrentThreadFn(this, env, args);
    }
    jint DetachCurrentThread() { return DetachCurrentThreadFn(this); }
};
