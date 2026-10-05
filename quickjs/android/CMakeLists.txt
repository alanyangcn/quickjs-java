cmake_minimum_required(VERSION 3.4.1)
project(quickjs LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

file(GLOB_RECURSE sources "../common/native/*.c" "../common/native/*.cpp")

add_library(quickjs SHARED ${sources})

if(DEFINED QUICKJS_VERSION)
  target_compile_definitions(quickjs PRIVATE CONFIG_VERSION="${QUICKJS_VERSION}")
endif()

# Keep every ELF LOAD segment compatible with Android's 16 KB page-size devices.
# LINK_FLAGS also works with the original CMake 3.4.1 build entry point.
set_property(TARGET quickjs APPEND_STRING PROPERTY LINK_FLAGS
  " -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")

target_link_libraries(quickjs)
