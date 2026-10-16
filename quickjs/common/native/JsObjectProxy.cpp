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
#include "JsObjectProxy.h"
#include "quickjs/quickjs.h"
#include "Context.h"
#include "ExceptionThrowers.h"
#include "JsMethodProxy.h"

JsObjectProxy::JsObjectProxy(Context* context, const std::string& name, JSAtom nameAtom)
    : name(name), nameAtom(nameAtom), context(context) {
}

JsObjectProxy::~JsObjectProxy() {
  for (auto method : methods) {
    delete method;
  }
  JS_FreeAtom(context->jsContext, nameAtom);
}

bool JsObjectProxy::hasSameMethods(const JsObjectProxy& other) const {
  if (name != other.name || methods.size() != other.methods.size()) return false;
  for (size_t i = 0; i < methods.size(); i++) {
    if (methods[i]->methodId != other.methods[i]->methodId) return false;
  }
  return true;
}

jobject JsObjectProxy::call(Context *context, JNIEnv* env, jint methodIndex, jobjectArray args) const {
  if (methodIndex < 0 || static_cast<size_t>(methodIndex) >= methods.size()) {
    throwJsExceptionFmt(env, context, "Could not find method %d on %s", methodIndex, name.c_str());
    return nullptr;
  }

  JSValue global = JS_GetGlobalObject(context->jsContext);
  JSValue thisPointer = JS_GetProperty(context->jsContext, global, nameAtom);
  JS_FreeValue(context->jsContext, global);
  if (JS_IsException(thisPointer)) {
    // Report the getter's exception rather than failing later on an undefined receiver.
    context->throwJsException(env, thisPointer);
    return nullptr;
  }

  jobject result = methods[methodIndex]->call(context, env, thisPointer, args);
  JS_FreeValue(context->jsContext, thisPointer);
  return result;
}
