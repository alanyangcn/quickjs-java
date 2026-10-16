/*
 * Copyright (C) 2019 Square, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "ExceptionThrowers.h"
#include <cstdarg>
#include <cstdio>
#include "Context.h"

namespace {

/*
 * Formats a message into `msg`. If the message is truncated, a trailing multi-byte UTF-8 sequence
 * that was cut short is dropped so the result is still valid UTF-8. Returns the message length.
 */
size_t formatMessage(char* msg, size_t size, const char* fmt, va_list args) {
  const int written = vsnprintf(msg, size, fmt, args);
  if (written < 0) {
    msg[0] = '\0';
    return 0;
  }
  if (static_cast<size_t>(written) < size) return written;

  size_t length = size - 1;
  size_t lead = length;
  while (lead > 0 && length - lead < 3 && (static_cast<unsigned char>(msg[lead - 1]) & 0xC0) == 0x80) {
    lead--;
  }
  if (lead > 0) {
    const auto c = static_cast<unsigned char>(msg[lead - 1]);
    const size_t expected = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (length - (lead - 1) < expected) length = lead - 1;
  }
  msg[length] = '\0';
  return length;
}

/*
 * Creates a Java string from standard UTF-8. `NewStringUTF()` expects modified UTF-8, which
 * garbles characters outside the BMP.
 */
jstring newStringFromUtf8(JNIEnv* env, const char* utf8, size_t length) {
  jbyteArray bytes = env->NewByteArray(length);
  if (!bytes) return nullptr;
  env->SetByteArrayRegion(bytes, 0, length, reinterpret_cast<const jbyte*>(utf8));
  jclass stringClass = env->FindClass("java/lang/String");
  jstring charsetName = stringClass ? env->NewStringUTF("UTF-8") : nullptr;
  jstring result = nullptr;
  if (charsetName) {
    jmethodID constructor = env->GetMethodID(stringClass, "<init>", "([BLjava/lang/String;)V");
    if (constructor) {
      result = static_cast<jstring>(env->NewObject(stringClass, constructor, bytes, charsetName));
    }
  }
  env->DeleteLocalRef(charsetName);
  env->DeleteLocalRef(stringClass);
  env->DeleteLocalRef(bytes);
  return result;
}

} // anonymous namespace

void throwJavaException(JNIEnv *env, const char *exceptionClass, const char *fmt, ...) {
  if (env->ExceptionCheck()) return; // Don't replace an exception that is already pending.
  char msg[512];
  va_list args;
  va_start (args, fmt);
  const size_t length = formatMessage(msg, sizeof(msg), fmt, args);
  va_end (args);
  jclass clazz = env->FindClass(exceptionClass);
  if (!clazz) return;
  jstring message = newStringFromUtf8(env, msg, length);
  jmethodID constructor = message
      ? env->GetMethodID(clazz, "<init>", "(Ljava/lang/String;)V")
      : nullptr;
  jobject exception = constructor ? env->NewObject(clazz, constructor, message) : nullptr;
  if (exception) env->Throw(static_cast<jthrowable>(exception));
  env->DeleteLocalRef(exception);
  env->DeleteLocalRef(message);
  env->DeleteLocalRef(clazz);
}

void throwJsExceptionFmt(JNIEnv *env, const Context *context, const char *fmt, ...) {
  if (env->ExceptionCheck()) return; // Don't replace an exception that is already pending.
  char msg[512];
  va_list args;
  va_start (args, fmt);
  const size_t length = formatMessage(msg, sizeof(msg), fmt, args);
  va_end (args);
  jstring message = newStringFromUtf8(env, msg, length);
  if (!message) return;
  jobject exception = env->NewObject(context->quickJsExceptionClass,
                                     context->quickJsExceptionConstructor,
                                     message,
                                     NULL);
  if (exception) env->Throw(static_cast<jthrowable>(exception));
  env->DeleteLocalRef(exception);
  env->DeleteLocalRef(message);
}
