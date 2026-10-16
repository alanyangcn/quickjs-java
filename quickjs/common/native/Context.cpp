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
#include "Context.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <limits>
#include <new>
#include <assert.h>
#include "JavaObjectProxy.h"
#include "JsObjectProxy.h"
#include "JsMethodProxy.h"
#include "ExceptionThrowers.h"
#include "quickjs/quickjs.h"

/*
 * Converts `string` to standard UTF-8 without a Context, for use while building proxies.
 * GetStringUTFChars() would return modified UTF-8.
 */
static std::string javaStringToUtf8(JNIEnv* env, jstring string) {
  jclass stringClass = env->GetObjectClass(string);
  const jmethodID getBytes = env->GetMethodID(stringClass, "getBytes", "(Ljava/lang/String;)[B");
  env->DeleteLocalRef(stringClass);
  jstring charsetName = getBytes ? env->NewStringUTF("UTF-8") : nullptr;
  auto bytes = charsetName
      ? static_cast<jbyteArray>(env->CallObjectMethod(string, getBytes, charsetName))
      : nullptr;
  std::string result;
  if (bytes && !env->ExceptionCheck()) {
    const jsize length = env->GetArrayLength(bytes);
    if (length > 0) {
      result.resize(length);
      env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(&result[0]));
    }
  }
  env->DeleteLocalRef(bytes);
  env->DeleteLocalRef(charsetName);
  return result;
}

std::string getName(JNIEnv* env, jobject object) {
  auto classType = env->GetObjectClass(object);
  const jmethodID method = env->GetMethodID(classType, "getName", "()Ljava/lang/String;");
  env->DeleteLocalRef(classType);
  if (!method) return {};
  auto javaString = static_cast<jstring>(env->CallObjectMethod(object, method));
  if (env->ExceptionCheck() || !javaString) {
    env->DeleteLocalRef(javaString);
    return {};
  }
  std::string str = javaStringToUtf8(env, javaString);
  env->DeleteLocalRef(javaString);
  return str;
}

static jclass getComponentType(JNIEnv* env, jclass arrayType) {
  auto classType = env->GetObjectClass(arrayType);
  const jmethodID method = env->GetMethodID(classType, "getComponentType", "()Ljava/lang/Class;");
  env->DeleteLocalRef(classType);
  if (!method) return nullptr;
  auto componentType = static_cast<jclass>(env->CallObjectMethod(arrayType, method));
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(componentType);
    return nullptr;
  }
  return componentType;
}

/*
 * Returns `value` as a UTF-8 string for error messages. Never fails: values that cannot be
 * converted (such as symbols) get a placeholder.
 */
std::string describeValue(JSContext* jsContext, JSValueConst value) {
  const char* str = JS_ToCString(jsContext, value);
  if (!str) {
    JS_FreeValue(jsContext, JS_GetException(jsContext));
    return "<unprintable>";
  }
  std::string result(str);
  JS_FreeCString(jsContext, str);
  return result;
}

namespace {

const char* const JAVA_EXCEPTION_CAUSE = "__quickjs_java_exception_cause__";

struct JavaExceptionCause {
  const Context* context;
  jthrowable throwable;
};

void javaExceptionFinalize(JSRuntime* runtime, JSValue value) {
  const auto context = static_cast<const Context*>(JS_GetRuntimeOpaque(runtime));
  const auto cause = static_cast<JavaExceptionCause*>(
      JS_GetOpaque(value, context->javaExceptionClassId));
  if (cause) {
    cause->context->getEnv()->DeleteGlobalRef(cause->throwable);
    delete cause;
  }
}

void jsFinalize(JSRuntime* jsRuntime, JSValue val) {
  auto context = reinterpret_cast<const Context*>(JS_GetRuntimeOpaque(jsRuntime));
  if (context) {
    delete reinterpret_cast<JavaObjectProxy*>(JS_GetOpaque(val, context->jsClassId));
  }
}

/*
 * Converts a number that is exactly representable as an int, including -0, without the wrapping
 * and truncation of ToInt32. Returns false for anything else.
 */
bool toExactInt32(JSValueConst value, int32_t* out) {
  if (JS_VALUE_GET_TAG(value) == JS_TAG_INT) {
    *out = JS_VALUE_GET_INT(value);
    return true;
  }
  if (!JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(value))) return false;
  const double d = JS_VALUE_GET_FLOAT64(value);
  // NaN fails both comparisons.
  if (!(d >= std::numeric_limits<int32_t>::min() && d <= std::numeric_limits<int32_t>::max())) {
    return false;
  }
  const auto i = static_cast<int32_t>(d);
  if (static_cast<double>(i) != d) return false;
  *out = i;
  return true;
}

struct JniThreadDetacher {
  JavaVM& javaVm;

  JniThreadDetacher(JavaVM* javaVm) : javaVm(*javaVm) {
  }

  ~JniThreadDetacher() {
    javaVm.DetachCurrentThread();
  }
};

} // anonymous namespace

Context::Context(JNIEnv* env)
    : javaVm(nullptr), jniVersion(env->GetVersion()), jsRuntime(JS_NewRuntime()),
      jsContext(jsRuntime ? JS_NewContext(jsRuntime) : nullptr), jsClassId(0),
      javaExceptionClassId(0), booleanClass(nullptr), integerClass(nullptr), doubleClass(nullptr),
      objectClass(nullptr), stringClass(nullptr), stringUtf8(nullptr),
      quickJsExceptionClass(nullptr), booleanValueOf(nullptr), booleanGetValue(nullptr),
      integerValueOf(nullptr), integerGetValue(nullptr), doubleValueOf(nullptr),
      doubleGetValue(nullptr), stringGetBytes(nullptr), stringConstructor(nullptr),
      quickJsExceptionConstructor(nullptr) {
  env->GetJavaVM(&javaVm);
  if (jsRuntime) {
    JS_SetRuntimeOpaque(jsRuntime, this);
    // QuickJS defaults to 1 MiB, which is as large as the whole stack of many threads (Android
    // gives non-main threads about 1 MiB). Leave room for the JVM and the bridge.
    JS_SetMaxStackSize(jsRuntime, DEFAULT_MAX_STACK_SIZE);
  }
}

namespace {

jclass findGlobalClass(JNIEnv* env, const char* name) {
  jclass localClass = env->FindClass(name);
  if (!localClass) return nullptr;
  auto globalClass = static_cast<jclass>(env->NewGlobalRef(localClass));
  env->DeleteLocalRef(localClass);
  return globalClass;
}

} // anonymous namespace

