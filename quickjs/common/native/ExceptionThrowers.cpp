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
#include "Scoped.h"

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
  ScopedLocalRef<jbyteArray> bytes(env, env->NewByteArray(length));
  if (!bytes) return nullptr;
  env->SetByteArrayRegion(bytes.get(), 0, length, reinterpret_cast<const jbyte*>(utf8));
  ScopedLocalRef<jclass> stringClass(env, env->FindClass("java/lang/String"));
  if (!stringClass) return nullptr;
  ScopedLocalRef<jstring> charsetName(env, env->NewStringUTF("UTF-8"));
  if (!charsetName) return nullptr;
  jmethodID constructor = env->GetMethodID(stringClass.get(), "<init>", "([BLjava/lang/String;)V");
  if (!constructor) return nullptr;
  return static_cast<jstring>(
      env->NewObject(stringClass.get(), constructor, bytes.get(), charsetName.get()));
}

} // anonymous namespace

void throwJavaException(JNIEnv *env, const char *exceptionClass, const char *fmt, ...) {
  if (env->ExceptionCheck()) return; // Don't replace an exception that is already pending.
  char msg[512];
  va_list args;
  va_start (args, fmt);
  const size_t length = formatMessage(msg, sizeof(msg), fmt, args);
  va_end (args);
  ScopedLocalRef<jclass> clazz(env, env->FindClass(exceptionClass));
  if (!clazz) return;
  ScopedLocalRef<jstring> message(env, newStringFromUtf8(env, msg, length));
  if (!message) return;
  jmethodID constructor = env->GetMethodID(clazz.get(), "<init>", "(Ljava/lang/String;)V");
  if (!constructor) return;
  ScopedLocalRef<jthrowable> exception(
      env, static_cast<jthrowable>(env->NewObject(clazz.get(), constructor, message.get())));
  if (exception) env->Throw(exception.get());
}

void throwJsExceptionFmt(JNIEnv *env, const Context *context, const char *fmt, ...) {
  if (env->ExceptionCheck()) return; // Don't replace an exception that is already pending.
  char msg[512];
  va_list args;
  va_start (args, fmt);
  const size_t length = formatMessage(msg, sizeof(msg), fmt, args);
  va_end (args);
  ScopedLocalRef<jstring> message(env, newStringFromUtf8(env, msg, length));
  if (!message) return;
  ScopedLocalRef<jthrowable> exception(env, static_cast<jthrowable>(env->NewObject(
      context->quickJsExceptionClass, context->quickJsExceptionConstructor, message.get(),
      nullptr)));
  if (exception) env->Throw(exception.get());
}
