#!/bin/bash
#
# Checks that a release tag matches the version in gradle.properties and CHANGELOG.md, and, given
# a directory of release files, that every file and POM carries that version.
#
# Usage: scripts/check-release-version.sh TAG [RELEASE_DIR]

set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "Usage: $0 TAG [RELEASE_DIR]" >&2
  exit 2
fi
tag=$1
dir=${2:-}

cd "$(dirname "$0")/.."
version=$(grep '^VERSION_NAME=' gradle.properties | cut -d= -f2)

failures=0
fail() {
  echo "FAIL $*" >&2
  failures=$((failures + 1))
}

[ "$tag" = "$version" ] || fail "tag $tag doesn't match VERSION_NAME=$version in gradle.properties"
case "$version" in
  *-SNAPSHOT) fail "VERSION_NAME=$version is a snapshot" ;;
esac
grep -q "^## \[$version\]" CHANGELOG.md || fail "CHANGELOG.md has no '## [$version]' section"

if [ -n "$dir" ]; then
  shopt -s nullglob
  files=("$dir"/*)
  [ ${#files[@]} -gt 0 ] || fail "$dir is empty"
  for file in "${files[@]}"; do
    name=$(basename "$file")
    case "$name" in
      *-"$version".* | *-"$version"-sources.jar | *-"$version"-javadoc.jar) ;;
      *) fail "$name isn't named for version $version" ;;
    esac
    if [[ "$name" == *.pom ]] && ! grep -q "<version>$version</version>" "$file"; then
      fail "$name doesn't declare version $version"
    fi
  done
  for expected in "quickjs-android-$version.aar" "quickjs-jvm-$version.jar"; do
    [ -f "$dir/$expected" ] || fail "missing $expected"
  done
fi

if [ "$failures" -gt 0 ]; then
  exit 1
fi
echo "Release $version checks passed."