bool Context::initialize(JNIEnv* env) {
  if (!javaVm || !jsRuntime || !jsContext) return false;

  booleanClass = findGlobalClass(env, "java/lang/Boolean");
  if (!booleanClass) return false;
  integerClass = findGlobalClass(env, "java/lang/Integer");
  if (!integerClass) return false;
  doubleClass = findGlobalClass(env, "java/lang/Double");
  if (!doubleClass) return false;
  objectClass = findGlobalClass(env, "java/lang/Object");
  if (!objectClass) return false;
  stringClass = findGlobalClass(env, "java/lang/String");
  if (!stringClass) return false;
  quickJsExceptionClass = findGlobalClass(env, "app/cash/quickjs/QuickJsException");
  if (!quickJsExceptionClass) return false;

  jstring utf8 = env->NewStringUTF("UTF-8");
  if (!utf8) return false;
  stringUtf8 = static_cast<jstring>(env->NewGlobalRef(utf8));
  env->DeleteLocalRef(utf8);
  if (!stringUtf8) return false;

  booleanValueOf = env->GetStaticMethodID(booleanClass, "valueOf", "(Z)Ljava/lang/Boolean;");
  if (!booleanValueOf) return false;
  booleanGetValue = env->GetMethodID(booleanClass, "booleanValue", "()Z");
  if (!booleanGetValue) return false;
  integerValueOf = env->GetStaticMethodID(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
  if (!integerValueOf) return false;
  integerGetValue = env->GetMethodID(integerClass, "intValue", "()I");
  if (!integerGetValue) return false;
  doubleValueOf = env->GetStaticMethodID(doubleClass, "valueOf", "(D)Ljava/lang/Double;");
  if (!doubleValueOf) return false;
  doubleGetValue = env->GetMethodID(doubleClass, "doubleValue", "()D");
  if (!doubleGetValue) return false;
  stringGetBytes = env->GetMethodID(stringClass, "getBytes", "(Ljava/lang/String;)[B");
  if (!stringGetBytes) return false;
  stringConstructor = env->GetMethodID(stringClass, "<init>", "([BLjava/lang/String;)V");
  if (!stringConstructor) return false;
  quickJsExceptionConstructor = env->GetMethodID(quickJsExceptionClass, "<init>",
                                                 "(Ljava/lang/String;Ljava/lang/String;)V");
  return quickJsExceptionConstructor != nullptr;
}

Context::~Context() {
  for (auto proxy : objectProxies) {
    delete proxy;
  }
  // Free JavaScript objects first: their finalizers may release Java references.
  if (jsContext) JS_FreeContext(jsContext);
  if (jsRuntime) JS_FreeRuntime(jsRuntime);
  auto env = javaVm ? getEnv() : nullptr;
  if (!env) return;
  for (auto refs : globalReferences) {
    env->DeleteGlobalRef(refs.second);
  }
  for (auto& entry : objectConverters) {
    env->DeleteGlobalRef(entry.first);
  }
  env->DeleteGlobalRef(interruptHandler);
  env->DeleteGlobalRef(interruptCause);
  env->DeleteGlobalRef(quickJsExceptionClass);
  env->DeleteGlobalRef(stringUtf8);
  env->DeleteGlobalRef(stringClass);
  env->DeleteGlobalRef(objectClass);
  env->DeleteGlobalRef(doubleClass);
  env->DeleteGlobalRef(integerClass);
  env->DeleteGlobalRef(booleanClass);
}

jobject Context::execute(JNIEnv* env, jbyteArray byteCode) {
  if (!byteCode) {
    throwJavaException(env, "java/lang/NullPointerException", "bytecode == null");
    return nullptr;
  }
  const auto buffer = env->GetByteArrayElements(byteCode, nullptr);
  if (!buffer) return nullptr;
  const auto bufferLength = env->GetArrayLength(byteCode);
  const auto flags = JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE;
  auto obj = JS_ReadObject(jsContext, reinterpret_cast<const uint8_t*>(buffer), bufferLength, flags);
  env->ReleaseByteArrayElements(byteCode, buffer, JNI_ABORT);

  if (JS_IsException(obj)) {
    throwJsException(env, obj);
    return nullptr;
  }

  if (JS_ResolveModule(jsContext, obj)) {
    JS_FreeValue(jsContext, obj);
    throwJsException(env, JS_EXCEPTION);
    return nullptr;
  }

  auto val = JS_EvalFunction(jsContext, obj);
  jobject result;
  if (!JS_IsException(val)) {
    result = toJavaObject(env, val, false);
  } else {
    result = nullptr;
    throwJsException(env, val);
  }
  JS_FreeValue(jsContext, val);

  return result;
}

jbyteArray Context::compile(JNIEnv* env, jstring source, jstring file) {
  if (!source || !file) {
    throwJavaException(env, "java/lang/NullPointerException",
                       source ? "fileName == null" : "sourceCode == null");
    return nullptr;
  }
  // Same encoding as eval(): standard UTF-8, which also keeps embedded NUL characters.
  const std::string sourceCode = toCppString(env, source);
  if (env->ExceptionCheck()) return nullptr;
  const std::string fileName = toCppString(env, file);
  if (env->ExceptionCheck()) return nullptr;

  auto compiled = JS_Eval(jsContext, sourceCode.c_str(), sourceCode.length(), fileName.c_str(),
                          JS_EVAL_FLAG_COMPILE_ONLY);

  if (JS_IsException(compiled)) {
    // TODO: figure out how to get the failing line number into the exception.
    throwJsException(env, compiled);
    return nullptr;
  }

  size_t bufferLength = 0;
  auto buffer = JS_WriteObject(jsContext, &bufferLength, compiled, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_REFERENCE);
  JS_FreeValue(jsContext, compiled);
  if (!buffer) {
    // JS_WriteObject() fails on allocation failure or an unserializable value.
    throwJsException(env, JS_EXCEPTION);
    return nullptr;
  }

  jbyteArray result = nullptr;
  if (bufferLength > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    throwJavaException(env, "java/lang/OutOfMemoryError", "Bytecode exceeds Java array size");
  } else {
    result = env->NewByteArray(bufferLength);
    if (result) {
      env->SetByteArrayRegion(result, 0, bufferLength, reinterpret_cast<const jbyte*>(buffer));
    }
  }
  js_free(jsContext, buffer);

  return result;
}

JsObjectProxy* Context::getObjectProxy(JNIEnv* env, jstring name, jobjectArray methods) {
  if (!name) {
    throwJavaException(env, "java/lang/NullPointerException", "name == null");
    return nullptr;
  }
  const std::string nameString = toCppString(env, name);
  if (env->ExceptionCheck()) return nullptr;
  const char* nameStr = nameString.c_str();
  const JSAtom nameAtom = JS_NewAtomLen(jsContext, nameString.data(), nameString.size());
  if (nameAtom == JS_ATOM_NULL) {
    throwJsException(env, JS_EXCEPTION);
    return nullptr;
  }

  JSValue global = JS_GetGlobalObject(jsContext);
  JSValue obj = JS_GetProperty(jsContext, global, nameAtom);
  JS_FreeValue(jsContext, global);

  JsObjectProxy* jsObjectProxy = nullptr;
  if (JS_IsObject(obj)) {
    // The proxy owns its own reference to the atom.
    jsObjectProxy = new JsObjectProxy(this, nameString, JS_DupAtom(jsContext, nameAtom));
    jsize numMethods = env->GetArrayLength(methods);
    for (int i = 0; i < numMethods && !env->ExceptionCheck(); ++i) {
      jobject method = env->GetObjectArrayElement(methods, i);
      if (!method) break;
      const auto methodNameString = ::getName(env, method);
      if (env->ExceptionCheck()) {
        env->DeleteLocalRef(method);
        break;
      }
      const char* methodNameStr = methodNameString.c_str();

      const JSAtom methodAtom =
          JS_NewAtomLen(jsContext, methodNameString.data(), methodNameString.size());
      JSValue prop = methodAtom == JS_ATOM_NULL
          ? JS_EXCEPTION
          : JS_GetProperty(jsContext, obj, methodAtom);
      if (JS_IsException(prop)) {
        throwJsException(env, prop);
      } else if (JS_IsFunction(jsContext, prop)) {
        jsObjectProxy->methods.push_back(
            new JsMethodProxy(this, env, methodNameString, JS_DupAtom(jsContext, methodAtom), method));
      } else {
        const char* msg = JS_IsUndefined(prop)
                          ? "JavaScript global %s has no method called %s"
                          : "JavaScript property %s.%s not callable";
        throwJsExceptionFmt(env, this, msg, nameStr, methodNameStr);
      }
      JS_FreeValue(jsContext, prop);
      if (methodAtom != JS_ATOM_NULL) JS_FreeAtom(jsContext, methodAtom);
      env->DeleteLocalRef(method);
    }
    if (env->ExceptionCheck()) {
      delete jsObjectProxy;
      jsObjectProxy = nullptr;
    } else {
      // Proxies look up the global by name on each call, so an equivalent proxy can be shared.
      for (auto existing : objectProxies) {
        if (existing->hasSameMethods(*jsObjectProxy)) {
          delete jsObjectProxy;
          jsObjectProxy = existing;
          break;
        }
      }
      if (std::find(objectProxies.begin(), objectProxies.end(), jsObjectProxy)
          == objectProxies.end()) {
        objectProxies.push_back(jsObjectProxy);
      }
    }
  } else if (JS_IsException(obj)) {
    throwJsException(env, obj);
  } else {
    const char* msg = JS_IsUndefined(obj)
                      ? "A global JavaScript object called %s was not found"
                      : "JavaScript global called %s is not an object";
    throwJavaException(env, "java/lang/IllegalArgumentException", msg, nameStr);
  }

  JS_FreeValue(jsContext, obj);
  JS_FreeAtom(jsContext, nameAtom);

  return jsObjectProxy;
}

void Context::setObjectProxy(JNIEnv* env, jstring name, jobject object, jobjectArray methods) {
  if (!name || !object) {
    throwJavaException(env, "java/lang/NullPointerException",
                       name ? "object == null" : "name == null");
    return;
  }
  const std::string nameString = toCppString(env, name);
  if (env->ExceptionCheck()) return;
  const char* nameStr = nameString.c_str();

  const auto objName = JS_NewAtomLen(jsContext, nameString.data(), nameString.size());
  if (objName == JS_ATOM_NULL) {
    throwJsException(env, JS_EXCEPTION);
    return;
  }
  auto global = JS_GetGlobalObject(jsContext);

  const int hasProperty = JS_HasProperty(jsContext, global, objName);
  if (hasProperty < 0) {
    throwJsException(env, JS_EXCEPTION);
  } else if (hasProperty) {
    throwJavaException(env, "java/lang/IllegalArgumentException",
                       "A global object called %s already exists", nameStr);
  } else {
    if (jsClassId == 0) {
      JS_NewClassID(&jsClassId);
      JSClassDef classDef;
      memset(&classDef, 0, sizeof(JSClassDef));
      classDef.class_name = "QuickJsAndroidProxy";
      classDef.finalizer = jsFinalize;
      if (JS_NewClass(jsRuntime, jsClassId, &classDef)) {
        jsClassId = 0;
        throwJavaException(env, "java/lang/OutOfMemoryError",
                           "Failed to allocate JavaScript proxy class");
      }
    }
    if (jsClassId != 0) {
      // Fully build the proxy before publishing it, so a failure leaves no global behind.
      auto proxy = JS_NewObjectClass(jsContext, jsClassId);
      if (JS_IsException(proxy)) {
        throwJsException(env, proxy);
      } else {
        std::unique_ptr<JavaObjectProxy> javaObject(new JavaObjectProxy(this, env, nameStr, object, methods, proxy));
        if (env->ExceptionCheck()) {
          JS_FreeValue(jsContext, proxy);
        } else {
          JS_SetOpaque(proxy, javaObject.release());
          // Takes ownership of proxy.
          if (JS_DefinePropertyValue(jsContext, global, objName, proxy,
                                     JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
            throwJsException(env, JS_EXCEPTION);
          }
        }
      }
    }
  }
  JS_FreeAtom(jsContext, objName);
  JS_FreeValue(jsContext, global);
}

namespace {

int pollInterruptHandler(JSRuntime*, void* opaque) {
  auto context = static_cast<Context*>(opaque);
  if (!context->interruptHandler || context->interruptCause) return 0;
  JNIEnv* env = context->getEnv();
  if (env->ExceptionCheck()) return 0; // Never call into Java with an exception pending.
  const jboolean interrupt =
      env->CallBooleanMethod(context->interruptHandler, context->interruptHandlerPoll);
  if (env->ExceptionCheck()) {
    // JavaScript can't carry this exception; throwJsException() rethrows it instead.
    jthrowable exception = env->ExceptionOccurred();
    env->ExceptionClear();
    context->interruptCause = static_cast<jthrowable>(env->NewGlobalRef(exception));
    env->DeleteLocalRef(exception);
    return 1;
  }
  return interrupt ? 1 : 0;
}

} // anonymous namespace

void Context::setInterruptHandler(JNIEnv* env, jobject handler) {
  jobject globalHandler = nullptr;
  jmethodID poll = nullptr;
  if (handler) {
    jclass handlerClass = env->GetObjectClass(handler);
    poll = env->GetMethodID(handlerClass, "poll", "()Z");
    env->DeleteLocalRef(handlerClass);
    if (!poll) return;
    globalHandler = env->NewGlobalRef(handler);
    if (!globalHandler) return;
  }
  env->DeleteGlobalRef(interruptHandler);
  interruptHandler = globalHandler;
  interruptHandlerPoll = poll;
  JS_SetInterruptHandler(jsRuntime, globalHandler ? pollInterruptHandler : nullptr, this);
}

jint Context::executePendingJobs(JNIEnv* env, jint maxJobs) {
  jint executed = 0;
  while (executed < maxJobs) {
    JSContext* jobContext = nullptr;
    const int status = JS_ExecutePendingJob(jsRuntime, &jobContext);
    if (status == 0) break;
    executed++;
    if (status < 0) {
      throwJsException(env, JS_EXCEPTION);
      break;
    }
  }
  return executed;
}

bool Context::getArrayLength(JNIEnv* env, const JSValueConst& value, jsize* length) const {
  auto jsLength = JS_GetPropertyStr(jsContext, value, "length");
  uint64_t index = 0;
  const int failed = JS_IsException(jsLength) || JS_ToIndex(jsContext, &index, jsLength);
  JS_FreeValue(jsContext, jsLength);
  if (failed) {
    throwJsException(env, JS_EXCEPTION);
    return false;
  }
  if (index > MAX_ARRAY_LENGTH) {
    throwJavaException(env, "java/lang/IllegalArgumentException",
                       "Cannot marshal a JavaScript array of length %llu to Java (limit %d)",
                       static_cast<unsigned long long>(index), static_cast<int>(MAX_ARRAY_LENGTH));
    return false;
  }
  *length = static_cast<jsize>(index);
  return true;
}

jobject
Context::toJavaObject(JNIEnv* env, const JSValueConst& value, bool throwOnUnsupportedType) {
  jobject result;
  switch (JS_VALUE_GET_NORM_TAG(value)) {
    case JS_TAG_EXCEPTION: {
      throwJsException(env, value);
      result = nullptr;
      break;
    }

    case JS_TAG_STRING: {
      result = toJavaString(env, value);
      break;
    }

    case JS_TAG_BOOL: {
      jvalue v;
      v.z = static_cast<jboolean>(JS_VALUE_GET_BOOL(value));
      result = env->CallStaticObjectMethodA(booleanClass, booleanValueOf, &v);
      break;
    }

    case JS_TAG_INT: {
      jvalue v;
      v.i = static_cast<jint>(JS_VALUE_GET_INT(value));
      result = env->CallStaticObjectMethodA(integerClass, integerValueOf, &v);
      break;
    }

    case JS_TAG_FLOAT64: {
      jvalue v;
      v.d = static_cast<jdouble>(JS_VALUE_GET_FLOAT64(value));
      result = env->CallStaticObjectMethodA(doubleClass, doubleValueOf, &v);
      break;
    }

    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
      result = nullptr;
      break;

    case JS_TAG_OBJECT:
      if (JS_IsArray(jsContext, value)) {
        // Only arrays on the current conversion path count: the same array may appear twice.
        const auto array = JS_VALUE_GET_PTR(value);
        if (std::find(arraysBeingConverted.begin(), arraysBeingConverted.end(), array)
            != arraysBeingConverted.end()) {
          throwJavaException(env, "java/lang/IllegalArgumentException",
                             "Cannot marshal a cyclic JavaScript array to Java");
          result = nullptr;
          break;
        }
        if (arraysBeingConverted.size() >= MAX_ARRAY_NESTING_DEPTH) {
          throwJavaException(env, "java/lang/IllegalArgumentException",
                             "Cannot marshal JavaScript arrays nested deeper than %d to Java",
                             static_cast<int>(MAX_ARRAY_NESTING_DEPTH));
          result = nullptr;
          break;
        }
        jsize arrayLength = 0;
        if (!getArrayLength(env, value, &arrayLength)) {
          result = nullptr;
          break;
        }
        // Bound the total work too: shared sub-arrays are converted once per reference.
        if (arraysBeingConverted.empty()) arrayElementsConverted = 0;
        arrayElementsConverted += arrayLength;
        if (arrayElementsConverted > MAX_ARRAY_LENGTH) {
          throwJavaException(env, "java/lang/IllegalArgumentException",
                             "Cannot marshal more than %d JavaScript array elements to Java",
                             static_cast<int>(MAX_ARRAY_LENGTH));
          result = nullptr;
          break;
        }
        arraysBeingConverted.push_back(array);
        result = env->NewObjectArray(arrayLength, objectClass, nullptr);
        for (jsize i = 0; result && i < arrayLength && !env->ExceptionCheck(); i++) {
          auto element = JS_GetPropertyUint32(jsContext, value, i);
          auto javaElement = toJavaObject(env, element, throwOnUnsupportedType);
          if (!env->ExceptionCheck()) {
            env->SetObjectArrayElement(static_cast<jobjectArray>(result), i, javaElement);
          }
          env->DeleteLocalRef(javaElement);
          JS_FreeValue(jsContext, element);
        }
        arraysBeingConverted.pop_back();
        if (env->ExceptionCheck()) {
          env->DeleteLocalRef(result);
          result = nullptr;
        }
        break;
      }
      // Fall through.
    default:
      if (throwOnUnsupportedType) {
        throwJsExceptionFmt(env, this, "Cannot marshal value %s to Java",
                            describeValue(jsContext, value).c_str());
      }
      result = nullptr;
      break;
  }
  return result;
}

jobject Context::eval(JNIEnv* env, jstring source, jstring file) {
  if (!source || !file) {
    throwJavaException(env, "java/lang/NullPointerException",
                       source ? "fileName == null" : "sourceCode == null");
    return nullptr;
  }
  std::string sourceCodeString = toCppString(env, source);
  if (env->ExceptionCheck()) return nullptr;
  std::string fileName = toCppString(env, file);
  if (env->ExceptionCheck()) return nullptr;

  JSValue evalValue = JS_Eval(jsContext, sourceCodeString.c_str(), sourceCodeString.length(),
                              fileName.c_str(), 0);

  jobject result = toJavaObject(env, evalValue, false);

  JS_FreeValue(jsContext, evalValue);

  return result;
}

Context::JavaToJavaScript
Context::getJavaToJsConverter(JNIEnv* env, jclass type, bool boxed) {
  if (!type || env->ExceptionCheck()) {
    return [](Context*, JNIEnv*, jvalue) { return JS_UNDEFINED; }; // Never called.
  }
  const auto typeName = getName(env, type);
  if (env->ExceptionCheck()) {
    return [](Context*, JNIEnv*, jvalue) { return JS_UNDEFINED; }; // Never called.
  }
  if (!typeName.empty() && typeName[0] == '[') {
    // type is an array.
    auto elementType = getComponentType(env, type);
    if (!elementType) {
      return [](Context*, JNIEnv*, jvalue) { return JS_UNDEFINED; }; // Never called.
    }
    const auto elementTypeName = getName(env, elementType);
    if (elementTypeName == "double") {
      env->DeleteLocalRef(elementType);
      return [](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        JSValue result = JS_NewArray(c->jsContext);
        auto elements = env->GetDoubleArrayElements(static_cast<jdoubleArray>(v.l), nullptr);
        if (!elements) {
          JS_FreeValue(c->jsContext, result);
          return c->throwJavaExceptionFromJs(env);
        }
        const auto length = env->GetArrayLength(static_cast<jarray>(v.l));
        for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
          JS_SetPropertyUint32(c->jsContext, result, i, JS_NewFloat64(c->jsContext, elements[i]));
        }
        env->ReleaseDoubleArrayElements(static_cast<jdoubleArray>(v.l), elements, JNI_ABORT);
        if (env->ExceptionCheck()) {
          JS_FreeValue(c->jsContext, result);
          return c->throwJavaExceptionFromJs(env);
        }
        return result;
      };
    } else if (elementTypeName == "int") {
      env->DeleteLocalRef(elementType);
      return [](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        JSValue result = JS_NewArray(c->jsContext);
        auto elements = env->GetIntArrayElements(static_cast<jintArray >(v.l), nullptr);
        if (!elements) {
          JS_FreeValue(c->jsContext, result);
          return c->throwJavaExceptionFromJs(env);
        }
        const auto length = env->GetArrayLength(static_cast<jarray>(v.l));
        for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
          JS_SetPropertyUint32(c->jsContext, result, i, JS_NewInt32(c->jsContext, elements[i]));
        }
        env->ReleaseIntArrayElements(static_cast<jintArray>(v.l), elements, JNI_ABORT);
        if (env->ExceptionCheck()) {
          JS_FreeValue(c->jsContext, result);
          return c->throwJavaExceptionFromJs(env);
        }
        return result;
      };
    } else if (elementTypeName == "boolean") {
      env->DeleteLocalRef(elementType);
      return [](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        JSValue result = JS_NewArray(c->jsContext);
        auto elements = env->GetBooleanArrayElements(static_cast<jbooleanArray>(v.l), nullptr);
        if (!elements) {
          JS_FreeValue(c->jsContext, result);
          return c->throwJavaExceptionFromJs(env);
        }
        const auto length = env->GetArrayLength(static_cast<jarray>(v.l));
        for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
          JS_SetPropertyUint32(c->jsContext, result, i, JS_NewBool(c->jsContext, elements[i]));
        }
        env->ReleaseBooleanArrayElements(static_cast<jbooleanArray>(v.l), elements, JNI_ABORT);
        if (env->ExceptionCheck()) {
          JS_FreeValue(c->jsContext, result);
          return c->throwJavaExceptionFromJs(env);
        }
        return result;
      };
    } else {
      auto converter = getJavaToJsConverter(env, elementType, true);
      env->DeleteLocalRef(elementType);
      return [converter](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        JSValue result = JS_NewArray(c->jsContext);
        jvalue element;
        const auto length = env->GetArrayLength(static_cast<jarray>(v.l));
        for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
          element.l = env->GetObjectArrayElement(static_cast<jobjectArray >(v.l), i);
          if (!env->ExceptionCheck()) {
            auto jsElement = converter(c, env, element);
            if (JS_IsException(jsElement)) {
              // A nested conversion failed; its exception is already pending in JavaScript.
              env->DeleteLocalRef(element.l);
              JS_FreeValue(c->jsContext, result);
              return JS_EXCEPTION;
            }
            JS_SetPropertyUint32(c->jsContext, result, i, jsElement);
          }
          env->DeleteLocalRef(element.l);
        }
        if (env->ExceptionCheck()) {
          JS_FreeValue(c->jsContext, result);
          return c->throwJavaExceptionFromJs(env);
        }
        return result;
      };
    }
  }

  if (typeName == "java.lang.String") {
    return [](Context* c, JNIEnv* env, jvalue v) {
      if (!v.l) return JS_NULL;
      std::string string = c->toCppString(env, static_cast<jstring>(v.l));
      if (env->ExceptionCheck()) return JS_NULL;
      auto jsString = JS_NewStringLen(c->jsContext, string.data(), string.size());
      return jsString;
    };
  } else if (typeName == "java.lang.Double" || (typeName == "double" && boxed)) {
    return [](Context* c, JNIEnv* env, jvalue v) {
      return v.l != nullptr
             ? JS_NewFloat64(c->jsContext, env->CallDoubleMethod(v.l, c->doubleGetValue))
             : JS_NULL;
    };
  } else if (typeName == "java.lang.Integer" || (typeName == "int" && boxed)) {
    return [](Context* c, JNIEnv* env, jvalue v) {
      return v.l != nullptr
             ? JS_NewInt32(c->jsContext, env->CallIntMethod(v.l, c->integerGetValue))
             : JS_NULL;
    };
  } else if (typeName == "java.lang.Boolean" || (typeName == "boolean" && boxed)) {
    return [](Context* c, JNIEnv* env, jvalue v) {
      return v.l != nullptr
             ? JS_NewBool(c->jsContext, env->CallBooleanMethod(v.l, c->booleanGetValue))
             : JS_NULL;
    };
  } else if (typeName == "double") {
    return [](Context* c, JNIEnv* env, jvalue v) {
      return JS_NewFloat64(c->jsContext, v.d);
    };
  } else if (typeName == "int") {
    return [](Context* c, JNIEnv* env, jvalue v) {
      return JS_NewInt32(c->jsContext, v.i);
    };
  } else if (typeName == "boolean") {
    return [](Context* c, JNIEnv* env, jvalue v) {
      return JS_NewBool(c->jsContext, v.z);
    };
  } else if (typeName == "java.lang.Object") {
    return [](Context* c, JNIEnv* env, jvalue v) {
      if (!v.l) return JS_NULL;
      const auto actualClass = env->GetObjectClass(v.l);
      if (!actualClass) return JS_NULL;
      if (env->IsSameObject(actualClass, c->objectClass)) {
        env->DeleteLocalRef(actualClass);
        throwJavaException(env, "java/lang/IllegalArgumentException",
                           "Unsupported Java type java.lang.Object");
        return JS_NULL;
      }
      // Building a converter takes several reflective calls, so reuse one per runtime class.
      for (const auto& entry : c->objectConverters) {
        if (env->IsSameObject(actualClass, entry.first)) {
          env->DeleteLocalRef(actualClass);
          return entry.second(c, env, v);
        }
      }
      const auto converter = c->getJavaToJsConverter(env, actualClass, true);
      if (!env->ExceptionCheck()) {
        auto globalClass = static_cast<jclass>(env->NewGlobalRef(actualClass));
        if (globalClass) c->objectConverters.emplace_back(globalClass, converter);
      }
      env->DeleteLocalRef(actualClass);
      if (env->ExceptionCheck()) return JS_NULL;
      return converter(c, env, v);
    };
  } else if (typeName == "void") {
    return [](Context*, JNIEnv* env, jvalue) {
      return JS_UNDEFINED;
    };
  } else {
    // Throw an exception for unsupported argument type.
    throwJavaException(env, "java/lang/IllegalArgumentException", "Unsupported Java type %s",
                       typeName.c_str());
    return [typeName](Context* context, JNIEnv* env, jvalue) {
      return JS_ThrowTypeError(context->jsContext, "Unsupported Java type %s", typeName.c_str());
    };
  }
}

