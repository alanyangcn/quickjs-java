cmake_minimum_required(VERSION 3.13...3.31)
project(quickjs LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

file(GLOB_RECURSE sources "../quickjs/common/native/*.c" "../quickjs/common/native/*.cpp")

add_library(quickjs SHARED ${sources})
# QuickJS uses GNU extensions such as malloc_usable_size().
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)

# Warnings for the bridge, but not for QuickJS's own sources. They are errors with Clang, which
# builds for Android and macOS; newer GCC versions may add warnings, so they stay warnings there.
file(GLOB bridge_sources "../quickjs/common/native/*.cpp")
set(bridge_warnings -Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND bridge_warnings -Werror)
endif()
set_source_files_properties(${bridge_sources} PROPERTIES COMPILE_OPTIONS "${bridge_warnings}")

if(DEFINED QUICKJS_VERSION)
  target_compile_definitions(quickjs PRIVATE CONFIG_VERSION="${QUICKJS_VERSION}")
endif()

# Keep every ELF LOAD segment compatible with Android's 16 KB page-size devices.
set_property(TARGET quickjs APPEND_STRING PROPERTY LINK_FLAGS
  " -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")

target_link_libraries(quickjs)
