#!/bin/bash
#
# Checks that native libraries work on Android devices with 16 KB pages: every ELF LOAD segment
# must be aligned to at least 16 KB, and APKs must store libraries 16 KB aligned.
#
# Usage: scripts/check-elf-alignment.sh FILE...
#   FILE may be a .so, an .aar or an .apk. APKs are also checked with `zipalign -c -P 16`.
#
# Finds llvm-readelf and zipalign on the PATH, or in $ANDROID_HOME (or $ANDROID_SDK_ROOT).

set -euo pipefail

if [ $# -eq 0 ]; then
  echo "Usage: $0 FILE..." >&2
  exit 2
fi

sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"

find_tool() {
  local name=$1 pattern=$2
  if command -v "$name" > /dev/null; then
    command -v "$name"
  elif [ -n "$sdk" ]; then
    # The newest matching tool in the SDK.
    find "$sdk" -path "$pattern" -name "$name" 2> /dev/null | sort -V | tail -n 1
  fi
}

readelf=$(find_tool llvm-readelf "*/toolchains/llvm/prebuilt/*/bin/*")
if [ -z "$readelf" ]; then
  echo "llvm-readelf not found. Add it to the PATH or set ANDROID_HOME." >&2
  exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

failures=0
checked=0

check_so() {
  local so=$1 label=$2
  local alignments
  alignments=$("$readelf" -lW "$so" | awk '$1 == "LOAD" { print $NF }')
  if [ -z "$alignments" ]; then
    echo "FAIL $label: no LOAD segments" >&2
    failures=$((failures + 1))
    return
  fi
  local alignment
  for alignment in $alignments; do
    if (( alignment < 0x4000 )); then
      echo "FAIL $label: LOAD segment aligned to $alignment, need at least 0x4000" >&2
      failures=$((failures + 1))
      return
    fi
  done
  echo "OK   $label"
  checked=$((checked + 1))
}

for file in "$@"; do
  case "$file" in
    *.so)
      check_so "$file" "$file"
      ;;
    *.aar|*.apk)
      dir="$work/$(basename "$file")"
      mkdir -p "$dir"
      unzip -q -o "$file" '*.so' -d "$dir" || true
      found=0
      while IFS= read -r so; do
        check_so "$so" "$file!${so#"$dir"/}"
        found=1
      done < <(find "$dir" -name '*.so' | sort)
      if [ "$found" -eq 0 ]; then
        echo "FAIL $file: contains no native libraries" >&2
        failures=$((failures + 1))
      fi
      if [[ "$file" == *.apk ]]; then
        zipalign=$(find_tool zipalign "*/build-tools/*")
        if [ -z "$zipalign" ]; then
          echo "zipalign not found. Add it to the PATH or set ANDROID_HOME." >&2
          exit 2
        fi
        if "$zipalign" -c -P 16 4 "$file" > /dev/null; then
          echo "OK   $file: zipalign -P 16"
        else
          echo "FAIL $file: zipalign -c -P 16 4" >&2
          failures=$((failures + 1))
        fi
      fi
      ;;
    *)
      echo "Unsupported file: $file" >&2
      exit 2
      ;;
  esac
done

if [ "$failures" -gt 0 ]; then
  echo "$failures problem(s) found." >&2
  exit 1
fi
echo "All $checked native libraries are 16 KB aligned."
