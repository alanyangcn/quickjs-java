# The instrumentation tests call this entry point; nothing else is kept on purpose.
-keep class app.cash.quickjs.minifytest.Scripts {
  public static *;
}

# The test APK doesn't bundle the Kotlin standard library because this app already depends on it,
# so androidx.test, which runs in this app's process, needs it kept here.
-keep class kotlin.** { *; }
