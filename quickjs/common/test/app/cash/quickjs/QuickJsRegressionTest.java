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
package app.cash.quickjs;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.fail;

/**
 * Regression tests for issues that used to crash the process or silently misbehave. Scenarios that
 * crashed natively run in a child JVM where one is available, so a regression fails the test
 * instead of killing the test runner.
 */
public final class QuickJsRegressionTest {
  private QuickJs quickjs;

  @Before public void setUp() {
    quickjs = QuickJs.create();
  }

  @After public void tearDown() {
    quickjs.close();
  }

  // R01: cyclic and deeply nested arrays.

  @Test public void directlyCyclicArrayThrows() throws Exception {
    runIsolated("directlyCyclicArrayThrows");
  }

  static void directlyCyclicArrayThrowsScenario(QuickJs quickjs) {
    try {
      quickjs.evaluate("var a = []; a[0] = a; a");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot marshal a cyclic JavaScript array to Java");
    }
    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
  }

  @Test public void indirectlyCyclicArrayThrows() throws Exception {
    runIsolated("indirectlyCyclicArrayThrows");
  }

  static void indirectlyCyclicArrayThrowsScenario(QuickJs quickjs) {
    try {
      quickjs.evaluate("var a = [1], b = [2, a]; a.push(b); a");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot marshal a cyclic JavaScript array to Java");
    }
    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
  }

  interface ObjectConsumer {
    void accept(Object value);
  }

  @Test public void cyclicArrayPassedToJavaThrows() throws Exception {
    runIsolated("cyclicArrayPassedToJavaThrows");
  }

  static void cyclicArrayPassedToJavaThrowsScenario(QuickJs quickjs) {
    quickjs.set("consumer", ObjectConsumer.class, new ObjectConsumer() {
      @Override public void accept(Object value) {
        throw new AssertionError();
      }
    });
    try {
      quickjs.evaluate("var a = []; a[0] = a; consumer.accept(a);");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot marshal a cyclic JavaScript array to Java");
    }
    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
  }

  @Test public void deeplyNestedArrayThrows() throws Exception {
    runIsolated("deeplyNestedArrayThrows");
  }

  static void deeplyNestedArrayThrowsScenario(QuickJs quickjs) {
    try {
      quickjs.evaluate("var a = []; for (var i = 0; i < 100000; i++) a = [a]; a");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat()
          .isEqualTo("Cannot marshal JavaScript arrays nested deeper than 256 to Java");
    }
    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
  }

  @Test public void nestedArrayWithinLimit() {
    Object result = quickjs.evaluate("var a = [1]; for (var i = 1; i < 256; i++) a = [a]; a");
    for (int i = 1; i < 256; i++) {
      result = ((Object[]) result)[0];
    }
    assertThat((Object[]) result).asList().containsExactly(1);
  }

  @Test public void repeatedArrayIsNotACycle() {
    Object[] result = (Object[]) quickjs.evaluate("var x = [1, 2]; [x, [x, x], x]");
    assertThat((Object[]) result[0]).asList().containsExactly(1, 2).inOrder();
    Object[] middle = (Object[]) result[1];
    assertThat((Object[]) middle[0]).asList().containsExactly(1, 2).inOrder();
    assertThat((Object[]) middle[1]).asList().containsExactly(1, 2).inOrder();
    assertThat((Object[]) result[2]).asList().containsExactly(1, 2).inOrder();
  }

  // R02: closing QuickJs from a callback while it is executing.

  interface Closer {
    void close();
  }

  @Test public void closeFromCallbackThrows() throws Exception {
    runIsolated("closeFromCallbackThrows");
  }

