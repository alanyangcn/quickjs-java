# QuickJS Java (Android 16 KB fork)

The [QuickJS embeddable JavaScript engine][qjs] packaged for Android and the JVM.

This fork keeps the `app.cash.quickjs` Java API of `app.cash.quickjs:quickjs-android:0.9.2`,
released 2021-08-04 from upstream commit `a738129cc4aa99206c00ae49e9c5b15cb58ad880`, and adds:

 * QuickJS 2026-06-04, updated from 2021-03-27.
 * Android native libraries that load on devices with 16 KB pages, for all four ABIs.
 * A standalone Android build (`quickjs-android/`) with current tools and a minimum of API 21.
 * Fixes for crashes, swallowed errors and JNI misuse in the bridge, plus resource limits,
   interruption and explicit `Promise` job execution. See the [change log](CHANGELOG.md).

The upstream repository is now `cashapp/zipline`; its current `0.9.2` tag refers to a different
Zipline release.


## Usage

```java
try (QuickJs engine = QuickJs.create()) {
  Log.d("Greeting", engine.evaluate("'hello world'.toUpperCase();").toString());
}
```

For long-lived usage, call `QuickJs.create()` and keep the returned instance for as long as you
need. Call `close()` when you are done to free its native memory.

### Calling Java from JavaScript

You can provide a Java object as a JavaScript global and call its methods from JavaScript. First,
define a Java interface that declares the methods JavaScript may call:

```java
interface Utf8 {
  String fromHex(String hex);
}
```