Context::JavaScriptToJava
Context::getJsToJavaConverter(JNIEnv* env, jclass type, bool boxed) {
  if (!type || env->ExceptionCheck()) {
    return [](Context*, JNIEnv*, const JSValueConst&) { return jvalue(); }; // Never called.
  }
  const auto typeName = getName(env, type);
  if (env->ExceptionCheck()) {
    return [](Context*, JNIEnv*, const JSValueConst&) { return jvalue(); }; // Never called.
  }
  if (!typeName.empty() && typeName[0] == '[') {
    // type is an array.
    auto elementType = getComponentType(env, type);
    if (!elementType) {
      return [](Context*, JNIEnv*, const JSValueConst&) { return jvalue(); }; // Never called.
    }
    const auto elementTypeName = getName(env, elementType);
    if (elementTypeName == "double") {
      env->DeleteLocalRef(elementType);
      return [](Context* c, JNIEnv* env, const JSValueConst& v) {
        jvalue result;
        if (JS_IsNull(v) || JS_IsUndefined(v)) {
          result.l = nullptr;
        } else if (JS_IsException(v)) {
          result.l = nullptr;
          c->throwJsException(env, v);
        } else {
          jsize length = 0;
          if (!c->getArrayLength(env, v, &length)) {
            result.l = nullptr;
          } else {
            result.l = env->NewDoubleArray(length);
            for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
              double element;
              auto jsElement = JS_GetPropertyUint32(c->jsContext, v, i);
              if (!JS_IsNumber(jsElement)) {
                throwJavaException(env, "java/lang/IllegalArgumentException",
                                   "Cannot convert value %s to double",
                                   describeValue(c->jsContext, jsElement).c_str());
              } else if (JS_ToFloat64(c->jsContext, &element, jsElement)) {
                c->throwJsException(env, jsElement);
              } else {
                env->SetDoubleArrayRegion(static_cast<jdoubleArray>(result.l), i, 1, &element);
              }
              JS_FreeValue(c->jsContext, jsElement);
            }
          }
        }
        return result;
      };
    } else if (elementTypeName == "int") {
      env->DeleteLocalRef(elementType);
      return [](Context* c, JNIEnv* env, const JSValueConst& v) {
        jvalue result;
        if (JS_IsNull(v) || JS_IsUndefined(v)) {
          result.l = nullptr;
        } else if (JS_IsException(v)) {
          result.l = nullptr;
          c->throwJsException(env, v);
        } else {
          jsize length = 0;
          if (!c->getArrayLength(env, v, &length)) {
            result.l = nullptr;
          } else {
            result.l = env->NewIntArray(length);
            for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
              int element;
              auto jsElement = JS_GetPropertyUint32(c->jsContext, v, i);
              if (!toExactInt32(jsElement, &element)) {
                throwJavaException(env, "java/lang/IllegalArgumentException",
                                   "Cannot convert value %s to int",
                                   describeValue(c->jsContext, jsElement).c_str());
              } else {
                env->SetIntArrayRegion(static_cast<jintArray>(result.l), i, 1, &element);
              }
              JS_FreeValue(c->jsContext, jsElement);
            }
          }
        }
        return result;
      };
    } else if (elementTypeName == "boolean") {
      env->DeleteLocalRef(elementType);
      return [](Context* c, JNIEnv* env, const JSValueConst& v) {
        jvalue result;
        if (JS_IsNull(v) || JS_IsUndefined(v)) {
          result.l = nullptr;
        } else if (JS_IsException(v)) {
          result.l = nullptr;
          c->throwJsException(env, v);
        } else {
          jsize length = 0;
          if (!c->getArrayLength(env, v, &length)) {
            result.l = nullptr;
          } else {
            result.l = env->NewBooleanArray(length);
            for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
              auto jsElement = JS_GetPropertyUint32(c->jsContext, v, i);
              if (!JS_IsBool(jsElement)) {
                throwJavaException(env, "java/lang/IllegalArgumentException",
                                   "Cannot convert value %s to boolean",
                                   describeValue(c->jsContext, jsElement).c_str());
              } else {
                int r = JS_ToBool(c->jsContext, jsElement);
                if (r < 0) {
                  c->throwJsException(env, jsElement);
                } else {
                  jboolean element = r != 0;
                  env->SetBooleanArrayRegion(static_cast<jbooleanArray>(result.l), i, 1, &element);
                }
              }
              JS_FreeValue(c->jsContext, jsElement);
            }
          }
        }
        return result;
      };
    } else {
      auto converter = getJsToJavaConverter(env, elementType, true);
      auto elementTypeGlobalRef = getGlobalRef(env, elementType);
      env->DeleteLocalRef(elementType);
      return [converter, elementTypeGlobalRef](Context* c, JNIEnv* env,
                                               const JSValueConst& v) {
        jvalue result;
        if (JS_IsNull(v) || JS_IsUndefined(v)) {
          result.l = nullptr;
        } else if (JS_IsException(v)) {
          result.l = nullptr;
          c->throwJsException(env, v);
        } else {
          jsize length = 0;
          if (!c->getArrayLength(env, v, &length)) {
            result.l = nullptr;
          } else {
            result.l = env->NewObjectArray(length, elementTypeGlobalRef, nullptr);
            for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
              auto jsElement = JS_GetPropertyUint32(c->jsContext, v, i);
              jvalue element = converter(c, env, jsElement);
              JS_FreeValue(c->jsContext, jsElement);
              if (env->ExceptionCheck()) break;
              env->SetObjectArrayElement(static_cast<jobjectArray>(result.l), i, element.l);
              env->DeleteLocalRef(element.l);
            }
          }
        }
        return result;
      };
    }
  }

  if (typeName == "java.lang.String") {
    return [](Context* c, JNIEnv* env, const JSValueConst& v) {
      jvalue result;
      if (JS_IsNull(v) || JS_IsUndefined(v)) {
        result.l = nullptr;
      } else if (JS_IsException(v)) {
        result.l = nullptr;
        c->throwJsException(env, v);
      } else if (!JS_IsString(v)) {
        result.l = nullptr;
        throwJavaException(env, "java/lang/IllegalArgumentException",
                           "Cannot convert value %s to String",
                           describeValue(c->jsContext, v).c_str());
      } else {
        result.l = c->toJavaString(env, v);
      }
      return result;
    };
  } else if (typeName == "java.lang.Double" || typeName == "double") {
    const bool nullable = typeName == "java.lang.Double" || boxed;
    return [nullable](Context* c, JNIEnv* env, const JSValueConst& v) {
      jvalue result;
      result.l = nullptr;
      if (JS_IsException(v)) {
        c->throwJsException(env, v);
      } else if (nullable && (JS_IsNull(v) || JS_IsUndefined(v))) {
        // Leave as null.
      } else if (!JS_IsNumber(v)) {
        throwJavaException(env, "java/lang/IllegalArgumentException",
                           "Cannot convert value %s to double",
                           describeValue(c->jsContext, v).c_str());
      } else if (JS_ToFloat64(c->jsContext, &result.d, v)) {
        c->throwJsException(env, JS_EXCEPTION);
        result.l = nullptr;
      } else if (nullable) {
        result.l = env->CallStaticObjectMethodA(c->doubleClass, c->doubleValueOf, &result);
      }
      return result;
    };
  } else if (typeName == "java.lang.Integer" || typeName == "int") {
    const bool nullable = typeName == "java.lang.Integer" || boxed;
    return [nullable](Context* c, JNIEnv* env, const JSValueConst& v) {
      jvalue result;
      result.l = nullptr;
      if (JS_IsException(v)) {
        c->throwJsException(env, v);
      } else if (nullable && (JS_IsNull(v) || JS_IsUndefined(v))) {
        // Leave as null.
      } else if (!toExactInt32(v, &result.i)) {
        result.l = nullptr;
        throwJavaException(env, "java/lang/IllegalArgumentException",
                           "Cannot convert value %s to int",
                           describeValue(c->jsContext, v).c_str());
      } else if (nullable) {
        result.l = env->CallStaticObjectMethodA(c->integerClass, c->integerValueOf, &result);
      }
      return result;
    };
  } else if (typeName == "java.lang.Boolean" || typeName == "boolean") {
    const bool nullable = typeName == "java.lang.Boolean" || boxed;
    return [nullable](Context* c, JNIEnv* env, const JSValueConst& v) {
      jvalue result;
      result.l = nullptr;
      if (JS_IsException(v)) {
        c->throwJsException(env, v);
      } else if (nullable && (JS_IsNull(v) || JS_IsUndefined(v))) {
        // Leave as null.
      } else if (!JS_IsBool(v)) {
        throwJavaException(env, "java/lang/IllegalArgumentException",
                           "Cannot convert value %s to boolean",
                           describeValue(c->jsContext, v).c_str());
      } else {
        result.z = static_cast<jboolean>(JS_VALUE_GET_BOOL(v) != 0);
        if (nullable) {
          result.l = env->CallStaticObjectMethodA(c->booleanClass, c->booleanValueOf, &result);
        }
      }
      return result;
    };
  } else if (typeName == "java.lang.Object") {
    return [](Context* c, JNIEnv* env, const JSValueConst& v) {
      jvalue result;
      result.l = c->toJavaObject(env, v);
      return result;
    };
  } else if (typeName == "void") {
    return [](Context* c, JNIEnv* env, const JSValueConst& v) {
      jvalue result;
      result.l = nullptr;
      if (JS_IsException(v)) {
        c->throwJsException(env, v);
      }
      return result;
    };
  } else {
    // Throw an exception for unsupported argument type.
    throwJavaException(env, "java/lang/IllegalArgumentException", "Unsupported Java type %s",
                       typeName.c_str());
    return [typeName](Context* context, JNIEnv* env, const JSValueConst&) {
      throwJavaException(env, "java/lang/IllegalArgumentException",
                         "Unsupported Java type %s", typeName.c_str());
      jvalue result;
      result.l = nullptr;
      return result;
    };
  }
}