  static void closeFromCallbackThrowsScenario(final QuickJs quickjs) {
    quickjs.set("closer", Closer.class, new Closer() {
      @Override public void close() {
        quickjs.close();
      }
    });
    try {
      quickjs.evaluate("closer.close();");
      fail();
    } catch (IllegalStateException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot close QuickJs while it is executing");
    }

    // A callback reached through a JavaScript proxy is guarded too.
    quickjs.evaluate("var proxy = { run: function() { closer.close(); } };");
    Runnable proxy = quickjs.get("proxy", Runnable.class);
    try {
      proxy.run();
      fail();
    } catch (IllegalStateException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot close QuickJs while it is executing");
    }

    // The engine is still usable, and can be closed once it returns.
    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
    quickjs.close();
    try {
      quickjs.evaluate("1 + 1");
      fail();
    } catch (NullPointerException expected) {
    }
  }

  // R03: null arguments to the public API.

  interface Greeter {
    String greet();
  }

  @Test public void nullArgumentsThrow() throws Exception {
    runIsolated("nullArgumentsThrow");
  }

  static void nullArgumentsThrowScenario(final QuickJs quickjs) {
    Greeter greeter = new Greeter() {
      @Override public String greet() {
        return "hi";
      }
    };
    byte[] bytecode = quickjs.compile("1 + 1", "test.js");

    assertNullPointerException("script == null", () -> quickjs.evaluate(null));
    assertNullPointerException("script == null", () -> quickjs.evaluate(null, "test.js"));
    assertNullPointerException("fileName == null", () -> quickjs.evaluate("1", null));
    assertNullPointerException("sourceCode == null", () -> quickjs.compile(null, "test.js"));
    assertNullPointerException("fileName == null", () -> quickjs.compile("1", null));
    assertNullPointerException("bytecode == null", () -> quickjs.execute(null));
    assertNullPointerException("name == null", () -> quickjs.get(null, Greeter.class));
    assertNullPointerException("type == null", () -> quickjs.get("greeter", null));
    assertNullPointerException("name == null", () -> quickjs.set(null, Greeter.class, greeter));
    assertNullPointerException("type == null", () -> quickjs.set("greeter", null, greeter));
    assertNullPointerException("object == null", () -> quickjs.set("greeter", Greeter.class, null));

    assertThat(quickjs.execute(bytecode)).isEqualTo(2);
  }

  @Test public void nullArgumentsToNativeMethodsThrow() throws Exception {
    runIsolated("nullArgumentsToNativeMethodsThrow");
  }

  /** The native layer must not trust the Java layer's checks. */
  static void nullArgumentsToNativeMethodsThrowScenario(QuickJs quickjs) throws Exception {
    Field contextField = QuickJs.class.getDeclaredField("context");
    contextField.setAccessible(true);
    long context = contextField.getLong(quickjs);

    assertNativeNullPointerException(quickjs, "bytecode == null",
        "execute", new Class<?>[] {long.class, byte[].class}, context, null);
    assertNativeNullPointerException(quickjs, "sourceCode == null",
        "compile", new Class<?>[] {long.class, String.class, String.class}, context, null, "f");
    assertNativeNullPointerException(quickjs, "fileName == null",
        "compile", new Class<?>[] {long.class, String.class, String.class}, context, "1", null);
    assertNativeNullPointerException(quickjs, "sourceCode == null",
        "evaluate", new Class<?>[] {long.class, String.class, String.class}, context, null, "f");
    assertNativeNullPointerException(quickjs, "fileName == null",
        "evaluate", new Class<?>[] {long.class, String.class, String.class}, context, "1", null);
    assertNativeNullPointerException(quickjs, "name == null",
        "get", new Class<?>[] {long.class, String.class, Object[].class},
        context, null, new Object[0]);
    assertNativeNullPointerException(quickjs, "name == null",
        "set", new Class<?>[] {long.class, String.class, Object.class, Object[].class},
        context, null, new Object(), new Object[0]);

    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
  }

  // R04: failed Java to JavaScript argument conversion must not call the JavaScript function.

  interface Sorter {
    Object[] sort(Object[] values);
    Object[] sortVarArgs(Object... values);
  }

