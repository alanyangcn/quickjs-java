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
package app.cash.quickjs.minifytest;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import app.cash.quickjs.QuickJs;
import app.cash.quickjs.QuickJsException;

/**
 * Uses QuickJs the way an app would. R8 processes this code with only the library's consumer rules
 * and the @Keep annotations the README asks for.
 */
public final class Scripts {
  private Scripts() {
  }

  @Keep
  interface Greeter {
    String greet(String name);
  }

  @Keep
  interface Formatter {
    String format(String value, int count);
  }

  /** Calls a Java object from JavaScript. */
  @NonNull public static String callJavaFromJavaScript() {
    try (QuickJs engine = QuickJs.create()) {
      engine.set("formatter", Formatter.class, new Formatter() {
        @Override public String format(String value, int count) {
          return value + " x" + count;
        }
      });
      return (String) engine.evaluate("formatter.format('apple', 3)");
    }
  }

  /** Calls a JavaScript object from Java. */
  @NonNull public static String callJavaScriptFromJava() {
    try (QuickJs engine = QuickJs.create()) {
      engine.evaluate("var greeter = { greet: function(name) { return 'Hello, ' + name; } };");
      return engine.get("greeter", Greeter.class).greet("world");
    }
  }

  /** Native code constructs QuickJsException and adds the JavaScript stack to it. */
  @NonNull public static String javaScriptException() {
    try (QuickJs engine = QuickJs.create()) {
      engine.evaluate("function fail() { throw new Error('boom'); }\nfail();", "script.js");
      return "no exception";
    } catch (QuickJsException e) {
      return e.getMessage() + " at " + e.getStackTrace()[0];
    }
  }

  /** Native code calls the interrupt handler. */
  @NonNull public static String interrupt() {
    try (QuickJs engine = QuickJs.create()) {
      engine.setInterruptHandler(new QuickJs.InterruptHandler() {
        @Override public boolean poll() {
          return true;
        }
      });
      engine.evaluate("while (true) {}");
      return "not interrupted";
    } catch (QuickJsException e) {
      return e.getMessage();
    }
  }
}