void Context::throwJsException(JNIEnv* env, const JSValue& value) const {
  if (env->ExceptionCheck()) return;
  if (interruptCause) {
    // The JavaScript "interrupted" error stands in for this exception.
    JS_FreeValue(jsContext, JS_GetException(jsContext));
    env->Throw(interruptCause);
    env->DeleteGlobalRef(interruptCause);
    interruptCause = nullptr;
    return;
  }
  JSValue exceptionValue = JS_GetException(jsContext);
  JSValue causeHolder = JS_UNDEFINED;
  if (javaExceptionClassId && JS_IsObject(exceptionValue)) {
    causeHolder = JS_GetPropertyStr(jsContext, exceptionValue, JAVA_EXCEPTION_CAUSE);
    if (JS_IsException(causeHolder)) {
      JS_FreeValue(jsContext, JS_GetException(jsContext));
    }
  }
  const auto cause = javaExceptionClassId
      ? static_cast<JavaExceptionCause*>(JS_GetOpaque(causeHolder, javaExceptionClassId))
      : nullptr;

  JSValue messageValue = JS_UNDEFINED;
  JSValue stackValue = JS_UNDEFINED;
  if (JS_IsObject(exceptionValue)) {
    messageValue = JS_GetPropertyStr(jsContext, exceptionValue, "message");
    if (JS_IsException(messageValue)) {
      JS_FreeValue(jsContext, JS_GetException(jsContext));
      messageValue = JS_UNDEFINED;
    }
    stackValue = JS_GetPropertyStr(jsContext, exceptionValue, "stack");
    if (JS_IsException(stackValue)) {
      JS_FreeValue(jsContext, JS_GetException(jsContext));
      stackValue = JS_UNDEFINED;
    }
  }
  const auto releaseValues = [&] {
    JS_FreeValue(jsContext, messageValue);
    JS_FreeValue(jsContext, stackValue);
    JS_FreeValue(jsContext, causeHolder);
    JS_FreeValue(jsContext, exceptionValue);
  };

  // If the JS does a `throw 2;`, there won't be a message property.
  jstring message = toJavaString(env,
                                 JS_IsUndefined(messageValue) ? exceptionValue : messageValue);
  if (env->ExceptionCheck()) {
    releaseValues();
    return;
  }
  jstring stack = JS_IsUndefined(stackValue) ? nullptr : toJavaString(env, stackValue);
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(message);
    releaseValues();
    return;
  }

  jobject exception;
  if (cause) {
    exception = env->NewLocalRef(cause->throwable);

    // add the JavaScript stack to this exception.
    if (exception && stack && !env->ExceptionCheck()) {
      const jmethodID addJavaScriptStack =
        env->GetStaticMethodID(quickJsExceptionClass,
                               "addJavaScriptStack",
                               "(Ljava/lang/Throwable;Ljava/lang/String;)V");
      if (addJavaScriptStack && !env->ExceptionCheck()) {
        env->CallStaticVoidMethod(quickJsExceptionClass, addJavaScriptStack, exception, stack);
      }
    }
  } else {
    exception = env->NewObject(quickJsExceptionClass,
                               quickJsExceptionConstructor,
                               message,
                               stack);
  }

  env->DeleteLocalRef(stack);
  env->DeleteLocalRef(message);
  releaseValues();
  if (exception && !env->ExceptionCheck()) env->Throw(static_cast<jthrowable>(exception));
  env->DeleteLocalRef(exception);
}