  @Test public void unsupportedArrayElementDoesNotCallJavaScript() {
    quickjs.evaluate(""
        + "var calls = 0;\n"
        + "var sorter = {\n"
        + "  sort: function(a) { calls++; return a; },\n"
        + "  sortVarArgs: function() { calls++; return [arguments.length]; }\n"
        + "};");
    Sorter sorter = quickjs.get("sorter", Sorter.class);

    Object[][] inputs = {
        {new Date(), 1, 2},
        {1, new Date(), 2},
        {1, 2, new Date()},
        {1, new Object[] {2, new Date()}},
    };
    for (Object[] input : inputs) {
      try {
        sorter.sort(input);
        fail();
      } catch (IllegalArgumentException e) {
        assertThat(e).hasMessageThat().isEqualTo("Unsupported Java type java.util.Date");
      }
      try {
        sorter.sortVarArgs(input);
        fail();
      } catch (IllegalArgumentException e) {
        assertThat(e).hasMessageThat().isEqualTo("Unsupported Java type java.util.Date");
      }
    }
    assertThat(quickjs.evaluate("calls")).isEqualTo(0);

    // No JavaScript exception is left pending.
    assertThat(sorter.sort(new Object[] {1, "a"})).asList().containsExactly(1, "a").inOrder();
    assertThat(sorter.sortVarArgs(1, 2, 3)).asList().containsExactly(3);
    assertThat(quickjs.evaluate("calls")).isEqualTo(2);
  }

  // R05: error messages containing characters outside the BMP.

  interface Echo {
    String echo(String value);
  }

  @Test public void conversionErrorMessageKeepsSupplementaryCharacters() {
    quickjs.set("e", Echo.class, new Echo() {
      @Override public String echo(String value) {
        return value;
      }
    });
    try {
      quickjs.evaluate("e.echo(['\\u{1F600}'])");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot convert value 😀 to String");
    }
  }

  @Test public void conversionErrorMessageForUnprintableValue() {
    quickjs.set("e", Echo.class, new Echo() {
      @Override public String echo(String value) {
        return value;
      }
    });
    try {
      quickjs.evaluate("e.echo(Symbol('x'))");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot convert value <unprintable> to String");
    }
    assertThat(quickjs.evaluate("e.echo('ok')")).isEqualTo("ok");
  }

  @Test public void truncatedErrorMessageDoesNotSplitCharacters() {
    quickjs.set("e", Echo.class, new Echo() {
      @Override public String echo(String value) {
        return value;
      }
    });
    try {
      quickjs.evaluate("e.echo(['\\u{1F600}'.repeat(300)])");
      fail();
    } catch (IllegalArgumentException e) {
      String prefix = "Cannot convert value ";
      String message = e.getMessage();
      assertThat(message).startsWith(prefix);
      String value = message.substring(prefix.length());
      assertThat(value).isNotEmpty();
      assertThat(value.length() % 2).isEqualTo(0);
      for (int i = 0; i < value.length(); i += 2) {
        assertThat(value.substring(i, i + 2)).isEqualTo("😀");
      }
    }
  }

  @Test public void marshalErrorMessageKeepsSupplementaryCharacters() {
    quickjs.set("consumer", ObjectConsumer.class, new ObjectConsumer() {
      @Override public void accept(Object value) {
        throw new AssertionError();
      }
    });
    try {
      quickjs.evaluate("consumer.accept({ toString: function() { return '\\u{1F600}'; } })");
      fail();
    } catch (QuickJsException e) {
      assertThat(e).hasMessageThat().isEqualTo("Cannot marshal value 😀 to Java");
    }
  }

  // R06: array lengths are read as indexes and bounded.

  interface DoubleArraySource {
    double[] get();
  }

  @Test public void sparseArrayLongerThanIntThrows() {
    try {
      quickjs.evaluate("var a = []; a.length = Math.pow(2, 31); a");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo(
          "Cannot marshal a JavaScript array of length 2147483648 to Java (limit 4194304)");
    }
  }

