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
#ifndef QUICKJS_ANDROID_CONTEXT_H
#define QUICKJS_ANDROID_CONTEXT_H

#include <jni.h>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include "quickjs/quickjs.h"

class JSRuntime;
class JSContext;
class JsObjectProxy;

class Context {
public:
  Context(JNIEnv *env);
  ~Context();

  // Looks up the Java classes and methods used by the bridge. Returns false on failure, possibly
  // with a pending Java exception. The destructor handles partially initialized instances.
  bool initialize(JNIEnv *env);

  JsObjectProxy* getObjectProxy(JNIEnv*, jstring name, jobjectArray methods);
  void setObjectProxy(JNIEnv*, jstring name, jobject object, jobjectArray methods);
  jobject eval(JNIEnv*, jstring source, jstring file);
  jobject execute(JNIEnv*, jbyteArray byteCode);
  jbyteArray compile(JNIEnv*, jstring source, jstring file);
  typedef std::function<JSValueConst(Context*, JNIEnv*, jvalue)> JavaToJavaScript;
  JavaToJavaScript getJavaToJsConverter(JNIEnv*, jclass type, bool boxed);
  typedef std::function<jvalue(Context*, JNIEnv*, JSValueConst)> JavaScriptToJava;
  JavaScriptToJava getJsToJavaConverter(JNIEnv*, jclass type, bool boxed);

  // Converts `value` to a Java object. Values with no Java representation, at any depth, throw
  // if `throwOnUnsupportedType` is true and become null otherwise.
  jobject toJavaObject(JNIEnv*, const JSValue& value, bool throwOnUnsupportedType = true);
  // Reads the length of an array-like value. On failure throws a Java exception, returns false.
  bool getArrayLength(JNIEnv*, const JSValueConst& value, jsize* length) const;
  void throwJsException(JNIEnv*, const JSValue& value) const;
  JSValue throwJavaExceptionFromJs(JNIEnv*) const;

  JNIEnv* getEnv() const;

  static JSValue
  jsCall(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

  jclass getGlobalRef(JNIEnv* env, jclass clazz);
  std::string toCppString(JNIEnv* env, jstring string) const;
  jstring toJavaString(JNIEnv* env, const JSValueConst& value) const;

  JavaVM* javaVm;
  const jint jniVersion;
  JSRuntime *jsRuntime;
  JSContext *jsContext;
  JSClassID jsClassId;
  mutable JSClassID javaExceptionClassId;
  jclass booleanClass;
  jclass integerClass;
  jclass doubleClass;
  jclass objectClass;
  jclass stringClass;
  jstring stringUtf8;
  jclass quickJsExceptionClass;
  jmethodID booleanValueOf;
  jmethodID booleanGetValue;
  jmethodID integerValueOf;
  jmethodID integerGetValue;
  jmethodID doubleValueOf;
  jmethodID doubleGetValue;
  jmethodID stringGetBytes;
  jmethodID stringConstructor;
  jmethodID quickJsExceptionConstructor;
  std::vector<JsObjectProxy*> objectProxies;
  std::unordered_map<std::string, jclass> globalReferences;

  // Largest array, and most array elements in one conversion, passed between JavaScript and Java.
  static const jsize MAX_ARRAY_LENGTH = 1 << 22;
  // Arrays currently being converted by toJavaObject(), used to detect cycles.
  static const size_t MAX_ARRAY_NESTING_DEPTH = 256;
  std::vector<void*> arraysBeingConverted;
  size_t arrayElementsConverted = 0;
};

std::string getName(JNIEnv* env, jobject javaClass);
std::string describeValue(JSContext* jsContext, JSValueConst value);

#endif //QUICKJS_ANDROID_CONTEXT_H
