/*
 * Copyright (C) 2015 Square, Inc.
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
package app.cash.quickjs;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import java.io.Closeable;
import java.lang.ref.PhantomReference;
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.Proxy;
import java.util.Collections;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.logging.Logger;

/**
 * An ECMAScript (JavaScript) interpreter backed by the 'QuickJS' native engine.
 *
 * <p>This class is NOT thread safe. If multiple threads access an instance concurrently, the
 * caller must serialize all calls, including {@link #close()}. An instance may be used from
 * different threads one after another. The only method that may be called while another call is
 * running is from within that call, on the same thread: for example, a Java method that JavaScript
 * calls may call {@link #evaluate} again, but it may not call {@link #close()}.
 *
 * <p>Bytecode from {@link #compile} is tied to this version of QuickJS. Never {@link #execute}
 * bytecode from an untrusted source.
 */
public final class QuickJs implements Closeable {
  static {
    QuickJsNativeLoader.load();
  }

  /** Polled periodically while JavaScript runs. See {@link #setInterruptHandler}. */
  @Keep // Called from native code.
  public interface InterruptHandler {
    /**
     * Returns true to stop the running JavaScript. It then fails with a {@link QuickJsException}
     * that JavaScript code can't catch. If this method throws, JavaScript is stopped the same way
     * and the exception is rethrown to the caller instead.
     */
    boolean poll();
  }

  /**
   * Create a new interpreter instance. Calls to this method <strong>must</strong> be matched with
   * calls to {@link #close()} on the returned instance to avoid leaking native memory.
   */
  @NonNull
  public static QuickJs create() {
    LeakDetector.reportLeaks();
    long context = createContext();
    if (context == 0) {
      throw new OutOfMemoryError("Cannot create QuickJs instance");
    }
    return new QuickJs(context);
  }

  private long context;

  /**
   * Number of native calls in progress on this instance. JavaScript can call back into Java, which
   * may call into QuickJs again, so this counts reentrant calls on the same thread.
   */
  private int activeCalls;

  private final LeakDetector leakDetector;

  private QuickJs(long context) {
    this.context = context;
    this.leakDetector = new LeakDetector(this);
  }

  /**
   * Evaluate {@code script} and return any result. {@code fileName} will be used in error
   * reporting.
   *
   * <p>The result is a {@link String}, {@link Boolean}, {@link Integer}, {@link Double}, an
   * {@code Object[]} of these, or null. JavaScript values with no Java representation, such as
   * objects and functions, become null, including inside arrays.
   *
   * @throws QuickJsException if there is an error evaluating the script.
   */
  @Nullable
  public Object evaluate(@NonNull String script, @NonNull String fileName) {
    checkNotNull(script, "script");
    checkNotNull(fileName, "fileName");
    activeCalls++;
    try {
      return evaluate(context, script, fileName);
    } finally {
      activeCalls--;
    }
  }

  /**
   * Evaluate {@code script} and return a result. See {@link #evaluate(String, String)}.
   *
   * @throws QuickJsException if there is an error evaluating the script.
   */
  @Nullable
  public Object evaluate(@NonNull String script) {
    return evaluate(script, "?");
  }

  /**
   * Provides {@code object} to JavaScript as a global object called {@code name}. {@code type}
   * defines the interface implemented by {@code object} that will be accessible to JavaScript.
   * {@code type} must be an interface that does not extend any other interfaces, and cannot define
   * any overloaded methods.
   *
   * <p>Methods of the interface may return {@code void} or any of the following types, which may
   * also be used as parameters: {@code boolean}, {@link Boolean}, {@code int}, {@link Integer},
   * {@code double}, {@link Double}, {@link String}, {@link Object}, and arrays of these, including
   * multi-dimensional arrays. Methods may be varargs.
   */
  public <T> void set(@NonNull String name, @NonNull Class<T> type,
      @NonNull T object) {
    checkNotNull(name, "name");
    checkNotNull(type, "type");
    checkNotNull(object, "object");
    if (!type.isInterface()) {
      throw new UnsupportedOperationException("Only interfaces can be bound. Received: " + type);
    }
    if (type.getInterfaces().length > 0) {
      throw new UnsupportedOperationException(type + " must not extend other interfaces");
    }
    if (!type.isInstance(object)) {
      throw new IllegalArgumentException(object.getClass() + " is not an instance of " + type);
    }
    LinkedHashMap<String, Method> methods = new LinkedHashMap<>();
    for (Method method : type.getMethods()) {
      if (Modifier.isStatic(method.getModifiers())) continue;
      if (methods.put(method.getName(), method) != null) {
        throw new UnsupportedOperationException(method.getName() + " is overloaded in " + type);
      }
    }
    activeCalls++;
    try {
      set(context, name, object, methods.values().toArray());
    } finally {
      activeCalls--;
    }
  }