  @Test public void arrayLikeLengthIsValidated() {
    quickjs.evaluate("var source = { value: null, get: function() { return this.value; } };");
    DoubleArraySource source = quickjs.get("source", DoubleArraySource.class);

    quickjs.evaluate("source.value = { length: 4194305 };");
    try {
      source.get();
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo(
          "Cannot marshal a JavaScript array of length 4194305 to Java (limit 4194304)");
    }

    quickjs.evaluate("source.value = { length: -1 };");
    try {
      source.get();
      fail();
    } catch (QuickJsException e) {
      assertThat(e).hasMessageThat().isEqualTo("invalid array index");
    }

    quickjs.evaluate("source.value = { length: 2, 0: 1.5, 1: 2.5 };");
    assertThat(source.get()).usingExactEquality().containsExactly(1.5, 2.5).inOrder();
  }

  @Test public void totalArrayElementsAreBounded() {
    // Shared sub-arrays are converted once per reference, so 2048 references to one array of 2048
    // elements is more than 4194304 elements in total.
    try {
      quickjs.evaluate("var x = new Array(2048).fill(1); new Array(2048).fill(x)");
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat()
          .isEqualTo("Cannot marshal more than 4194304 JavaScript array elements to Java");
    }
    Object[] result = (Object[]) quickjs.evaluate(
        "var x = new Array(2048).fill(1); new Array(2047).fill(x)");
    assertThat(result).hasLength(2047);
  }

  // R08: exceptions thrown by getters are reported as they are.

  interface Greeters {
    String greet();
  }

  @Test public void getReportsGetterException() {
    quickjs.evaluate("var g = { get greet() { throw new Error('getter failed'); } };");
    try {
      quickjs.get("g", Greeters.class);
      fail();
    } catch (QuickJsException e) {
      assertThat(e).hasMessageThat().isEqualTo("getter failed");
    }
    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
  }

  @Test public void callReportsGlobalGetterException() {
    quickjs.evaluate(""
        + "var broken = false;\n"
        + "Object.defineProperty(globalThis, 'g', {\n"
        + "  get: function() {\n"
        + "    if (broken) throw new Error('global getter failed');\n"
        + "    return { greet: function() { return 'hi'; } };\n"
        + "  }\n"
        + "});");
    Greeters greeter = quickjs.get("g", Greeters.class);
    assertThat(greeter.greet()).isEqualTo("hi");

    quickjs.evaluate("broken = true;");
    try {
      greeter.greet();
      fail();
    } catch (QuickJsException e) {
      assertThat(e).hasMessageThat().isEqualTo("global getter failed");
    }

    quickjs.evaluate("broken = false;");
    assertThat(greeter.greet()).isEqualTo("hi");
  }

  // R09: numbers and booleans convert by the same strict rules everywhere.

  interface BoxedResults {
    Integer integer();
    Double number();
    Boolean bool();
    int primitiveInt();
    double primitiveDouble();
    boolean primitiveBoolean();
  }

  @Test public void boxedReturnValuesAreStrict() {
    quickjs.evaluate("var r = {\n"
        + "  value: null,\n"
        + "  integer: function() { return this.value; },\n"
        + "  number: function() { return this.value; },\n"
        + "  bool: function() { return this.value; },\n"
        + "  primitiveInt: function() { return this.value; },\n"
        + "  primitiveDouble: function() { return this.value; },\n"
        + "  primitiveBoolean: function() { return this.value; }\n"
        + "};");
    BoxedResults results = quickjs.get("r", BoxedResults.class);

    assertThat(results.integer()).isNull();
    assertThat(results.number()).isNull();
    assertThat(results.bool()).isNull();

    quickjs.evaluate("r.value = 'abc';");
    assertConversionFails("Cannot convert value abc to int", results::integer);
    assertConversionFails("Cannot convert value abc to double", results::number);
    assertConversionFails("Cannot convert value abc to boolean", results::bool);

    quickjs.evaluate("r.value = 1;");
    assertThat(results.integer()).isEqualTo(1);
    assertThat(results.number()).isEqualTo(1.0);
    assertConversionFails("Cannot convert value 1 to boolean", results::bool);

    quickjs.evaluate("r.value = 1.5;");
    assertConversionFails("Cannot convert value 1.5 to int", results::integer);
    assertThat(results.number()).isEqualTo(1.5);
  }

