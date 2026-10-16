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
#include <jni.h>
#include <new>
#include "Context.h"
#include "JsObjectProxy.h"
#include "ExceptionThrowers.h"

namespace {

Context* getContext(JNIEnv* env, jlong context) {
  if (!context) {
    throwJavaException(env, "java/lang/NullPointerException",
                       "Null QuickJs context - did you close your QuickJs?");
  }
  return reinterpret_cast<Context*>(context);
}

/*
 * Tracks native calls into a context. The outermost call records the current thread's stack top,
 * so QuickJS's stack limit applies to whichever thread is using the engine, and clears state left
 * over from an interrupted call.
 */
class CallScope {
public:
  explicit CallScope(JNIEnv* env, Context* context) : env(env), context(context) {
    if (context->nativeCallDepth++ == 0) {
      JS_UpdateStackTop(context->jsRuntime);
    }
  }

  ~CallScope() {
    if (--context->nativeCallDepth == 0 && context->interruptCause) {
      env->DeleteGlobalRef(context->interruptCause);
      context->interruptCause = nullptr;
    }
  }

private:
  JNIEnv* const env;
  Context* const context;
};

} // anonymous namespace

extern "C" JNIEXPORT jlong JNICALL
Java_app_cash_quickjs_QuickJs_createContext(JNIEnv* env, jclass type) {
  Context* c = new(std::nothrow) Context(env);
  if (!c || !c->initialize(env)) {
    delete c;
    c = nullptr;
  }
  return reinterpret_cast<jlong>(c);
}

extern "C" JNIEXPORT void JNICALL
Java_app_cash_quickjs_QuickJs_destroyContext(JNIEnv* env, jobject type, jlong context) {
  delete reinterpret_cast<Context*>(context);
}

extern "C" JNIEXPORT jobject JNICALL
Java_app_cash_quickjs_QuickJs_evaluate__JLjava_lang_String_2Ljava_lang_String_2(JNIEnv* env,
                                                                                    jobject type,
                                                                                    jlong context_,
                                                                                    jstring sourceCode,
                                                                                    jstring fileName) {
  Context* context = getContext(env, context_);
  if (!context) return nullptr;
  CallScope scope(env, context);
  return context->eval(env, sourceCode, fileName);
}

extern "C" JNIEXPORT jlong JNICALL
Java_app_cash_quickjs_QuickJs_get(JNIEnv* env, jobject thiz, jlong _context, jstring name,
                                      jobjectArray methods) {
  Context* context = getContext(env, _context);
  if (!context) return 0L;
  CallScope scope(env, context);
  return reinterpret_cast<jlong>(context->getObjectProxy(env, name, methods));
}

extern "C" JNIEXPORT void JNICALL
Java_app_cash_quickjs_QuickJs_set(JNIEnv* env, jobject thiz, jlong _context, jstring name,
                                      jobject object, jobjectArray methods) {
  Context* context = getContext(env, _context);
  if (!context) return;
  CallScope scope(env, context);
  context->setObjectProxy(env, name, object, methods);
}

extern "C" JNIEXPORT jobject JNICALL
Java_app_cash_quickjs_QuickJs_call(JNIEnv* env, jobject thiz, jlong _context, jlong instance,
                                       jint methodIndex, jobjectArray args) {
  Context* context = getContext(env, _context);
  if (!context) return nullptr;

  const JsObjectProxy* jsObjectProxy = reinterpret_cast<const JsObjectProxy*>(instance);
  if (!jsObjectProxy) {
    throwJavaException(env, "java/lang/NullPointerException", "Invalid JavaScript object");
    return nullptr;
  }

  CallScope scope(env, context);
  return jsObjectProxy->call(context, env, methodIndex, args);
}

extern "C" JNIEXPORT jobject JNICALL
Java_app_cash_quickjs_QuickJs_execute(JNIEnv* env, jobject thiz, jlong _context, jbyteArray bytecode) {
  Context* context = getContext(env, _context);
  if (!context) return nullptr;
  CallScope scope(env, context);
  return context->execute(env, bytecode);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_app_cash_quickjs_QuickJs_compile(JNIEnv* env, jobject thiz, jlong _context, jstring sourceCode, jstring fileName) {
  Context* context = getContext(env, _context);
  if (!context) return nullptr;
  CallScope scope(env, context);
  return context->compile(env, sourceCode, fileName);
}

extern "C" JNIEXPORT jint JNICALL
Java_app_cash_quickjs_QuickJs_executePendingJobs(JNIEnv* env, jobject thiz, jlong _context,
                                                 jint maxJobs) {
  Context* context = getContext(env, _context);
  if (!context) return 0;
  CallScope scope(env, context);
  return context->executePendingJobs(env, maxJobs);
}

extern "C" JNIEXPORT void JNICALL
Java_app_cash_quickjs_QuickJs_setMemoryLimit(JNIEnv* env, jobject thiz, jlong _context,
                                             jlong limit) {
  Context* context = getContext(env, _context);
  if (!context) return;
  JS_SetMemoryLimit(context->jsRuntime, limit > 0 ? static_cast<size_t>(limit) : static_cast<size_t>(-1));
}

extern "C" JNIEXPORT void JNICALL
Java_app_cash_quickjs_QuickJs_setMaxStackSize(JNIEnv* env, jobject thiz, jlong _context,
                                              jlong size) {
  Context* context = getContext(env, _context);
  if (!context) return;
  JS_SetMaxStackSize(context->jsRuntime, static_cast<size_t>(size));
}

extern "C" JNIEXPORT void JNICALL
Java_app_cash_quickjs_QuickJs_setInterruptHandler(JNIEnv* env, jobject thiz, jlong _context,
                                                  jobject handler) {
  Context* context = getContext(env, _context);
  if (!context) return;
  context->setInterruptHandler(env, handler);
}