JSValue Context::throwJavaExceptionFromJs(JNIEnv* env) const {
  assert(env->ExceptionCheck()); // There must be something to throw.
  auto exception = env->ExceptionOccurred();
  env->ExceptionClear();
  if (!javaExceptionClassId) {
    JS_NewClassID(&javaExceptionClassId);
    JSClassDef definition = {};
    definition.class_name = "QuickJsJavaExceptionCause";
    definition.finalizer = javaExceptionFinalize;
    if (JS_NewClass(jsRuntime, javaExceptionClassId, &definition) < 0) {
      javaExceptionClassId = 0;
      env->DeleteLocalRef(exception);
      return JS_ThrowOutOfMemory(jsContext);
    }
  }

  JS_ThrowInternalError(jsContext, "Java Exception");
  auto error = JS_GetException(jsContext);
  if (!JS_IsObject(error)) {
    env->DeleteLocalRef(exception);
    return JS_Throw(jsContext, error);
  }
  auto holder = JS_NewObjectClass(jsContext, javaExceptionClassId);
  if (JS_IsException(holder)) {
    env->DeleteLocalRef(exception);
    JS_FreeValue(jsContext, error);
    return JS_EXCEPTION;
  }
  auto globalCause = static_cast<jthrowable>(env->NewGlobalRef(exception));
  env->DeleteLocalRef(exception);
  auto cause = globalCause ? new(std::nothrow) JavaExceptionCause{this, globalCause} : nullptr;
  if (!cause) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteGlobalRef(globalCause);
    JS_FreeValue(jsContext, holder);
    JS_FreeValue(jsContext, error);
    return JS_ThrowOutOfMemory(jsContext);
  }
  JS_SetOpaque(holder, cause);
  // A non-enumerable, read-only holder follows this specific error's lifetime.
  if (JS_DefinePropertyValueStr(jsContext, error, JAVA_EXCEPTION_CAUSE, holder, 0) < 0) {
    JS_FreeValue(jsContext, error);
    return JS_EXCEPTION;
  }
  return JS_Throw(jsContext, error);
}

