plugins {
  id("com.android.library") version "9.4.1"
  `maven-publish`
}

group = "app.cash.quickjs"
version = "0.9.2-16kb.1"

android {
  namespace = "app.cash.quickjs"
  compileSdk = 37
  ndkVersion = "28.2.13676358"

  defaultConfig {
    minSdk = 21
    ndk {
      abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86", "x86_64")
    }
    externalNativeBuild {
      cmake {
        arguments += "-DANDROID_STL=c++_static"
        val quickJsVersion = file("../quickjs/common/native/quickjs/VERSION").readText().trim()
        arguments += "-DQUICKJS_VERSION=$quickJsVersion"
        cFlags += "-fstrict-aliasing"
        cppFlags += "-fstrict-aliasing"
      }
    }
  }

  externalNativeBuild {
    cmake {
      path = file("../quickjs/android/CMakeLists.txt")
      version = "3.22.1"
    }
  }

  buildTypes {
    release {
      externalNativeBuild {
        cmake {
          arguments += "-DCMAKE_BUILD_TYPE=MinSizeRel"
          val flags = listOf("-g0", "-Os", "-fomit-frame-pointer", "-DNDEBUG", "-fvisibility=hidden")
          cFlags += flags
          cppFlags += flags
        }
      }
    }
  }

  publishing {
    singleVariant("release") {
      withSourcesJar()
    }
  }
}

androidComponents {
  onVariants { variant ->
    variant.sources.java?.addStaticSourceDirectory("../quickjs/common/java")
    variant.sources.java?.addStaticSourceDirectory("../quickjs/android/src/main/java")
  }
}

dependencies {
  api("androidx.annotation:annotation:1.1.0")
}

publishing {
  publications {
    register<MavenPublication>("release") {
      artifactId = "quickjs-android"
      afterEvaluate { from(components["release"]) }
    }
  }
}
