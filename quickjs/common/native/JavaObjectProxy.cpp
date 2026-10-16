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
#include "JavaObjectProxy.h"
#include <cstdlib>
#include <cstring>
#include "Context.h"
#include "ExceptionThrowers.h"
#include "JavaMethodProxy.h"
#include "Scoped.h"

JavaObjectProxy::JavaObjectProxy(Context* c, JNIEnv* env, const char* name, jobject object,
                                 jobjectArray methods, JSValueConst proxy)
    : context(c), name(name), javaThis(env->NewGlobalRef(object)) {
  const auto numMethods = env->GetArrayLength(methods);
  functions.resize(numMethods);
  auto f = functions.data();
  for (int i = 0; i < numMethods && !env->ExceptionCheck(); i++) {
    ScopedLocalRef<jobject> method(env, env->GetObjectArrayElement(methods, i));
    if (env->ExceptionCheck()) {
      break;
    }
    proxies.emplace_back(context, env, method.get());
    if (env->ExceptionCheck()) {
      break;
    }
    const auto& proxyMethod = proxies.back();
    // Freed with std::free() in the destructor. strdup() isn't standard C++.
    const auto functionName = static_cast<char*>(std::malloc(proxyMethod.name.size() + 1));
    if (!functionName) {
      throwJavaException(env, "java/lang/OutOfMemoryError", "Cannot allocate method name");
      break;
    }
    std::memcpy(functionName, proxyMethod.name.c_str(), proxyMethod.name.size() + 1);
    // What JS_CFUNC_MAGIC_DEF() expands to, without its C99 designated initializers, which
    // aren't valid C++11.
    JSCFunctionListEntry& entry = f[i];
    entry.name = functionName;
    entry.prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
    entry.def_type = JS_DEF_CFUNC;
    entry.magic = static_cast<int16_t>(i);
    entry.u.func.length = static_cast<uint8_t>(proxyMethod.numArgs());
    entry.u.func.cproto = JS_CFUNC_generic_magic;
    entry.u.func.cfunc.generic_magic = Context::jsCall;
  }

  if (!env->ExceptionCheck()) {
    JS_SetPropertyFunctionList(context->jsContext, proxy, functions.data(), functions.size());
  }
}

JavaObjectProxy::~JavaObjectProxy() {
  context->getEnv()->DeleteGlobalRef(javaThis);
  for (auto& f : functions) {
    std::free(const_cast<char*>(f.name));
  }
}

JSValue
JavaObjectProxy::call(int magic, int argc, JSValueConst* argv) const {
  if (magic >= 0 && static_cast<size_t>(magic) < proxies.size()) {
    return proxies[magic].invoke(context, javaThis, argc, argv);
  } else {
    return JS_ThrowInternalError(context->jsContext, "Function not found");
  }
}
