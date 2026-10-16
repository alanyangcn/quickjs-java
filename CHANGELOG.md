# Change Log

## [Unreleased]


## [0.10.0-16kb.1] - 2026-10-16

Changes in this fork since upstream 0.9.2. Earlier builds of the fork were versioned
`0.9.2-16kb.1` and `0.9.2-16kb.2` but not tagged. This release is `0.10.0-16kb.1` because it
changes behavior that callers may rely on.

### Breaking changes

* **Bytecode.** QuickJS is updated to 2026-06-04, so bytecode from `compile()` with earlier versions
  can't be executed. Compile your scripts again with this version.
* **Strict conversions.** `int` and `Integer` accept only numbers that are exactly an int, and
  `boolean` and `Boolean` only booleans, in arguments, return values and arrays alike. Values such
  as `'1'`, `1.5` or `1` (for a boolean) now throw `IllegalArgumentException` instead of being
  converted. Convert them in JavaScript, or use `double`, `String` or `Object`.
* **Primitive return values.** A proxy method returning `int`, `double` or `boolean` throws
  `IllegalArgumentException` when JavaScript returns `null` or `undefined`. Use `Integer`,
  `Double` or `Boolean` if JavaScript may return nothing.
* **`close()` while executing.** Calling `close()` from a Java method that JavaScript is calling
  throws `IllegalStateException`. Close the instance after the outermost call returns.
* **`evaluate()` results.** Values with no Java representation become `null` at any depth;
  `evaluate("[{}]")` returns `[null]` instead of throwing.
* **Limits.** Arrays passed between Java and JavaScript may have at most 4,194,304 elements in
  total and be nested at most 256 deep.
* **Native frames.** Stack traces no longer include QuickJS's own frames for native functions.
* **R8 and ProGuard.** Interfaces passed to `set()` and `get()` need `@Keep` in shrunk apps; see
  the README. This was already necessary, but it is now documented and tested.
* **JVM resources.** The JVM JAR stores its native libraries under `<os>-<arch>/`. This only
  matters if you build the JAR yourself.

### Added

* Android: native libraries load on devices with 16 KB pages (every ELF LOAD segment is aligned to
  16 KB). The build fails if they aren't.
* Android: a standalone build in `quickjs-android/` (NDK r28c, AGP 9.4.1, Gradle 9.8.0) with all
  four ABIs and a statically linked C++ runtime. The minimum Android version is now API 21.
* Android: the shared tests run as instrumentation tests, including on a 16 KB emulator in CI.
* JVM: native libraries for Linux and macOS on both x86_64 and arm64.
* `executePendingJobs(maxJobs)` runs pending jobs such as `Promise` reactions.
* `setMemoryLimit()`, `setMaxStackSize()` and `setInterruptHandler()` limit and stop scripts.
* `scripts/check-elf-alignment.sh` checks AARs and APKs for 16 KB page support.

### Changed

