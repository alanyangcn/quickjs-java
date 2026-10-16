#!/bin/bash
#
# Builds the JVM native library for this machine and copies it to
# quickjs/jvm/src/main/resources/<os>-<arch>/, where QuickJsNativeLoader looks for it.
#
# Usage: scripts/build-jvm-native.sh [extra CMake arguments...]

set -euo pipefail

cd "$(dirname "$0")/.."

case "$(uname -s)" in
  Darwin) os=darwin; library=libquickjs.dylib ;;
  Linux) os=linux; library=libquickjs.so ;;
  *) echo "Unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac

case "$(uname -m)" in
  x86_64|amd64) arch=x86_64 ;;
  arm64|aarch64) arch=aarch64 ;;
  *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

build_dir="build/jni/$os-$arch"
cmake -S quickjs/jvm -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKJS_VERSION="$(cat quickjs/common/native/quickjs/VERSION)" \
  "$@"
cmake --build "$build_dir" --verbose

output_dir="quickjs/jvm/src/main/resources/$os-$arch"
mkdir -p "$output_dir"
cp -v "$build_dir/$library" "$output_dir/"
