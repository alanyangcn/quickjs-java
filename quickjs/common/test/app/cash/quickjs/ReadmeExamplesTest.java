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

import java.nio.charset.StandardCharsets;
import org.junit.Test;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.fail;

/** The examples from README.md. Keep the two in sync. */
public final class ReadmeExamplesTest {
  @Test public void usage() {
    try (QuickJs engine = QuickJs.create()) {
      assertThat(engine.evaluate("'hello world'.toUpperCase();")).isEqualTo("HELLO WORLD");
    }
  }

  interface Utf8 {
    String fromHex(String hex);
  }

  @Test public void callingJavaFromJavaScript() {
    try (QuickJs engine = QuickJs.create()) {
      Utf8 utf8 = new Utf8() {
        @Override public String fromHex(String hex) {
          byte[] bytes = new byte[hex.length() / 2];
          for (int i = 0; i < bytes.length; i++) {
            bytes[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
          }
          return new String(bytes, StandardCharsets.UTF_8);
        }
      };

      engine.set("Utf8", Utf8.class, utf8);

      String greeting = (String) engine.evaluate(""
          + "var hexEnc = 'EC9588EB8595ED9598EC84B8EC9A9421';\n"
          + "var message = Utf8.fromHex(hexEnc);\n"
          + "message;");
      assertThat(greeting).isEqualTo("안녕하세요!");
    }
  }

  interface Greeter {
    String greet(String name);
  }

  @Test public void callingJavaScriptFromJava() {
    try (QuickJs engine = QuickJs.create()) {
      engine.evaluate("var Greeter = { greet: function(name) { return 'Hello, ' + name; } };");

      Greeter greeter = engine.get("Greeter", Greeter.class);
      assertThat(greeter.greet("world")).isEqualTo("Hello, world");
    }
  }

  @Test public void promises() {
    try (QuickJs engine = QuickJs.create()) {
      engine.evaluate("var result; Promise.resolve(42).then(function(v) { result = v; });");
      while (engine.executePendingJobs(100) > 0) {
        // Keep going until no jobs are left.
      }
      assertThat(engine.evaluate("result")).isEqualTo(42);
    }
  }

  @Test public void limits() {
    try (QuickJs engine = QuickJs.create()) {
      engine.setMemoryLimit(16 * 1024 * 1024);
      engine.setMaxStackSize(256 * 1024);
      final long deadline = System.nanoTime() + 100_000_000L; // 100 ms.
      engine.setInterruptHandler(new QuickJs.InterruptHandler() {
        @Override public boolean poll() {
          return System.nanoTime() > deadline;
        }
      });
      try {
        engine.evaluate("while (true) {}");
        fail();
      } catch (QuickJsException e) {
        assertThat(e).hasMessageThat().isEqualTo("interrupted");
      }
    }
  }
}
