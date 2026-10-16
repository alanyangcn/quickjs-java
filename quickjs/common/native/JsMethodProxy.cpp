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

#include "JsMethodProxy.h"
#include <algorithm>
#include "Context.h"
#include "ExceptionThrowers.h"
#include "Scoped.h"

JsMethodProxy::JsMethodProxy(Context* context, JNIEnv* env, const std::string& name, JSAtom atom,
                             jobject method)
    : name(name), methodId(env->FromReflectedMethod(method)), jsContext(context->jsContext),
      atom(atom), isVarArgs(false) {
  ScopedLocalRef<jclass> methodClass(env, env->GetObjectClass(method));

  const jmethodID getReturnType =
      env->GetMethodID(methodClass.get(), "getReturnType", "()Ljava/lang/Class;");
  if (!getReturnType) return;
  ScopedLocalRef<jclass> returnedClass(
      env, static_cast<jclass>(env->CallObjectMethod(method, getReturnType)));
  if (env->ExceptionCheck()) return;
  resultLoader = context->getJsToJavaConverter(env, returnedClass.get(), true);
  if (env->ExceptionCheck()) return;
  const auto returnTypeName = getName(env, returnedClass.get());
  if (returnTypeName == "int" || returnTypeName == "double" || returnTypeName == "boolean") {
    primitiveReturnType = returnTypeName;
  }

  const jmethodID isVarArgsMethod = env->GetMethodID(methodClass.get(), "isVarArgs", "()Z");
  if (!isVarArgsMethod) return;
  isVarArgs = env->CallBooleanMethod(method, isVarArgsMethod);
  if (env->ExceptionCheck()) return;

  const jmethodID getParameterTypes =
      env->GetMethodID(methodClass.get(), "getParameterTypes", "()[Ljava/lang/Class;");
  if (!getParameterTypes) return;
  ScopedLocalRef<jobjectArray> parameterTypes(
      env, static_cast<jobjectArray>(env->CallObjectMethod(method, getParameterTypes)));
  if (!parameterTypes || env->ExceptionCheck()) return;
  const jsize numArgs = env->GetArrayLength(parameterTypes.get());
  for (jsize i = 0; i < numArgs && !env->ExceptionCheck(); ++i) {
    ScopedLocalRef<jclass> parameterType(
        env, static_cast<jclass>(env->GetObjectArrayElement(parameterTypes.get(), i)));
    if (!parameterType) break;
    argumentLoaders.push_back(context->getJavaToJsConverter(env, parameterType.get(), true));
  }
}

JsMethodProxy::~JsMethodProxy() {
  JS_FreeAtom(jsContext, atom);
}

jobject JsMethodProxy::call(Context* context, JNIEnv* env, JSValue thisPointer, jobjectArray args) const {
  const auto totalArgs = std::min<int>(argumentLoaders.size(), args ? env->GetArrayLength(args) : 0);
  std::vector<JSValue> arguments;
  // Converters report failures either as a pending Java exception or by returning JS_EXCEPTION
  // with the cause pending in JavaScript. Either way, the JavaScript function must not be called.
  bool failed = false;
  for (int numArgs = 0; numArgs < totalArgs && !failed && !env->ExceptionCheck(); numArgs++) {
    jvalue arg;
    arg.l = env->GetObjectArrayElement(args, numArgs);
    if (env->ExceptionCheck()) break;
    ScopedLocalRef<jobject> argRef(env, arg.l);
    JSValue value = argumentLoaders[numArgs](context, env, arg);
    if (JS_IsException(value)) {
      failed = true;
    } else if (!isVarArgs || numArgs < totalArgs - 1) {
      arguments.push_back(value);
    } else if (JS_IsArray(context->jsContext, value)) {
      // Spread varargs into separate arguments.
      ScopedJSValue varArgs(context->jsContext, value);
      jsize length = 0;
      if (context->getArrayLength(env, varArgs.get(), &length)) {
        for (jsize i = 0; i < length; i++) {
          arguments.push_back(JS_GetPropertyUint32(context->jsContext, varArgs.get(), i));
        }
      }
    } else {
      arguments.push_back(value);
    }
  }

  jobject result = nullptr;
  if (failed) {
    // Rethrows the original Java exception if the JavaScript error carries one.
    context->throwJsException(env, JS_EXCEPTION);
  } else if (!env->ExceptionCheck()) {
    ScopedJSValue callResult(context->jsContext, JS_Invoke(
        context->jsContext, thisPointer, atom, arguments.size(), arguments.data()));
    result = resultLoader(context, env, callResult.get()).l;
    if (!result && !env->ExceptionCheck() && !primitiveReturnType.empty()) {
      // The Java proxy would otherwise fail to unbox null with an unrelated NullPointerException.
      throwJavaException(env, "java/lang/IllegalArgumentException",
                         "Cannot convert value %s to %s",
                         describeValue(context->jsContext, callResult.get()).c_str(),
                         primitiveReturnType.c_str());
    }
  }
  for (JSValue argument : arguments) {
    JS_FreeValue(context->jsContext, argument);
  }
  return result;
}