JNIEnv* Context::getEnv() const {
  JNIEnv* env = nullptr;
  javaVm->GetEnv(reinterpret_cast<void**>(&env), jniVersion);
  if (env) {
    return env;
  }

  javaVm->AttachCurrentThread(
#ifdef __ANDROID__
      &env,
#else
      reinterpret_cast<void**>(&env),
#endif
      nullptr);
  if (env) {
    thread_local JniThreadDetacher detacher(javaVm);
  }
  return env;
}

JSValue
Context::jsCall(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
  auto context = reinterpret_cast<const Context*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
  if (context) {
    auto proxy = reinterpret_cast<const JavaObjectProxy*>(JS_GetOpaque(this_val, context->jsClassId));
    if (proxy) {
      return proxy->call(magic, argc, argv);
    }
  }
  return JS_ThrowReferenceError(ctx, "Null Java Proxy");
}

jclass Context::getGlobalRef(JNIEnv* env, jclass clazz) {
  auto name = getName(env, clazz);
  auto i = globalReferences.find(name);
  if (i != globalReferences.end()) {
    return i->second;
  }

  auto globalRef = static_cast<jclass>(env->NewGlobalRef(clazz));
  globalReferences[name] = globalRef;
  return globalRef;
}

/*
 * Converts `string` to UTF-8. Prefer this over `GetStringUTFChars()` for any string that might
 * contain non-ASCII characters because that function returns modified UTF-8.
 */