  @Test public void primitiveReturnValuesRejectNull() {
    quickjs.evaluate("var r = {\n"
        + "  value: undefined,\n"
        + "  integer: function() { return this.value; },\n"
        + "  number: function() { return this.value; },\n"
        + "  bool: function() { return this.value; },\n"
        + "  primitiveInt: function() { return this.value; },\n"
        + "  primitiveDouble: function() { return this.value; },\n"
        + "  primitiveBoolean: function() { return this.value; }\n"
        + "};");
    final BoxedResults results = quickjs.get("r", BoxedResults.class);

    assertConversionFails("Cannot convert value undefined to int", results::primitiveInt);
    assertConversionFails("Cannot convert value undefined to double", results::primitiveDouble);
    assertConversionFails("Cannot convert value undefined to boolean", results::primitiveBoolean);

    quickjs.evaluate("r.value = null;");
    assertConversionFails("Cannot convert value null to int", results::primitiveInt);

    quickjs.evaluate("r.value = true;");
    assertThat(results.primitiveBoolean()).isTrue();
  }

  interface IntConsumer {
    int accept(int value);
  }

  @Test public void intArgumentsAcceptExactIntegers() {
    quickjs.set("c", IntConsumer.class, new IntConsumer() {
      @Override public int accept(int value) {
        return value;
      }
    });
    assertThat(quickjs.evaluate("c.accept(-0)")).isEqualTo(0);
    assertThat(quickjs.evaluate("c.accept(0.5 + 0.5)")).isEqualTo(1);
    assertThat(quickjs.evaluate("c.accept(2147483647)")).isEqualTo(2147483647);
    assertThat(quickjs.evaluate("c.accept(-2147483648)")).isEqualTo(-2147483648);

    assertEvaluateFails("Cannot convert value 1.5 to int", "c.accept(1.5)");
    assertEvaluateFails("Cannot convert value NaN to int", "c.accept(NaN)");
    assertEvaluateFails("Cannot convert value 2147483648 to int", "c.accept(2147483648)");
    assertEvaluateFails("Cannot convert value 1 to int", "c.accept('1')");
  }

  interface BoxedConsumer {
    Object accept(Integer i, Boolean b);
  }

  @Test public void boxedArgumentsAreStrict() {
    quickjs.set("c", BoxedConsumer.class, new BoxedConsumer() {
      @Override public Object accept(Integer i, Boolean b) {
        return i + ":" + b;
      }
    });
    assertThat(quickjs.evaluate("c.accept(-0, true)")).isEqualTo("0:true");
    assertThat(quickjs.evaluate("c.accept(null, undefined)")).isEqualTo("null:null");
    assertEvaluateFails("Cannot convert value abc to int", "c.accept('abc', true)");
    assertEvaluateFails("Cannot convert value 1 to boolean", "c.accept(1, 1)");
  }

  // R10: evaluate() turns unsupported values into null at any depth.

  @Test public void evaluateUnsupportedValuesBecomeNull() {
    assertThat(quickjs.evaluate("({})")).isNull();
    assertThat((Object[]) quickjs.evaluate("[{}]")).asList().containsExactly((Object) null);
    Object[] result = (Object[]) quickjs.evaluate("[1, function() {}, [{}, 'a']]");
    assertThat(result[0]).isEqualTo(1);
    assertThat(result[1]).isNull();
    assertThat((Object[]) result[2]).asList().containsExactly(null, "a").inOrder();
  }

  // R15: interrupting scripts and limiting resources.

  @Test public void interruptHandlerStopsRunawayScript() {
    final AtomicInteger polls = new AtomicInteger();
    quickjs.setInterruptHandler(new QuickJs.InterruptHandler() {
      @Override public boolean poll() {
        return polls.incrementAndGet() >= 3;
      }
    });
    try {
      // JavaScript can't catch the interruption.
      quickjs.evaluate("try { while (true) {} } catch (e) { 'caught' }");
      fail();
    } catch (QuickJsException e) {
      assertThat(e).hasMessageThat().isEqualTo("interrupted");
    }
    assertThat(polls.get()).isEqualTo(3);

    quickjs.setInterruptHandler(null);
    assertThat(quickjs.evaluate("var n = 0; for (var i = 0; i < 100000; i++) n++; n"))
        .isEqualTo(100000);
  }

