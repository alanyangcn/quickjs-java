/*
 * Copyright (C) 2026 Square, Inc.
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
#ifndef QUICKJS_ANDROID_SCOPED_H
#define QUICKJS_ANDROID_SCOPED_H

#include <jni.h>
#include "quickjs/quickjs.h"

/** Deletes a JNI local reference when it goes out of scope. */
template <typename T>
class ScopedLocalRef {
public:
  ScopedLocalRef(JNIEnv* env, T ref) : env(env), ref(ref) {
  }

  ~ScopedLocalRef() {
    if (ref) env->DeleteLocalRef(ref);
  }

  T get() const {
    return ref;
  }

  /** Gives up ownership, for example to return the reference to Java. */
  T release() {
    T result = ref;
    ref = nullptr;
    return result;
  }

  explicit operator bool() const {
    return ref != nullptr;
  }

  ScopedLocalRef(const ScopedLocalRef&) = delete;
  ScopedLocalRef& operator=(const ScopedLocalRef&) = delete;

private:
  JNIEnv* const env;
  T ref;
};

/** Pushes a JNI local reference frame and pops it when it goes out of scope. */
class ScopedLocalFrame {
public:
  ScopedLocalFrame(JNIEnv* env, jint capacity) : env(env), pushed(env->PushLocalFrame(capacity) == 0) {
  }

  ~ScopedLocalFrame() {
    if (pushed) env->PopLocalFrame(nullptr);
  }

  /** False if the frame couldn't be pushed; an OutOfMemoryError is then pending. */
  bool ok() const {
    return pushed;
  }

  ScopedLocalFrame(const ScopedLocalFrame&) = delete;
  ScopedLocalFrame& operator=(const ScopedLocalFrame&) = delete;

private:
  JNIEnv* const env;
  const bool pushed;
};

/** Frees a JavaScript value when it goes out of scope. */
class ScopedJSValue {
public:
  ScopedJSValue(JSContext* jsContext, JSValue value) : jsContext(jsContext), value(value) {
  }

  ~ScopedJSValue() {
    JS_FreeValue(jsContext, value);
  }

  const JSValue& get() const {
    return value;
  }

  /** Gives up ownership, for example to pass the value to a function that frees it. */
  JSValue release() {
    JSValue result = value;
    value = JS_UNDEFINED;
    return result;
  }

  ScopedJSValue(const ScopedJSValue&) = delete;
  ScopedJSValue& operator=(const ScopedJSValue&) = delete;

private:
  JSContext* const jsContext;
  JSValue value;
};

/** Frees a JavaScript atom when it goes out of scope. */
class ScopedAtom {
public:
  ScopedAtom(JSContext* jsContext, JSAtom atom) : jsContext(jsContext), atom(atom) {
  }

  ~ScopedAtom() {
    if (atom != JS_ATOM_NULL) JS_FreeAtom(jsContext, atom);
  }

  JSAtom get() const {
    return atom;
  }

  ScopedAtom(const ScopedAtom&) = delete;
  ScopedAtom& operator=(const ScopedAtom&) = delete;

private:
  JSContext* const jsContext;
  const JSAtom atom;
};

/** Frees a string from JS_ToCString() when it goes out of scope. */
class ScopedCString {
public:
  ScopedCString(JSContext* jsContext, const char* string) : jsContext(jsContext), string(string) {
  }

  ~ScopedCString() {
    if (string) JS_FreeCString(jsContext, string);
  }

  const char* get() const {
    return string;
  }

  ScopedCString(const ScopedCString&) = delete;
  ScopedCString& operator=(const ScopedCString&) = delete;

private:
  JSContext* const jsContext;
  const char* const string;
};

#endif //QUICKJS_ANDROID_SCOPED_H
