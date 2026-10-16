cmake_minimum_required(VERSION 3.13...3.31)

# Load on any supported macOS version rather than only the one that built it.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum macOS version")
endif()

project(quickjs LANGUAGES C CXX)

# Choose the compiler with -DCMAKE_CXX_COMPILER or the CC/CXX environment variables; setting it
# here, after project(), has no effect.

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JAVA_AWT_LIBRARY NotNeeded)
set(JAVA_AWT_INCLUDE_PATH NotNeeded)
find_package(JNI REQUIRED)
include_directories(${JNI_INCLUDE_DIRS})

file(GLOB_RECURSE sources "../common/native/*.c" "../common/native/*.cpp")

add_library(quickjs SHARED ${sources})
target_compile_definitions(quickjs PUBLIC CONFIG_VERSION="${QUICKJS_VERSION}")
# QuickJS uses GNU extensions such as malloc_usable_size().
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)

# Warnings for the bridge, but not for QuickJS's own sources. They are errors with Clang, which
# builds for Android and macOS; newer GCC versions may add warnings, so they stay warnings there.
file(GLOB bridge_sources "../common/native/*.cpp")
set(bridge_warnings -Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND bridge_warnings -Werror)
endif()
set_source_files_properties(${bridge_sources} PROPERTIES COMPILE_OPTIONS "${bridge_warnings}")

if(NOT APPLE)
  # Don't depend on the build machine's C++ runtime.
  target_link_options(quickjs PRIVATE -static-libstdc++ -static-libgcc)
endif()