* QuickJS is updated from 2021-03-27 to 2026-06-04, which adds many newer JavaScript features
  (see its [change log](https://bellard.org/quickjs/Changelog)) and fixes a use-after-free that
  could crash the process when JavaScript ran out of memory. Bytecode from `compile()` with
  earlier versions can't be executed, some error messages changed, and stack traces of functions
  called through a proxy now include line numbers. The default stack limit stays 256 KiB rather
  than QuickJS's new default of 1 MiB.
* Numbers and booleans convert by the same strict rules everywhere: `int` and `Integer` accept only
  numbers that are exactly an int, `boolean` and `Boolean` only booleans. Previously some
  directions converted `'abc'` to `0` or used truthiness.
* A proxy method returning `int`, `double` or `boolean` throws `IllegalArgumentException` when
  JavaScript returns `null` or `undefined`, instead of a `NullPointerException`.
* `evaluate()` and `execute()` return `null` for unsupported values at any depth; previously they
  threw inside arrays.
* `close()` throws `IllegalStateException` when called while the instance is executing.
* Arrays passed between Java and JavaScript are limited to 4,194,304 elements in total.
* `compile()`, file names and global names use standard UTF-8 like `evaluate()`.
* Calling `get()` again for the same global and interface reuses the native proxy.
* Leaked instances are detected with a phantom reference instead of `finalize()`.
* Native frames of QuickJS functions are left out of the unified stack trace.
* The version is defined once, in `gradle.properties`.
* The JVM build uses Gradle 9.8.0 and CMake 3.13 or newer. The original `quickjs/android` module
  and `sample-octane` are no longer built.

### Fixed

* Crashes on cyclic or deeply nested arrays, on `null` arguments, on closing the engine from a
  callback, and on deep recursion when the engine is used from another thread than the one that
  created it. These now throw exceptions.
* A Java exception during argument conversion no longer lets the JavaScript function run.
* Error messages keep characters outside the BMP and never print `(null)`.
* `set()` no longer leaves the global behind when it fails.
* `get()` and proxy calls report exceptions thrown by getters.
* Array lengths are read correctly for sparse and array-like values.
* JNI exceptions are checked and local references released; the tests run cleanly under CheckJNI.
* Initialization failures and module resolution errors are reported instead of crashing or leaking.


## [0.9.2] - 2021-08-04

### Added

* `compile()` method takes JS source and produces a version-specific bytecode representation.
* `execute()` method takes version-specific bytecode and runs it.


### Changed

* Methods are no longer `synchronized`. If you are performing concurrent access add your own synchronization.


### Fixed

* Self-extract native libraries from JAR when running on the JVM.
* Correct UTF-8 handling of multi-byte graphemes to avoid mismatch between Java's modified UTF-8 and QuickJS's traditional UTF-8.


## [0.9.1] - 2021-07-12

JVM artifact is now available at `app.cash.quickjs:quickjs-jvm` for Linux and Mac OS!

### Fixed

* Handle null argument array which was sometimes supplied to native code instead of a zero-element array.
* Properly track the associated proxy class from native code to avoid a segfault.
* Eliminate a segfault during engine close when cleaning up proxied objects.


## [0.9.0] - 2021-06-14

Backing JS engine change to QuickJS.
Package name is now `app.cash.quickjs`.
Entrypoint is `QuickJs` class.
Maven coordinates are now `app.cash.quickjs:quickjs-android`.
The API and behavior should otherwise be unchanged.


[Unreleased]: https://github.com/alanyangcn/quickjs-java/compare/0.10.0-16kb.1...HEAD
[0.10.0-16kb.1]: https://github.com/alanyangcn/quickjs-java/compare/a738129cc4aa99206c00ae49e9c5b15cb58ad880...0.10.0-16kb.1
[0.9.2]: https://github.com/cashapp/quickjs-java/releases/tag/0.9.2
[0.9.1]: https://github.com/cashapp/quickjs-java/releases/tag/0.9.1
[0.9.0]: https://github.com/cashapp/quickjs-java/releases/tag/0.9.0



# Duktape change log

## Version 1.4.0 *(2021-06-14)*

 * New: Update to Duktape 2.6.0.
 * Fix: Correct a few JNI reference leaks which may have eventually caused a native crash.
 * Migrated to AndroidX annotations.

## Version 1.3.0 *(2018-08-02)*

 * New: update to Duktape 2.2.1.
 * Fix: update build settings to reduce AAR output size.

## Version 1.2.0 *(2017-09-08)*

 * New: support for arrays of supported types as arguments between Java/JavaScript.
 * New: update to Duktape 1.8.0.
 * Fix: explicitly release temporary JVM objects when returning from calls to Java from JavaScript.
 * Fix: allocate a local frame when binding Java interfaces to allow many methods and arguments.

## Version 1.1.0 *(2016-11-08)*

 * New: support parsing common date formats in JavaScript's "new Date('str')" constructor.
 * Fix: Duktape.evaluate returns null if the implicit return type is unsupported.

## Version 1.0.0 *(2016-09-28)*

 * Renamed Duktape.proxy and Duktape.bind to Duktape.get and Duktape.set.
 * New: support for arguments of type Object between Java/JavaScript.
 * New: support variadic (VarArgs) functions on Java/JavaScript calls.
 * Fix: Make creation and use of a Duktape instance thread-safe.

## Version 0.9.6 *(2016-08-31)*

 * New: call JavaScript methods from Java via proxies.
 * New: update to Duktape 1.5.0.

## Version 0.9.5 *(2016-03-07)*

 * New: call Java methods from JavaScript.
 * New: improved stacktraces. Includes both JavaScript and Java code from the call stack.
 * New: update to Duktape 1.4.0.

## Version 0.9.4 *(2015-11-02)*

 * New: expose JavaScript stacktraces when things fail.

## Version 0.9.3 *(2015-10-07)*

 * Fix: Use global refs in JNI.

## Version 0.9.2 *(2015-10-06)*

 * Fix: Get the timezone from Java rather than using UTC.
 * Fix: Use recommended flags for building.

## Version 0.9.1 *(2015-09-22)*

 * Fix: Correctly propagate errors as exceptions.


## Version 0.9.0 *(2015-09-08)*

Initial release.