  @Test public void interruptHandlerExceptionIsRethrown() {
    quickjs.setInterruptHandler(new QuickJs.InterruptHandler() {
      @Override public boolean poll() {
        throw new IllegalStateException("deadline exceeded");
      }
    });
    try {
      quickjs.evaluate("while (true) {}");
      fail();
    } catch (IllegalStateException e) {
      assertThat(e).hasMessageThat().isEqualTo("deadline exceeded");
    }

    quickjs.setInterruptHandler(null);
    assertThat(quickjs.evaluate("1 + 1")).isEqualTo(2);
  }

  @Test public void memoryLimit() {
    quickjs.setMemoryLimit(4 * 1024 * 1024);
    try {
      quickjs.evaluate("var a = []; while (true) a.push(new Array(1000).fill(1));");
      fail();
    } catch (QuickJsException expected) {
      // Usually "out of memory". Near the limit, QuickJS may be unable to allocate the error
      // (it throws null) or the bridge may be unable to convert its message.
    }
    // Freeing memory needs memory too: remove the limit first.
    quickjs.setMemoryLimit(0);
    quickjs.evaluate("a = null;");
    assertThat(quickjs.evaluate("new Array(1000000).fill(1).length")).isEqualTo(1000000);
  }

  @Test public void maxStackSize() {
    // How deep JavaScript can recurse depends on the native stack frame size, which varies with
    // the ABI and compiler options, so compare depths rather than expecting fixed ones.
    quickjs.evaluate(""
        + "function depth(n) { return n == 0 ? 0 : 1 + depth(n - 1); }\n"
        + "function maxDepth() {\n"
        + "  var lo = 0, hi = 1000000;\n"
        + "  while (lo < hi) {\n"
        + "    var mid = (lo + hi + 1) >> 1;\n"
        + "    try { depth(mid); lo = mid; } catch (e) { hi = mid - 1; }\n"
        + "  }\n"
        + "  return lo;\n"
        + "}");
    int defaultDepth = (Integer) quickjs.evaluate("maxDepth()");
    assertThat(defaultDepth).isGreaterThan(10);

    quickjs.setMaxStackSize(32 * 1024);
    int smallDepth = (Integer) quickjs.evaluate("maxDepth()");
    assertThat(smallDepth).isLessThan(defaultDepth / 4);
    try {
      quickjs.evaluate("depth(" + (smallDepth + 10) + ")");
      fail();
    } catch (QuickJsException e) {
      assertThat(e).hasMessageThat().isEqualTo("stack overflow");
    }

    quickjs.setMaxStackSize(256 * 1024);
    assertThat(quickjs.evaluate("maxDepth()")).isEqualTo(defaultDepth);
  }

  @Test public void stackLimitFollowsCallingThread() throws Exception {
    runIsolated("stackLimitFollowsCallingThread");
  }

  /** The engine is created on one thread and used on another. */
  static void stackLimitFollowsCallingThreadScenario(final QuickJs quickjs) throws Exception {
    final AtomicReference<Throwable> failure = new AtomicReference<>();
    Thread thread = new Thread(null, new Runnable() {
      @Override public void run() {
        try {
          quickjs.evaluate("function depth(n) { return n == 0 ? 0 : 1 + depth(n - 1); }");
          assertThat(quickjs.evaluate("depth(10)")).isEqualTo(10);
          try {
            quickjs.evaluate("function forever() { return forever() + 1; } forever()");
            fail();
          } catch (QuickJsException e) {
            assertThat(e).hasMessageThat().isEqualTo("stack overflow");
          }
        } catch (Throwable t) {
          failure.set(t);
        }
      }
    }, "quickjs", 4 * 1024 * 1024);
    thread.start();
    thread.join();
    if (failure.get() instanceof Exception) throw (Exception) failure.get();
    if (failure.get() instanceof Error) throw (Error) failure.get();
  }

