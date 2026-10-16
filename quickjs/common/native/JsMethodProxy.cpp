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

JsMethodProxy::JsMethodProxy(Context* context, JNIEnv* env, const std::string& name, JSAtom atom,
                             jobject method)
    : name(name), methodId(env->FromReflectedMethod(method)), jsContext(context->jsContext),
      atom(atom), isVarArgs(false) {
  const jclass methodClass = env->GetObjectClass(method);

  const jmethodID getReturnType =
      env->GetMethodID(methodClass, "getReturnType", "()Ljava/lang/Class;");
  const auto returnedClass = getReturnType
      ? static_cast<jclass>(env->CallObjectMethod(method, getReturnType))
      : nullptr;
  if (!env->ExceptionCheck()) {
    resultLoader = context->getJsToJavaConverter(env, returnedClass, true);
  }
  if (!env->ExceptionCheck()) {
    const auto returnTypeName = getName(env, returnedClass);
    if (returnTypeName == "int" || returnTypeName == "double" || returnTypeName == "boolean") {
      primitiveReturnType = returnTypeName;
    }
  }
  env->DeleteLocalRef(returnedClass);

  const jmethodID isVarArgsMethod =
      env->ExceptionCheck() ? nullptr : env->GetMethodID(methodClass, "isVarArgs", "()Z");
  if (isVarArgsMethod) {
    isVarArgs = env->CallBooleanMethod(method, isVarArgsMethod);
  }

  const jmethodID getParameterTypes = env->ExceptionCheck()
      ? nullptr
      : env->GetMethodID(methodClass, "getParameterTypes", "()[Ljava/lang/Class;");
  jobjectArray parameterTypes = getParameterTypes
      ? static_cast<jobjectArray>(env->CallObjectMethod(method, getParameterTypes))
      : nullptr;
  if (parameterTypes && !env->ExceptionCheck()) {
    const jsize numArgs = env->GetArrayLength(parameterTypes);
    for (jsize i = 0; i < numArgs && !env->ExceptionCheck(); ++i) {
      auto parameterType = env->GetObjectArrayElement(parameterTypes, i);
      if (!parameterType) break;
      argumentLoaders.push_back(
          context->getJavaToJsConverter(env, static_cast<jclass>(parameterType), true));
      env->DeleteLocalRef(parameterType);
    }
  }
  env->DeleteLocalRef(parameterTypes);
  env->DeleteLocalRef(methodClass);
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
  int numArgs;
  jvalue arg;
  for (numArgs = 0; numArgs < totalArgs && !failed && !env->ExceptionCheck(); numArgs++) {
    arg.l = env->GetObjectArrayElement(args, numArgs);
    auto value = argumentLoaders[numArgs](context, env, arg);
    env->DeleteLocalRef(arg.l);
    if (JS_IsException(value)) {
      failed = true;
    } else if (!isVarArgs || numArgs < totalArgs - 1) {
      arguments.push_back(value);
    } else if (JS_IsArray(context->jsContext, value)) {
      jsize length = 0;
      if (context->getArrayLength(env, value, &length)) {
        for (jsize i = 0; i < length; i++) {
          arguments.push_back(JS_GetPropertyUint32(context->jsContext, value, i));
        }
      }
      JS_FreeValue(context->jsContext, value);
    } else {
      arguments.push_back(value);
    }
  }

  jobject result;
  if (failed) {
    // Rethrows the original Java exception if the JavaScript error carries one.
    context->throwJsException(env, JS_EXCEPTION);
    result = nullptr;
  } else if (!env->ExceptionCheck()) {
    JSValue callResult = JS_Invoke(context->jsContext, thisPointer, atom, arguments.size(),
                                   arguments.data());
    result = resultLoader(context, env, callResult).l;
    if (!result && !env->ExceptionCheck() && !primitiveReturnType.empty()) {
      // The Java proxy would otherwise fail to unbox null with an unrelated NullPointerException.
      throwJavaException(env, "java/lang/IllegalArgumentException",
                         "Cannot convert value %s to %s",
                         describeValue(context->jsContext, callResult).c_str(),
                         primitiveReturnType.c_str());
    }
    JS_FreeValue(context->jsContext, callResult);
  } else {
    result = nullptr;
  }
  for (JSValue argument : arguments) {
    JS_FreeValue(context->jsContext, argument);
  }

  return result;
}