  /**
   * Attaches to a global JavaScript object called {@code name} that implements {@code type}.
   * {@code type} defines the interface implemented in JavaScript that will be accessible to Java.
   * {@code type} must be an interface that does not extend any other interfaces, and cannot define
   * any overloaded methods.
   *
   * <p>Methods of the interface may use the same types as {@link #set}. The returned proxy looks
   * up the global called {@code name} on every call, so it sees later reassignments.
   */
  @NonNull
  public <T> T get(@NonNull final String name, @NonNull final Class<T> type) {
    checkNotNull(name, "name");
    checkNotNull(type, "type");
    if (!type.isInterface()) {
      throw new UnsupportedOperationException("Only interfaces can be proxied. Received: " + type);
    }
    if (type.getInterfaces().length > 0) {
      throw new UnsupportedOperationException(type + " must not extend other interfaces");
    }
    LinkedHashMap<String, Method> methods = new LinkedHashMap<>();
    for (Method method : type.getMethods()) {
      if (Modifier.isStatic(method.getModifiers())) continue;
      if (methods.put(method.getName(), method) != null) {
        throw new UnsupportedOperationException(method.getName() + " is overloaded in " + type);
      }
    }

    // Native code identifies methods by their position in this array.
    Object[] methodArray = methods.values().toArray();
    final Map<Method, Integer> methodIndexes = new HashMap<>();
    for (int i = 0; i < methodArray.length; i++) {
      methodIndexes.put((Method) methodArray[i], i);
    }

    final long instance;
    activeCalls++;
    try {
      instance = get(context, name, methodArray);
    } finally {
      activeCalls--;
    }

    Object proxy = Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] { type },
        new InvocationHandler() {
          @Override
          public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            if (method.getDeclaringClass() == Object.class) {
              switch (method.getName()) {
                case "equals":
                  return proxy == args[0];
                case "hashCode":
                  return System.identityHashCode(proxy);
                case "toString":
                  return toString();
                default:
                  throw new AssertionError("Unexpected Object method: " + method);
              }
            }
            Integer methodIndex = methodIndexes.get(method);
            if (methodIndex == null) {
              throw new AssertionError("Unexpected method: " + method);
            }
            activeCalls++;
            try {
              return call(context, instance, methodIndex, args);
            } finally {
              activeCalls--;
            }
          }

