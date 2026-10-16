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
import java.util.Date;
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