  // R16: Promise jobs run only when asked to.

  @Test public void executePendingJobs() {
    quickjs.evaluate(""
        + "var log = [];\n"
        + "Promise.resolve(1)\n"
        + "    .then(function(v) { log.push(v); return v + 1; })\n"
        + "    .then(function(v) { log.push(v); });");
    assertThat(quickjs.evaluate("log.length")).isEqualTo(0);

    assertThat(quickjs.executePendingJobs(1)).isEqualTo(1);
    assertThat(quickjs.evaluate("log.join()")).isEqualTo("1");

    assertThat(quickjs.executePendingJobs(100)).isEqualTo(1);
    assertThat(quickjs.evaluate("log.join()")).isEqualTo("1,2");
    assertThat(quickjs.executePendingJobs(100)).isEqualTo(0);

    try {
      quickjs.executePendingJobs(0);
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo("maxJobs <= 0: 0");
    }
  }

  // R17: compile() and names use the same UTF-8 encoding as evaluate().

  @Test public void compileAndEvaluateDecodeSourceAlike() {
    String[] scripts = {
        "'a\u0000b'.length",
        "'\uD800'.length + ':' + '\uD800'.charCodeAt(0)",
        "'\uD83D\uDE00'.length",
    };
    for (String script : scripts) {
      Object evaluated = quickjs.evaluate(script);
      Object executed = quickjs.execute(quickjs.compile(script, "test.js"));
      assertThat(executed).isEqualTo(evaluated);
    }
    assertThat(quickjs.evaluate("'a\u0000b'.length")).isEqualTo(3);
  }

  interface Named {
    String name();
  }

  @Test public void supplementaryCharactersInNames() {
    // U+1D4B3 is a letter outside the BMP, so it is a valid identifier.
    String name = "\uD835\uDCB3";
    quickjs.set(name, Named.class, new Named() {
      @Override public String name() {
        return "set";
      }
    });
    assertThat(quickjs.evaluate(name + ".name()")).isEqualTo("set");

    quickjs.evaluate("var " + name + "2 = { name: function() { return 'get'; } };");
    assertThat(quickjs.get(name + "2", Named.class).name()).isEqualTo("get");

    try {
      quickjs.evaluate("throw new Error('boom');", "\uD83D\uDE00.js");
      fail();
    } catch (QuickJsException e) {
      assertThat(e.getStackTrace()[0].getFileName()).isEqualTo("\uD83D\uDE00.js");
    }
  }

  // R18: proxies and leaked instances.

  @Test public void repeatedGetReturnsWorkingProxies() {
    quickjs.evaluate("var named = { name: function() { return 'a'; } };");
    List<Named> proxies = new ArrayList<>();
    for (int i = 0; i < 1000; i++) {
      proxies.add(quickjs.get("named", Named.class));
    }
    quickjs.evaluate("named = { name: function() { return 'b'; } };");
    for (Named proxy : proxies) {
      assertThat(proxy.name()).isEqualTo("b");
    }
  }

  @Test public void leakedInstanceIsReported() throws Exception {
    final List<String> warnings = new ArrayList<>();
    Handler handler = new Handler() {
      @Override public void publish(LogRecord record) {
        synchronized (warnings) {
          warnings.add(record.getMessage());
        }
      }

      @Override public void flush() {
      }

      @Override public void close() {
      }
    };
    Logger logger = Logger.getLogger(QuickJs.class.getName());
    logger.addHandler(handler);
    try {
      QuickJs.create(); // Leaked on purpose.
      for (int i = 0; i < 100; i++) {
        System.gc();
        Thread.sleep(10);
        QuickJs.create().close(); // Reports leaks found so far.
        synchronized (warnings) {
          if (!warnings.isEmpty()) break;
        }
      }
      synchronized (warnings) {
        assertThat(warnings).contains("QuickJs instance leaked!");
      }
    } finally {
      logger.removeHandler(handler);
    }
  }

  // R20: converters for java.lang.Object arguments are cached per runtime class.

  interface Echoes {
    Object echo(Object value);
  }