std::string Context::toCppString(JNIEnv* env, jstring string) const {
  const jbyteArray utf8BytesObject = static_cast<jbyteArray>(env->CallObjectMethod(string, stringGetBytes, stringUtf8));
  if (!utf8BytesObject || env->ExceptionCheck()) return {};
  size_t utf8Length = env->GetArrayLength(utf8BytesObject);
  jbyte* utf8Bytes = env->GetByteArrayElements(utf8BytesObject, NULL);
  if (!utf8Bytes) {
    env->DeleteLocalRef(utf8BytesObject);
    return {};
  }
  std::string result = std::string(reinterpret_cast<char*>(utf8Bytes), utf8Length);
  env->ReleaseByteArrayElements(utf8BytesObject, utf8Bytes, JNI_ABORT);
  env->DeleteLocalRef(utf8BytesObject);
  return result;
}

/*
 * Converts `value` to a Java string. Prefer this over `NewStringUTF()` for any string that might
 * contain non-ASCII characters because that function expects modified UTF-8.
 */
jstring Context::toJavaString(JNIEnv* env, const JSValueConst& value) const {
  if (env->ExceptionCheck()) return nullptr;
  size_t utf8Length = 0;
  const char* string = JS_ToCStringLen(jsContext, &utf8Length, value);
  if (!string) {
    JS_FreeValue(jsContext, JS_GetException(jsContext));
    throwJsExceptionFmt(env, this, "Failed to convert JavaScript value to string");
    return nullptr;
  }
  if (utf8Length > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    JS_FreeCString(jsContext, string);
    throwJavaException(env, "java/lang/OutOfMemoryError", "JavaScript string exceeds Java array size");
    return nullptr;
  }
  jbyteArray utf8BytesObject = env->NewByteArray(utf8Length);
  if (!utf8BytesObject) {
    JS_FreeCString(jsContext, string);
    return nullptr;
  }
  env->SetByteArrayRegion(utf8BytesObject, 0, utf8Length,
                          reinterpret_cast<const jbyte*>(string));
  JS_FreeCString(jsContext, string);
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(utf8BytesObject);
    return nullptr;
  }
  jstring result = static_cast<jstring>(env->NewObject(stringClass, stringConstructor, utf8BytesObject, stringUtf8));
  env->DeleteLocalRef(utf8BytesObject);
  return result;
}
