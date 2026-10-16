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
#include "Scoped.h"
#include "quickjs/quickjs.h"

/*
 * Converts `string` to standard UTF-8 without a Context, for use while building proxies.
 * GetStringUTFChars() would return modified UTF-8.
 */
static std::string javaStringToUtf8(JNIEnv* env, jstring string) {
  jmethodID getBytes;
  {
    ScopedLocalRef<jclass> stringClass(env, env->GetObjectClass(string));
    getBytes = env->GetMethodID(stringClass.get(), "getBytes", "(Ljava/lang/String;)[B");
  }
  if (!getBytes) return {};
  ScopedLocalRef<jstring> charsetName(env, env->NewStringUTF("UTF-8"));
  if (!charsetName) return {};
  ScopedLocalRef<jbyteArray> bytes(env, static_cast<jbyteArray>(
      env->CallObjectMethod(string, getBytes, charsetName.get())));
  if (!bytes || env->ExceptionCheck()) return {};
  std::string result;
  const jsize length = env->GetArrayLength(bytes.get());
  if (length > 0) {
    result.resize(length);
    env->GetByteArrayRegion(bytes.get(), 0, length, reinterpret_cast<jbyte*>(&result[0]));
  }
  return result;
}

std::string getName(JNIEnv* env, jobject object) {
  jmethodID method;
  {
    ScopedLocalRef<jclass> classType(env, env->GetObjectClass(object));
    method = env->GetMethodID(classType.get(), "getName", "()Ljava/lang/String;");
  }
  if (!method) return {};
  ScopedLocalRef<jstring> name(env, static_cast<jstring>(env->CallObjectMethod(object, method)));
  if (env->ExceptionCheck() || !name) return {};
  return javaStringToUtf8(env, name.get());
}

/** Returns a local reference to the component type of `arrayType`, or null with an exception. */
static jclass getComponentType(JNIEnv* env, jclass arrayType) {
  jmethodID method;
  {
    ScopedLocalRef<jclass> classType(env, env->GetObjectClass(arrayType));
    method = env->GetMethodID(classType.get(), "getComponentType", "()Ljava/lang/Class;");
  }
  if (!method) return nullptr;
  ScopedLocalRef<jclass> componentType(
      env, static_cast<jclass>(env->CallObjectMethod(arrayType, method)));
  if (env->ExceptionCheck()) return nullptr;
  return componentType.release();
}

/*
 * Returns `value` as a UTF-8 string for error messages. Never fails: values that cannot be
 * converted (such as symbols) get a placeholder.
 */
