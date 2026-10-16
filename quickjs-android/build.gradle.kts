import java.util.Properties

plugins {
  id("com.android.library") version "9.4.1"
  `maven-publish`
}

// The version and coordinates are shared with the JVM build in the repository root.
val rootProperties = Properties().apply {
  file("../gradle.properties").inputStream().use { load(it) }
}
group = rootProperties.getProperty("GROUP")
version = rootProperties.getProperty("VERSION_NAME")

android {
  namespace = "app.cash.quickjs"
  compileSdk = 37
  ndkVersion = "28.2.13676358"

  defaultConfig {
    minSdk = 21
    testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
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
    // The same tests that run on the JVM, as instrumentation tests.
    variant.androidTest?.sources?.java?.addStaticSourceDirectory("../quickjs/common/test")
  }
}

dependencies {
  api("androidx.annotation:annotation:1.1.0")

  androidTestImplementation("androidx.test:runner:1.6.2")
  androidTestImplementation("junit:junit:4.13.2")
  androidTestImplementation("com.google.truth:truth:1.0")
}

// Every native library must load on devices with 16 KB pages.
val checkElfAlignment = tasks.register<Exec>("checkElfAlignment") {
  description = "Checks that the release AAR's native libraries are 16 KB aligned."
  group = "verification"
  val aar = layout.buildDirectory.file("outputs/aar/quickjs-android-release.aar")
  val sdkDirectory = androidComponents.sdkComponents.sdkDirectory
  dependsOn("bundleReleaseAar")
  inputs.file(aar)
  doFirst {
    environment("ANDROID_HOME", sdkDirectory.get().asFile.absolutePath)
  }
  commandLine("../scripts/check-elf-alignment.sh", aar.get().asFile.absolutePath)
}

tasks.named("check") {
  dependsOn(checkElfAlignment)
}

// The "release" component only exists once the Android plugin has configured the project.
afterEvaluate {
  publishing {
    publications {
      create<MavenPublication>("release") {
        artifactId = "quickjs-android"
        from(components["release"])
        pom {
          name.set("QuickJS for Android")
          description.set(rootProperties.getProperty("POM_DESCRIPTION"))
          url.set(rootProperties.getProperty("POM_URL"))
          licenses {
            license {
              name.set(rootProperties.getProperty("POM_LICENCE_NAME"))
              url.set(rootProperties.getProperty("POM_LICENCE_URL"))
              distribution.set(rootProperties.getProperty("POM_LICENCE_DIST"))
            }
          }
          scm {
            url.set(rootProperties.getProperty("POM_SCM_URL"))
            connection.set(rootProperties.getProperty("POM_SCM_CONNECTION"))
            developerConnection.set(rootProperties.getProperty("POM_SCM_DEV_CONNECTION"))
          }
        }
      }
    }
  }
}
