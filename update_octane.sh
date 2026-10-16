#!/bin/bash
#
# Replaces the Octane benchmark sources used by sample-octane.
#
# The new files are prepared in a temporary directory and only replace the current ones once the
# download has succeeded, so a failure leaves the tree untouched.

set -euo pipefail

cd "$(dirname "$0")"
ASSET_LOCATION=sample-octane/src/main/assets/octane

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Download and extract ZIP from GitHub.
curl --fail --location --silent --show-error --output "$work/master.zip" \
  https://github.com/chromium/octane/archive/master.zip
unzip -q "$work/master.zip" -d "$work"

mkdir "$work/new"
cp "$work"/octane-master/*.js "$work/new/"
# Remove node-based runner.
rm "$work/new/run.js"
# Remove typescript test.
rm "$work"/new/typescript*.js

rm -f "$ASSET_LOCATION"/*.js
cp "$work"/new/*.js "$ASSET_LOCATION/"
