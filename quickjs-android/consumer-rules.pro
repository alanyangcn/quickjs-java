# Rules for apps that use this library with R8 or ProGuard.

# Native code finds these by name.
-keepclasseswithmembernames,includedescriptorclasses class app.cash.quickjs.QuickJs {
  native <methods>;
}
-keep class app.cash.quickjs.QuickJsException {
  <init>(java.lang.String, java.lang.String);
  private static void addJavaScriptStack(java.lang.Throwable, java.lang.String);
}
-keep interface app.cash.quickjs.QuickJs$InterruptHandler {
  boolean poll();
}

# Interfaces passed to QuickJs.set() and QuickJs.get() are matched to JavaScript by method name,
# so apps must keep them, for example by annotating them with @androidx.annotation.Keep.