Implement it in Java, here with [Okio's `ByteString`][okio]:

```java
Utf8 utf8 = new Utf8() {
  @Override public String fromHex(String hex) {
    return okio.ByteString.decodeHex(hex).utf8();
  }
};
```

Then set it as a JavaScript global:

```java
// Attach our interface to a JavaScript object called Utf8.
engine.set("Utf8", Utf8.class, utf8);

String greeting = (String) engine.evaluate(""
    // Here we have a hex encoded string.
    + "var hexEnc = 'EC9588EB8595ED9598EC84B8EC9A9421';\n"
    // Call out to Java to decode it!
    + "var message = Utf8.fromHex(hexEnc);\n"
    + "message;");

Log.d("Greeting", greeting);
```

### Calling JavaScript from Java

You can attach a Java interface to a JavaScript global object and call its functions from Java.
Define the interface:

```java
interface Greeter {
  String greet(String name);
}
```

Define the JavaScript object, then get a proxy for it:

```java
engine.evaluate("var Greeter = { greet: function(name) { return 'Hello, ' + name; } };");

Greeter greeter = engine.get("Greeter", Greeter.class);
Log.d("Greeting", greeter.greet("world")); // Hello, world
```

The interfaces passed to `set()` and `get()` must not extend other interfaces or overload
methods. The examples in this file are kept working by `ReadmeExamplesTest`.

### R8 and ProGuard

JavaScript finds the methods of interfaces passed to `set()` and `get()` by name, so apps that
shrink their code must keep those interfaces. Annotate them with `@Keep`:

```java
@Keep // androidx.annotation.Keep
interface Greeter {
  String greet(String name);
}
```

Without it, R8 may rename the methods or merge the interface into its implementation, and calls
fail with errors like `Only interfaces can be bound. Received: class q`. The library's own classes
need no rules; its AAR includes them. `quickjs-android/minify-test` checks this with R8.

### Supported types

The same types work in both directions: as parameters and return values of interfaces passed to
`set()`, which JavaScript calls, and of interfaces passed to `get()`, which Java calls.

| Java type | JavaScript value | Notes |
| --- | --- | --- |
| `boolean`, `Boolean` | `true`, `false` | Only booleans; `1` and `'true'` are rejected. |
| `int`, `Integer` | numbers that are exactly an int | `-0` and `2.0` are accepted; `1.5`, `NaN`, `2147483648` and strings are rejected. |
| `double`, `Double` | any number | |
| `String` | strings | Non-strings are rejected. |
| `Object` | any of the above, or an array of them | Other values (objects, functions, ...) are rejected, at any depth. Java values passed as `Object` must be one of these types too. |
| Arrays of the above | arrays | Including multi-dimensional arrays such as `Double[][]`. At most 4,194,304 elements. |
| `void` | ignored | Return type only. |

 * `null` and `undefined` become `null` for boxed types, `String`, `Object` and arrays. For `int`,
   `double` and `boolean` they are rejected.
 * Rejected values throw `IllegalArgumentException` with the value in the message, such as
   `Cannot convert value 1.5 to int`. Errors thrown by JavaScript throw `QuickJsException`.
 * Methods may be varargs. Java varargs are passed to JavaScript as separate arguments.
 * Arrays are copied, not shared. Converting an array that contains itself, arrays nested more
   than 256 deep, or more than 4,194,304 elements in total throws `IllegalArgumentException`.

`evaluate()` and `execute()` return a `String`, `Boolean`, `Integer`, `Double`, `Object[]` of these,
or `null`. Unlike arguments, values with no Java representation become `null`, at any depth:
`evaluate("({})")` returns `null` and `evaluate("[{}]")` returns `[null]`.

### Execution contract

 * **Threads.** `QuickJs` is not thread safe. Calls to one instance, including `close()`, must not
   overlap; serialize them yourself if several threads use it. An instance may move between
   threads.
 * **Reentrancy.** A Java method called from JavaScript may call back into the same instance, but
   it may not `close()` it: that throws `IllegalStateException`.
 * **Promises.** Jobs such as `Promise` reactions never run on their own. Run them with
   `executePendingJobs(maxJobs)`; jobs still pending at `close()` are discarded.

   ```java
   engine.evaluate("var result; Promise.resolve(42).then(function(v) { result = v; });");
   while (engine.executePendingJobs(100) > 0) {
     // Keep going until no jobs are left.
   }
   ```

 * **Proxies.** A proxy from `get()` looks up the global by name on every call, so it sees later
   reassignments of that global.
 * **Bytecode.** Bytecode from `compile()` only runs on the same QuickJS version. Never `execute()`
   bytecode from an untrusted source: QuickJS doesn't validate it.
 * **Leaks.** An instance that is garbage collected without being closed leaks its native memory;
   a warning is logged.

### Limits and interruption

```java
engine.setMemoryLimit(16 * 1024 * 1024); // Bytes; 0 removes the limit.
engine.setMaxStackSize(256 * 1024);      // The default; 0 removes the limit.
final long deadline = System.nanoTime() + 100_000_000L; // 100 ms.
engine.setInterruptHandler(new QuickJs.InterruptHandler() {
  @Override public boolean poll() {
    return System.nanoTime() > deadline;
  }
});
```

Exceeding a limit throws `QuickJsException` (`out of memory`, `stack overflow`). The interrupt
handler is polled on the thread running JavaScript; when it returns `true`, the script stops with a
`QuickJsException` (`interrupted`) that JavaScript can't catch.


## Using this fork

This fork isn't published to Maven Central. Use it in one of these ways.

### Android: source with a composite build

Add the fork as a Git submodule of your project:

```sh
git submodule add https://github.com/alanyangcn/quickjs-java.git vendor/quickjs-java
git submodule update --init
```

In your `settings.gradle.kts`, include its Android build:

```kotlin
includeBuild("vendor/quickjs-java/quickjs-android") {
  dependencySubstitution {
    substitute(module("app.cash.quickjs:quickjs-android")).using(project(":"))
  }
}
```

Then depend on it as usual:

```kotlin
dependencies {
  implementation("app.cash.quickjs:quickjs-android:0.9.2-16kb.3-SNAPSHOT")
}
```

The building machine needs JDK 17 or newer and the Android SDK; Gradle downloads NDK r28c and uses
CMake 3.22.1 from the SDK.

### Android: local Maven repository

```sh
cd quickjs-android
./gradlew publishReleasePublicationToMavenLocal
```

Then add `mavenLocal()` to your repositories and depend on
`app.cash.quickjs:quickjs-android:<version>`, with the version from `gradle.properties`.

### Android: AAR

Download `quickjs-android-<version>.aar` from a [release][releases], or build it with
`./gradlew assembleRelease` in `quickjs-android/`
(`build/outputs/aar/quickjs-android-release.aar`), and add it to your project:

```kotlin
dependencies {
  implementation(files("libs/quickjs-android-0.9.2-16kb.3.aar"))
  implementation("androidx.annotation:annotation:1.1.0")
}
```

The AAR has native libraries for `armeabi-v7a`, `arm64-v8a`, `x86` and `x86_64`. To reduce your app's
size, use [ABI splits](https://developer.android.com/build/configure-apk-splits) or an
[Android App Bundle](https://developer.android.com/guide/app-bundle).

### JVM

Download `quickjs-jvm-<version>.jar` from a [release][releases]. It supports Linux and macOS on
x86_64 and arm64 (aarch64), and needs `androidx.annotation:annotation:1.1.0`. To build it, see
below.

### Checking 16 KB page support

The build already checks the AAR. To check your own app, run:

```sh
scripts/check-elf-alignment.sh app.apk
```

It verifies that every native library's ELF LOAD segments are aligned to at least 16 KB
(`llvm-readelf -lW`) and that the APK passes `zipalign -c -P 16`. It needs `ANDROID_HOME`, or
`llvm-readelf` and `zipalign` on the `PATH`. These checks don't replace running the app on a device
or emulator where `adb shell getconf PAGE_SIZE` prints `16384`; see
[Android's 16 KB page size guide](https://developer.android.com/guide/practices/page-sizes).


## Building

The version is `VERSION_NAME` in `gradle.properties`; both builds read it from there.

### Android

```sh
cd quickjs-android
./gradlew build                      # Builds the AAR, runs lint and checks 16 KB alignment.
./gradlew connectedDebugAndroidTest  # Runs the tests on a connected device or emulator.
```

The tests in `quickjs/common/test` run both as instrumentation tests and on the JVM.

### JVM

The JVM build needs JDK 17 or newer, CMake 3.13 or newer and a C/C++ compiler. First build the
native library for your machine into `quickjs/jvm/src/main/resources/<os>-<arch>/`:

```sh
scripts/build-jvm-native.sh
```

Then build the JAR and run the tests, which use CheckJNI and fail on its warnings:

```sh
./gradlew :quickjs:jvm:build
```

The JAR is in `quickjs/jvm/build/libs/`. It includes the native libraries found in
`quickjs/jvm/src/main/resources/`; CI builds a JAR with all four platforms.

### Updating QuickJS

```sh
./update_quickjs.sh https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz
```


## History

Upstream released `app.cash.quickjs:quickjs-android` and `quickjs-jvm` up to 0.9.2 on Maven
Central. Those versions don't support 16 KB pages and lack this fork's fixes. The original
`quickjs/android` module and the `sample-octane` app are no longer built.

This repository used to host an Android packaging of the [Duktape](https://duktape.org/) engine.
Its history and release tags are still present, and its versions are listed on
[Maven Central](https://central.sonatype.com/artifact/com.squareup.duktape/duktape-android).


## License

    Copyright 2015 Square, Inc.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

Note: The included C code from QuickJS is licensed under MIT.


 [qjs]: https://bellard.org/quickjs/
 [okio]: https://github.com/square/okio
 [releases]: https://github.com/alanyangcn/quickjs-java/releases
