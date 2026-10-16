#!/bin/bash
#
# Replaces the vendored QuickJS sources with those of a release.
#
# Usage: ./update_quickjs.sh URL
#   URL is a release tarball, e.g. https://bellard.org/quickjs/quickjs-2021-03-27.tar.xz
#
# The new sources are prepared in a temporary directory and only replace the current ones once
# everything has been downloaded and checked, so a failure leaves the tree untouched.

set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 URL" >&2
  exit 2
fi
url=$1

cd "$(dirname "$0")"
target=quickjs/common/native/quickjs

# Files copied from the release. Review this list against each new release.
files=(
  cutils.c cutils.h
  libregexp.c libregexp.h libregexp-opcode.h
  libunicode.c libunicode.h libunicode-table.h
  list.h
  quickjs.c quickjs.h quickjs-atom.h quickjs-opcode.h
  VERSION
)
# Release sources the library doesn't use: tools, tests, the libc bindings and bignum support.
unused=(libbf.c qjs.c qjsc.c quickjs-libc.c run-test262.c unicode_gen.c)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl --fail --location --silent --show-error --output "$work/quickjs.tar.xz" "$url"
mkdir "$work/release"
tar -xJf "$work/quickjs.tar.xz" -C "$work/release"

release=("$work"/release/quickjs-*)
if [ ${#release[@]} -ne 1 ] || [ ! -d "${release[0]}" ]; then
  echo "Expected one quickjs-* directory in $url" >&2
  exit 1
fi
release=${release[0]}

mkdir "$work/new"
for file in "${files[@]}"; do
  if [ ! -f "$release/$file" ]; then
    echo "Missing $file in $url; update the file list in $0" >&2
    exit 1
  fi
  cp "$release/$file" "$work/new/"
done

# Library sources that aren't copied usually mean the file list is out of date.
for source in "$release"/*.c; do
  name=$(basename "$source")
  case " ${files[*]} ${unused[*]} " in
    *" $name "*) ;;
    *) echo "Note: not copying $name; check whether the library needs it." >&2 ;;
  esac
done

# Swap the directories so the old sources are only removed once the new ones are in place.
rm -rf "$target.old"
mv "$target" "$target.old"
mv "$work/new" "$target"
rm -rf "$target.old"

echo "Updated $target to QuickJS $(cat "$target/VERSION")."
echo "Bytecode from compile() is not compatible across QuickJS versions."
