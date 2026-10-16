// An app that uses the library with R8 enabled. Its instrumentation tests run against the minified
// build, so they fail if the library's consumer rules or the documented @Keep advice are wrong.
plugins {
  id("com.android.application")
}

android {
  namespace = "app.cash.quickjs.minifytest"
  compileSdk = 37

  defaultConfig {
    applicationId = "app.cash.quickjs.minifytest"
    minSdk = 21
    targetSdk = 37
    versionCode = 1
    versionName = "1"
    testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
  }

  buildTypes {
    create("minified") {
      initWith(getByName("debug"))
      // R8 doesn't rename anything in debuggable builds.
      isDebuggable = false
      isMinifyEnabled = true
      proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
      testProguardFiles("test-proguard-rules.pro")
      matchingFallbacks += "release"
    }
  }

  testBuildType = "minified"
}

dependencies {
  implementation(project(":"))
  implementation("androidx.annotation:annotation:1.1.0")

  androidTestImplementation("androidx.test:runner:1.7.0")
  androidTestImplementation("junit:junit:4.13.2")
}
