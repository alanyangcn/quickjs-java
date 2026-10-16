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

import org.junit.Test;

import static org.junit.Assert.assertEquals;

/** Runs against the R8-minified app. */
public final class MinifiedTest {
  @Test public void callJavaFromJavaScript() {
    assertEquals("apple x3", Scripts.callJavaFromJavaScript());
  }

  @Test public void callJavaScriptFromJava() {
    assertEquals("Hello, world", Scripts.callJavaScriptFromJava());
  }

  @Test public void javaScriptException() {
    assertEquals("boom at JavaScript.fail(script.js:1)", Scripts.javaScriptException());
  }

  @Test public void interrupt() {
    assertEquals("interrupted", Scripts.interrupt());
  }
}
