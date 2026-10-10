#!/usr/bin/env bash
# Builds the phone APK signed with the local key and publishes it as a
# pre-release:  release-apk.sh <number> <notes file>
set -euo pipefail
cd "$(dirname "$0")/app"
n="$1"; notes="$2"
: "${CORDED_KEYSTORE:?set CORDED_KEYSTORE and CORDED_KEYSTORE_PASSWORD}"
flutter build apk --release --target-platform android-arm64 --split-per-abi \
  --build-name "0.3.0-test$n" --build-number "$n"
out="$(mktemp -d)/corded-android-arm64.apk"
cp build/app/outputs/flutter-apk/app-arm64-v8a-release.apk "$out"
git tag "android-test-$n"
git push -q origin "android-test-$n"
gh release create "android-test-$n" "$out" --prerelease --title "Android test build $n" --notes-file "$notes"