          @Override
          public String toString() {
            return String.format("QuickJsProxy{name=%s, type=%s}", name, type.getName());
          }
        });
    return (T) proxy;
  }

  /**
   * Compile {@code sourceCode} and return the bytecode. {@code fileName} will be used in error
   * reporting.
   *
   * @throws QuickJsException if the sourceCode could not be compiled.
   */
  @NonNull
  public byte[] compile(@NonNull String sourceCode, @NonNull String fileName) {
    checkNotNull(sourceCode, "sourceCode");
    checkNotNull(fileName, "fileName");
    activeCalls++;
    try {
      return compile(context, sourceCode, fileName);
    } finally {
      activeCalls--;
    }
  }

  /**
   * Load and execute {@code bytecode} and return the result, converted like
   * {@link #evaluate(String, String)}. Only execute bytecode from {@link #compile} with the same
   * version of this library; never execute bytecode from an untrusted source.
   *
   * @throws QuickJsException if there is an error loading or executing the code.
   */
  @Nullable
  public Object execute(@NonNull byte[] bytecode) {
    checkNotNull(bytecode, "bytecode");
    activeCalls++;
    try {
      return execute(context, bytecode);
    } finally {
      activeCalls--;
    }
  }

  /**
   * Runs up to {@code maxJobs} pending jobs, such as {@code Promise} reactions, and returns the
   * number run. Jobs only run when this method is called; they never run automatically. Jobs still
   * pending when this instance is closed are discarded.
   *
   * @throws IllegalArgumentException if {@code maxJobs} is not positive.
   * @throws QuickJsException if a job throws. Later jobs stay pending.
   */
  public int executePendingJobs(int maxJobs) {
    if (maxJobs <= 0) {
      throw new IllegalArgumentException("maxJobs <= 0: " + maxJobs);
    }
    activeCalls++;
    try {
      return executePendingJobs(context, maxJobs);
    } finally {
      activeCalls--;
    }
  }

  /**
   * Limits the memory that JavaScript may allocate to {@code bytes}, or removes the limit if
   * {@code bytes} is 0. Allocations over the limit fail with a {@link QuickJsException}.
   */
  public void setMemoryLimit(long bytes) {
    if (bytes < 0) {
      throw new IllegalArgumentException("bytes < 0: " + bytes);
    }
    setMemoryLimit(context, bytes);
  }

  /**
   * Limits the native stack that JavaScript may use to {@code bytes}, or removes the limit if
   * {@code bytes} is 0. The default is 256 KiB. Deeper recursion fails with a
   * {@link QuickJsException}. The limit must be smaller than the stack of the calling thread.
   */
  public void setMaxStackSize(long bytes) {
    if (bytes < 0) {
      throw new IllegalArgumentException("bytes < 0: " + bytes);
    }
    setMaxStackSize(context, bytes);
  }

  /**
   * Sets a handler that is polled periodically while JavaScript runs, on the thread running it,
   * which can stop runaway scripts. Pass null to remove the handler.
   */
  public void setInterruptHandler(@Nullable InterruptHandler handler) {
    setInterruptHandler(context, handler);
  }

  /**
   * Release the native resources associated with this object. You <strong>must</strong> call this
   * method for each instance to avoid leaking native memory.
   *
   * @throws IllegalStateException if called while this instance is executing, such as from a Java
   *     method that JavaScript is calling.
   */
  @Override public void close() {
    if (activeCalls > 0) {
      throw new IllegalStateException("Cannot close QuickJs while it is executing");
    }
    if (context != 0) {
      long contextToClose = context;
      context = 0;
      leakDetector.closed();
      destroyContext(contextToClose);
    }
  }

  // Not Objects.requireNonNull(), which needs API 19.
  private static void checkNotNull(Object value, String name) {
    if (value == null) {
      throw new NullPointerException(name + " == null");
    }
  }

  /**
   * Logs instances that became unreachable without being closed. Unlike {@code finalize()}, this
   * works on all supported Java and Android versions without delaying garbage collection.
   */
  private static final class LeakDetector extends PhantomReference<QuickJs> {
    private static final ReferenceQueue<QuickJs> queue = new ReferenceQueue<>();
    /** Keeps detectors reachable until their instance is closed or reported. */
    private static final Set<LeakDetector> open =
        Collections.synchronizedSet(Collections.newSetFromMap(new IdentityHashMap<LeakDetector, Boolean>()));

    LeakDetector(QuickJs quickJs) {
      super(quickJs, queue);
      open.add(this);
    }

    void closed() {
      open.remove(this);
      clear();
    }

    static void reportLeaks() {
      for (Reference<? extends QuickJs> leaked; (leaked = queue.poll()) != null; ) {
        if (open.remove(leaked)) {
          Logger.getLogger(QuickJs.class.getName()).warning("QuickJs instance leaked!");
        }
      }
    }
  }

  private static native long createContext();
  private native void destroyContext(long context);
  private native Object evaluate(long context, String sourceCode, String fileName);
  private native long get(long context, String name, Object[] methods);
  private native void set(long context, String name, Object object, Object[] methods);
  private native Object call(long context, long instance, int methodIndex, Object[] args);
  private native Object execute(long context, byte[] bytecode);
  private native byte[] compile(long context, String sourceCode, String fileName);
  private native int executePendingJobs(long context, int maxJobs);
  private native void setMemoryLimit(long context, long bytes);
  private native void setMaxStackSize(long context, long bytes);
  private native void setInterruptHandler(long context, InterruptHandler handler);
}