  @Test public void objectArgumentsOfManyTypes() {
    quickjs.evaluate("var echoes = { echo: function(v) { return v; } };");
    Echoes echoes = quickjs.get("echoes", Echoes.class);
    for (int i = 0; i < 3; i++) {
      assertThat(echoes.echo(1)).isEqualTo(1);
      assertThat(echoes.echo("a")).isEqualTo("a");
      assertThat(echoes.echo(true)).isEqualTo(true);
      assertThat(echoes.echo(1.5)).isEqualTo(1.5);
      assertThat((Object[]) echoes.echo(new Object[] {1, "b"})).asList()
          .containsExactly(1, "b").inOrder();
      assertThat((Object[]) echoes.echo(new String[] {"c"})).asList().containsExactly("c");
      try {
        echoes.echo(new Date());
        fail();
      } catch (IllegalArgumentException e) {
        assertThat(e).hasMessageThat().isEqualTo("Unsupported Java type java.util.Date");
      }
    }
  }

  private static void assertConversionFails(String message, Runnable runnable) {
    try {
      runnable.run();
      fail();
    } catch (IllegalArgumentException e) {
      assertThat(e).hasMessageThat().isEqualTo(message);
    }
  }

  private void assertEvaluateFails(final String message, final String script) {
    assertConversionFails(message, new Runnable() {
      @Override public void run() {
        quickjs.evaluate(script);
      }
    });
  }

  private static void assertNullPointerException(String message, Runnable runnable) {
    try {
      runnable.run();
      fail();
    } catch (NullPointerException e) {
      assertThat(e).hasMessageThat().isEqualTo(message);
    }
  }

  private static void assertNativeNullPointerException(QuickJs quickjs, String message,
      String name, Class<?>[] parameterTypes, Object... args) throws Exception {
    Method method = QuickJs.class.getDeclaredMethod(name, parameterTypes);
    method.setAccessible(true);
    try {
      method.invoke(quickjs, args);
      fail();
    } catch (InvocationTargetException e) {
      assertThat(e.getCause()).isInstanceOf(NullPointerException.class);
      assertThat(e.getCause()).hasMessageThat().isEqualTo(message);
    }
  }

  /**
   * Runs the static method {@code <name>Scenario(QuickJs)} in a child JVM if possible, otherwise
   * in this process (on Android, for example).
   */
  private void runIsolated(String name) throws Exception {
    File java = new File(System.getProperty("java.home"), "bin/java");
    if (!java.canExecute()) {
      runScenario(name, quickjs);
      return;
    }

    Process process = new ProcessBuilder(java.getPath(),
        "-cp", System.getProperty("java.class.path"),
        QuickJsRegressionTest.class.getName(), name)
        .redirectErrorStream(true)
        .start();
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    try (InputStream in = process.getInputStream()) {
      byte[] buffer = new byte[8192];
      for (int n; (n = in.read(buffer)) != -1; ) {
        output.write(buffer, 0, n);
      }
    }
    int exitCode = process.waitFor();
    if (exitCode != 0) {
      throw new AssertionError(
          "Scenario " + name + " exited with code " + exitCode + ":\n" + output.toString("UTF-8"));
    }
  }

  private static void runScenario(String name, QuickJs quickjs) throws Exception {
    Method scenario =
        QuickJsRegressionTest.class.getDeclaredMethod(name + "Scenario", QuickJs.class);
    try {
      scenario.invoke(null, quickjs);
    } catch (InvocationTargetException e) {
      Throwable cause = e.getCause();
      if (cause instanceof Exception) throw (Exception) cause;
      if (cause instanceof Error) throw (Error) cause;
      throw e;
    }
  }

  /** Entry point for the child JVM started by {@link #runIsolated}. */
  public static void main(String[] args) {
    QuickJs quickjs = QuickJs.create();
    try {
      runScenario(args[0], quickjs);
    } catch (Throwable t) {
      t.printStackTrace();
      System.exit(1);
    } finally {
      quickjs.close();
    }
    System.exit(0);
  }
}
