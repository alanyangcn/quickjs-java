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

import org.junit.Test;

import static app.cash.quickjs.QuickJsNativeLoader.nativeLibraryJarPath;
import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.fail;

public final class QuickJsNativeLoaderTest {
  @Test public void pathsIncludeOsAndArchitecture() {
    assertThat(nativeLibraryJarPath("Mac OS X", "aarch64"))
        .isEqualTo("/darwin-aarch64/libquickjs.dylib");
    assertThat(nativeLibraryJarPath("Mac OS X", "x86_64"))
        .isEqualTo("/darwin-x86_64/libquickjs.dylib");
    assertThat(nativeLibraryJarPath("Linux", "amd64")).isEqualTo("/linux-x86_64/libquickjs.so");
    assertThat(nativeLibraryJarPath("Linux", "aarch64")).isEqualTo("/linux-aarch64/libquickjs.so");
  }

  @Test public void unsupportedPlatformsNameOsAndArchitecture() {
    try {
      nativeLibraryJarPath("Linux", "riscv64");
      fail();
    } catch (IllegalStateException e) {
      assertThat(e).hasMessageThat().isEqualTo("Unsupported architecture: riscv64 on Linux");
    }
    try {
      nativeLibraryJarPath("Windows 11", "amd64");
      fail();
    } catch (IllegalStateException e) {
      assertThat(e).hasMessageThat().isEqualTo("Unsupported OS: Windows 11");
    }
  }
}