std::string describeValue(JSContext* jsContext, JSValueConst value) {
  ScopedCString str(jsContext, JS_ToCString(jsContext, value));
  if (!str.get()) {
    JS_FreeValue(jsContext, JS_GetException(jsContext));
    return "<unprintable>";
  }
  return str.get();
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
  ScopedLocalRef<jclass> localClass(env, env->FindClass(name));
  if (!localClass) return nullptr;
  return static_cast<jclass>(env->NewGlobalRef(localClass.get()));
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

  {
    ScopedLocalRef<jstring> utf8(env, env->NewStringUTF("UTF-8"));
    if (!utf8) return false;
    stringUtf8 = static_cast<jstring>(env->NewGlobalRef(utf8.get()));
  }
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
  std::vector<uint8_t> buffer(env->GetArrayLength(byteCode));
  if (!buffer.empty()) {
    env->GetByteArrayRegion(byteCode, 0, buffer.size(), reinterpret_cast<jbyte*>(buffer.data()));
  }
  const auto flags = JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE;
  ScopedJSValue obj(jsContext, JS_ReadObject(jsContext, buffer.data(), buffer.size(), flags));
  if (JS_IsException(obj.get())) {
    throwJsException(env, obj.get());
    return nullptr;
  }

  // compile() only produces scripts. Modules would need a module loader and evaluate
  // asynchronously, which this API doesn't support.
  if (JS_VALUE_GET_TAG(obj.get()) == JS_TAG_MODULE) {
    throwJavaException(env, "java/lang/IllegalArgumentException",
                       "Module bytecode is not supported");
    return nullptr;
  }

  // Takes ownership of obj.
  ScopedJSValue result(jsContext, JS_EvalFunction(jsContext, obj.release()));
  if (JS_IsException(result.get())) {
    throwJsException(env, result.get());
    return nullptr;
  }
  return toJavaObject(env, result.get(), false);
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

  ScopedJSValue compiled(jsContext, JS_Eval(jsContext, sourceCode.c_str(), sourceCode.length(),
                                            fileName.c_str(), JS_EVAL_FLAG_COMPILE_ONLY));
  if (JS_IsException(compiled.get())) {
    throwJsException(env, compiled.get());
    return nullptr;
  }

  size_t bufferLength = 0;
  std::unique_ptr<uint8_t, std::function<void(uint8_t*)>> buffer(
      JS_WriteObject(jsContext, &bufferLength, compiled.get(),
                     JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_REFERENCE),
      [this](uint8_t* p) { js_free(jsContext, p); });
  if (!buffer) {
    // JS_WriteObject() fails on allocation failure or an unserializable value.
    throwJsException(env, JS_EXCEPTION);
    return nullptr;
  }
  if (bufferLength > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    throwJavaException(env, "java/lang/OutOfMemoryError", "Bytecode exceeds Java array size");
    return nullptr;
  }
  jbyteArray result = env->NewByteArray(bufferLength);
  if (result) {
    env->SetByteArrayRegion(result, 0, bufferLength, reinterpret_cast<const jbyte*>(buffer.get()));
  }
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
  ScopedAtom nameAtom(jsContext, JS_NewAtomLen(jsContext, nameString.data(), nameString.size()));
  if (nameAtom.get() == JS_ATOM_NULL) {
    throwJsException(env, JS_EXCEPTION);
    return nullptr;
  }

  ScopedJSValue global(jsContext, JS_GetGlobalObject(jsContext));
  ScopedJSValue obj(jsContext, JS_GetProperty(jsContext, global.get(), nameAtom.get()));
  if (JS_IsException(obj.get())) {
    throwJsException(env, obj.get());
    return nullptr;
  }
  if (!JS_IsObject(obj.get())) {
    const char* msg = JS_IsUndefined(obj.get())
                      ? "A global JavaScript object called %s was not found"
                      : "JavaScript global called %s is not an object";
    throwJavaException(env, "java/lang/IllegalArgumentException", msg, nameStr);
    return nullptr;
  }

  // The proxy owns its own reference to the atom.
  std::unique_ptr<JsObjectProxy> jsObjectProxy(
      new JsObjectProxy(this, nameString, JS_DupAtom(jsContext, nameAtom.get())));
  const jsize numMethods = env->GetArrayLength(methods);
  for (jsize i = 0; i < numMethods && !env->ExceptionCheck(); ++i) {
    ScopedLocalRef<jobject> method(env, env->GetObjectArrayElement(methods, i));
    if (!method) break;
    const auto methodName = ::getName(env, method.get());
    if (env->ExceptionCheck()) break;

    ScopedAtom methodAtom(jsContext,
                          JS_NewAtomLen(jsContext, methodName.data(), methodName.size()));
    if (methodAtom.get() == JS_ATOM_NULL) {
      throwJsException(env, JS_EXCEPTION);
      break;
    }
    ScopedJSValue prop(jsContext, JS_GetProperty(jsContext, obj.get(), methodAtom.get()));
    if (JS_IsException(prop.get())) {
      throwJsException(env, prop.get());
    } else if (JS_IsFunction(jsContext, prop.get())) {
      jsObjectProxy->methods.push_back(new JsMethodProxy(
          this, env, methodName, JS_DupAtom(jsContext, methodAtom.get()), method.get()));
    } else {
      const char* msg = JS_IsUndefined(prop.get())
                        ? "JavaScript global %s has no method called %s"
                        : "JavaScript property %s.%s not callable";
      throwJsExceptionFmt(env, this, msg, nameStr, methodName.c_str());
    }
  }
  if (env->ExceptionCheck()) return nullptr;

  // Proxies look up the global by name on each call, so an equivalent proxy can be shared.
  for (auto existing : objectProxies) {
    if (existing->hasSameMethods(*jsObjectProxy)) return existing;
  }
  objectProxies.push_back(jsObjectProxy.get());
  return jsObjectProxy.release();
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

  ScopedAtom objName(jsContext, JS_NewAtomLen(jsContext, nameString.data(), nameString.size()));
  if (objName.get() == JS_ATOM_NULL) {
    throwJsException(env, JS_EXCEPTION);
    return;
  }
  ScopedJSValue global(jsContext, JS_GetGlobalObject(jsContext));

  const int hasProperty = JS_HasProperty(jsContext, global.get(), objName.get());
  if (hasProperty < 0) {
    throwJsException(env, JS_EXCEPTION);
    return;
  }
  if (hasProperty) {
    throwJavaException(env, "java/lang/IllegalArgumentException",
                       "A global object called %s already exists", nameStr);
    return;
  }

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
      return;
    }
  }

  // Fully build the proxy before publishing it, so a failure leaves no global behind.
  ScopedJSValue proxy(jsContext, JS_NewObjectClass(jsContext, jsClassId));
  if (JS_IsException(proxy.get())) {
    throwJsException(env, proxy.get());
    return;
  }
  std::unique_ptr<JavaObjectProxy> javaObject(
      new JavaObjectProxy(this, env, nameStr, object, methods, proxy.get()));
  if (env->ExceptionCheck()) return;
  JS_SetOpaque(proxy.get(), javaObject.release());
  // Takes ownership of proxy.
  if (JS_DefinePropertyValue(jsContext, global.get(), objName.get(), proxy.release(),
                             JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
    throwJsException(env, JS_EXCEPTION);
  }
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
    ScopedLocalRef<jthrowable> exception(env, env->ExceptionOccurred());
    env->ExceptionClear();
    context->interruptCause = static_cast<jthrowable>(env->NewGlobalRef(exception.get()));
    return 1;
  }
  return interrupt ? 1 : 0;
}

} // anonymous namespace

void Context::setInterruptHandler(JNIEnv* env, jobject handler) {
  jobject globalHandler = nullptr;
  jmethodID poll = nullptr;
  if (handler) {
    ScopedLocalRef<jclass> handlerClass(env, env->GetObjectClass(handler));
    poll = env->GetMethodID(handlerClass.get(), "poll", "()Z");
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
  uint64_t index = 0;
  {
    ScopedJSValue jsLength(jsContext, JS_GetPropertyStr(jsContext, value, "length"));
    if (JS_IsException(jsLength.get()) || JS_ToIndex(jsContext, &index, jsLength.get())) {
      throwJsException(env, JS_EXCEPTION);
      return false;
    }
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

JSValue
Context::newArray(JNIEnv* env, size_t length, const std::function<JSValue(size_t)>& element) {
  ScopedJSValue array(jsContext, JS_NewArray(jsContext));
  if (JS_IsException(array.get())) return JS_EXCEPTION;
  for (size_t i = 0; i < length; i++) {
    JSValue value = element(i);
    if (env->ExceptionCheck()) {
      JS_FreeValue(jsContext, value);
      return throwJavaExceptionFromJs(env);
    }
    if (JS_IsException(value)) return JS_EXCEPTION;
    // Takes ownership of value.
    if (JS_SetPropertyUint32(jsContext, array.get(), i, value) < 0) return JS_EXCEPTION;
  }
  return array.release();
}

template <typename ArrayType>
jvalue Context::toJavaArray(JNIEnv* env, const JSValueConst& value,
                            const std::function<ArrayType(jsize)>& newArray,
                            const std::function<void(ArrayType, jsize, JSValueConst)>& setElement) {
  jvalue result;
  result.l = nullptr;
  if (JS_IsNull(value) || JS_IsUndefined(value)) return result;
  if (JS_IsException(value)) {
    throwJsException(env, value);
    return result;
  }
  jsize length = 0;
  if (!getArrayLength(env, value, &length)) return result;
  ScopedLocalRef<ArrayType> array(env, newArray(length));
  if (!array) return result;
  for (jsize i = 0; i < length && !env->ExceptionCheck(); i++) {
    ScopedJSValue element(jsContext, JS_GetPropertyUint32(jsContext, value, i));
    if (JS_IsException(element.get())) {
      throwJsException(env, element.get());
      break;
    }
    setElement(array.get(), i, element.get());
  }
  if (!env->ExceptionCheck()) result.l = array.release();
  return result;
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
        if (std::find(arraysBeingConverted.begin(), arraysBeingConverted.end(),
                      JS_VALUE_GET_PTR(value))
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
        ScopedLocalRef<jobjectArray> array(
            env, env->NewObjectArray(arrayLength, objectClass, nullptr));
        arraysBeingConverted.push_back(JS_VALUE_GET_PTR(value));
        for (jsize i = 0; array && i < arrayLength && !env->ExceptionCheck(); i++) {
          ScopedJSValue element(jsContext, JS_GetPropertyUint32(jsContext, value, i));
          ScopedLocalRef<jobject> javaElement(
              env, toJavaObject(env, element.get(), throwOnUnsupportedType));
          if (!env->ExceptionCheck()) {
            env->SetObjectArrayElement(array.get(), i, javaElement.get());
          }
        }
        arraysBeingConverted.pop_back();
        result = env->ExceptionCheck() ? nullptr : array.release();
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

  ScopedJSValue evalValue(jsContext, JS_Eval(jsContext, sourceCodeString.c_str(),
                                             sourceCodeString.length(), fileName.c_str(), 0));
  return toJavaObject(env, evalValue.get(), false);
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
    ScopedLocalRef<jclass> elementType(env, getComponentType(env, type));
    if (!elementType) {
      return [](Context*, JNIEnv*, jvalue) { return JS_UNDEFINED; }; // Never called.
    }
    const auto elementTypeName = getName(env, elementType.get());
    if (elementTypeName == "double") {
      return [](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        const auto array = static_cast<jdoubleArray>(v.l);
        std::vector<jdouble> elements(env->GetArrayLength(array));
        if (!elements.empty()) env->GetDoubleArrayRegion(array, 0, elements.size(), &elements[0]);
        return c->newArray(env, elements.size(), [&](size_t i) {
          return JS_NewFloat64(c->jsContext, elements[i]);
        });
      };
    } else if (elementTypeName == "int") {
      return [](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        const auto array = static_cast<jintArray>(v.l);
        std::vector<jint> elements(env->GetArrayLength(array));
        if (!elements.empty()) env->GetIntArrayRegion(array, 0, elements.size(), &elements[0]);
        return c->newArray(env, elements.size(), [&](size_t i) {
          return JS_NewInt32(c->jsContext, elements[i]);
        });
      };
    } else if (elementTypeName == "boolean") {
      return [](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        const auto array = static_cast<jbooleanArray>(v.l);
        std::vector<jboolean> elements(env->GetArrayLength(array));
        if (!elements.empty()) env->GetBooleanArrayRegion(array, 0, elements.size(), &elements[0]);
        return c->newArray(env, elements.size(), [&](size_t i) {
          return JS_NewBool(c->jsContext, elements[i]);
        });
      };
    } else {
      auto converter = getJavaToJsConverter(env, elementType.get(), true);
      return [converter](Context* c, JNIEnv* env, jvalue v) {
        if (!v.l) return JS_NULL;
        const auto array = static_cast<jobjectArray>(v.l);
        return c->newArray(env, env->GetArrayLength(array), [&](size_t i) {
          jvalue element;
          element.l = env->GetObjectArrayElement(array, i);
          if (env->ExceptionCheck()) return JS_UNDEFINED;
          ScopedLocalRef<jobject> elementRef(env, element.l);
          return converter(c, env, element);
        });
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
      ScopedLocalRef<jclass> actualClass(env, env->GetObjectClass(v.l));
      if (!actualClass) return JS_NULL;
      if (env->IsSameObject(actualClass.get(), c->objectClass)) {
        throwJavaException(env, "java/lang/IllegalArgumentException",
                           "Unsupported Java type java.lang.Object");
        return JS_NULL;
      }
      // Building a converter takes several reflective calls, so reuse one per runtime class.
      for (const auto& entry : c->objectConverters) {
        if (env->IsSameObject(actualClass.get(), entry.first)) {
          return entry.second(c, env, v);
        }
      }
      const auto converter = c->getJavaToJsConverter(env, actualClass.get(), true);
      if (env->ExceptionCheck()) return JS_NULL;
      auto globalClass = static_cast<jclass>(env->NewGlobalRef(actualClass.get()));
      if (globalClass) c->objectConverters.emplace_back(globalClass, converter);
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
    ScopedLocalRef<jclass> elementType(env, getComponentType(env, type));
    if (!elementType) {
      return [](Context*, JNIEnv*, const JSValueConst&) { return jvalue(); }; // Never called.
    }
    const auto elementTypeName = getName(env, elementType.get());
    if (elementTypeName == "double") {
      return [](Context* c, JNIEnv* env, const JSValueConst& v) {
        return c->toJavaArray<jdoubleArray>(env, v, [&](jsize length) {
          return env->NewDoubleArray(length);
        }, [&](jdoubleArray array, jsize i, JSValueConst element) {
          if (!JS_IsNumber(element)) {
            throwJavaException(env, "java/lang/IllegalArgumentException",
                               "Cannot convert value %s to double",
                               describeValue(c->jsContext, element).c_str());
            return;
          }
          double d = 0;
          JS_ToFloat64(c->jsContext, &d, element); // Can't fail for a number.
          env->SetDoubleArrayRegion(array, i, 1, &d);
        });
      };
    } else if (elementTypeName == "int") {
      return [](Context* c, JNIEnv* env, const JSValueConst& v) {
        return c->toJavaArray<jintArray>(env, v, [&](jsize length) {
          return env->NewIntArray(length);
        }, [&](jintArray array, jsize i, JSValueConst element) {
          int32_t n;
          if (!toExactInt32(element, &n)) {
            throwJavaException(env, "java/lang/IllegalArgumentException",
                               "Cannot convert value %s to int",
                               describeValue(c->jsContext, element).c_str());
            return;
          }
          jint j = n;
          env->SetIntArrayRegion(array, i, 1, &j);
        });
      };
    } else if (elementTypeName == "boolean") {
      return [](Context* c, JNIEnv* env, const JSValueConst& v) {
        return c->toJavaArray<jbooleanArray>(env, v, [&](jsize length) {
          return env->NewBooleanArray(length);
        }, [&](jbooleanArray array, jsize i, JSValueConst element) {
          if (!JS_IsBool(element)) {
            throwJavaException(env, "java/lang/IllegalArgumentException",
                               "Cannot convert value %s to boolean",
                               describeValue(c->jsContext, element).c_str());
            return;
          }
          jboolean b = static_cast<jboolean>(JS_VALUE_GET_BOOL(element) != 0);
          env->SetBooleanArrayRegion(array, i, 1, &b);
        });
      };
    } else {
      auto converter = getJsToJavaConverter(env, elementType.get(), true);
      auto elementTypeGlobalRef = getGlobalRef(env, elementType.get());
      if (!elementTypeGlobalRef) {
        return [](Context*, JNIEnv*, const JSValueConst&) { return jvalue(); }; // Never called.
      }
      return [converter, elementTypeGlobalRef](Context* c, JNIEnv* env, const JSValueConst& v) {
        return c->toJavaArray<jobjectArray>(env, v, [&](jsize length) {
          return env->NewObjectArray(length, elementTypeGlobalRef, nullptr);
        }, [&](jobjectArray array, jsize i, JSValueConst element) {
          ScopedLocalRef<jobject> javaElement(env, converter(c, env, element).l);
          if (!env->ExceptionCheck()) env->SetObjectArrayElement(array, i, javaElement.get());
        });
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
  ScopedJSValue exceptionValue(jsContext, JS_GetException(jsContext));

  // Reads a property, ignoring failures: the exception being reported matters more.
  const auto getProperty = [this](const JSValueConst& object, const char* name) {
    JSValue property = JS_GetPropertyStr(jsContext, object, name);
    if (JS_IsException(property)) {
      JS_FreeValue(jsContext, JS_GetException(jsContext));
      return JS_UNDEFINED;
    }
    return property;
  };
  const bool isObject = JS_IsObject(exceptionValue.get());
  ScopedJSValue causeHolder(jsContext, javaExceptionClassId && isObject
      ? getProperty(exceptionValue.get(), JAVA_EXCEPTION_CAUSE)
      : JS_UNDEFINED);
  const auto cause = javaExceptionClassId
      ? static_cast<JavaExceptionCause*>(JS_GetOpaque(causeHolder.get(), javaExceptionClassId))
      : nullptr;
  ScopedJSValue messageValue(
      jsContext, isObject ? getProperty(exceptionValue.get(), "message") : JS_UNDEFINED);
  ScopedJSValue stackValue(
      jsContext, isObject ? getProperty(exceptionValue.get(), "stack") : JS_UNDEFINED);

  // If the JS does a `throw 2;`, there won't be a message property.
  ScopedLocalRef<jstring> message(env, toJavaString(
      env, JS_IsUndefined(messageValue.get()) ? exceptionValue.get() : messageValue.get()));
  if (env->ExceptionCheck()) return;
  ScopedLocalRef<jstring> stack(
      env, JS_IsUndefined(stackValue.get()) ? nullptr : toJavaString(env, stackValue.get()));
  if (env->ExceptionCheck()) return;

  if (cause) {
    // Rethrow the original Java exception, with the JavaScript stack added.
    ScopedLocalRef<jthrowable> exception(
        env, static_cast<jthrowable>(env->NewLocalRef(cause->throwable)));
    if (!exception) return;
    if (stack) {
      const jmethodID addJavaScriptStack =
          env->GetStaticMethodID(quickJsExceptionClass, "addJavaScriptStack",
                                 "(Ljava/lang/Throwable;Ljava/lang/String;)V");
      if (!addJavaScriptStack) return;
      env->CallStaticVoidMethod(quickJsExceptionClass, addJavaScriptStack, exception.get(),
                                stack.get());
      if (env->ExceptionCheck()) return;
    }
    env->Throw(exception.get());
  } else {
    ScopedLocalRef<jthrowable> exception(env, static_cast<jthrowable>(env->NewObject(
        quickJsExceptionClass, quickJsExceptionConstructor, message.get(), stack.get())));
    if (exception && !env->ExceptionCheck()) env->Throw(exception.get());
  }
}

JSValue Context::throwJavaExceptionFromJs(JNIEnv* env) const {
  assert(env->ExceptionCheck()); // There must be something to throw.
  ScopedLocalRef<jthrowable> exception(env, env->ExceptionOccurred());
  env->ExceptionClear();
  if (!javaExceptionClassId) {
    JS_NewClassID(&javaExceptionClassId);
    JSClassDef definition = {};
    definition.class_name = "QuickJsJavaExceptionCause";
    definition.finalizer = javaExceptionFinalize;
    if (JS_NewClass(jsRuntime, javaExceptionClassId, &definition) < 0) {
      javaExceptionClassId = 0;
      return JS_ThrowOutOfMemory(jsContext);
    }
  }

  JS_ThrowInternalError(jsContext, "Java Exception");
  ScopedJSValue error(jsContext, JS_GetException(jsContext));
  if (!JS_IsObject(error.get())) {
    return JS_Throw(jsContext, error.release());
  }
  ScopedJSValue holder(jsContext, JS_NewObjectClass(jsContext, javaExceptionClassId));
  if (JS_IsException(holder.get())) {
    return JS_EXCEPTION;
  }
  auto globalCause = static_cast<jthrowable>(env->NewGlobalRef(exception.get()));
  auto cause = globalCause ? new(std::nothrow) JavaExceptionCause{this, globalCause} : nullptr;
  if (!cause) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteGlobalRef(globalCause);
    return JS_ThrowOutOfMemory(jsContext);
  }
  JS_SetOpaque(holder.get(), cause);
  // A non-enumerable, read-only holder follows this specific error's lifetime. Takes ownership of
  // holder.
  if (JS_DefinePropertyValueStr(jsContext, error.get(), JAVA_EXCEPTION_CAUSE, holder.release(),
                                0) < 0) {
    return JS_EXCEPTION;
  }
  return JS_Throw(jsContext, error.release());
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
  if (env->ExceptionCheck()) return nullptr;
  auto i = globalReferences.find(name);
  if (i != globalReferences.end()) {
    return i->second;
  }

  auto globalRef = static_cast<jclass>(env->NewGlobalRef(clazz));
  if (globalRef) globalReferences[name] = globalRef;
  return globalRef;
}

/*
 * Converts `string` to UTF-8. Prefer this over `GetStringUTFChars()` for any string that might
 * contain non-ASCII characters because that function returns modified UTF-8.
 */
std::string Context::toCppString(JNIEnv* env, jstring string) const {
  ScopedLocalRef<jbyteArray> utf8Bytes(env, static_cast<jbyteArray>(
      env->CallObjectMethod(string, stringGetBytes, stringUtf8)));
  if (!utf8Bytes || env->ExceptionCheck()) return {};
  std::string result;
  const jsize length = env->GetArrayLength(utf8Bytes.get());
  if (length > 0) {
    result.resize(length);
    env->GetByteArrayRegion(utf8Bytes.get(), 0, length, reinterpret_cast<jbyte*>(&result[0]));
  }
  return result;
}

/*
 * Converts `value` to a Java string. Prefer this over `NewStringUTF()` for any string that might
 * contain non-ASCII characters because that function expects modified UTF-8.
 */
jstring Context::toJavaString(JNIEnv* env, const JSValueConst& value) const {
  if (env->ExceptionCheck()) return nullptr;
  size_t utf8Length = 0;
  ScopedCString string(jsContext, JS_ToCStringLen(jsContext, &utf8Length, value));
  if (!string.get()) {
    JS_FreeValue(jsContext, JS_GetException(jsContext));
    throwJsExceptionFmt(env, this, "Failed to convert JavaScript value to string");
    return nullptr;
  }
  if (utf8Length > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    throwJavaException(env, "java/lang/OutOfMemoryError", "JavaScript string exceeds Java array size");
    return nullptr;
  }
  ScopedLocalRef<jbyteArray> utf8Bytes(env, env->NewByteArray(utf8Length));
  if (!utf8Bytes) return nullptr;
  env->SetByteArrayRegion(utf8Bytes.get(), 0, utf8Length,
                          reinterpret_cast<const jbyte*>(string.get()));
  if (env->ExceptionCheck()) return nullptr;
  return static_cast<jstring>(
      env->NewObject(stringClass, stringConstructor, utf8Bytes.get(), stringUtf8));
}
