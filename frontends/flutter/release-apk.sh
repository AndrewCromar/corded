#!/usr/bin/env bash
# Builds the phone APK signed with the local key and publishes it as a
# pre-release:  release-apk.sh <number> <notes file>
set -euo pipefail
cd "$(dirname "$0")/app"
n="$1"; notes="$2"
: "${CORDED_KEYSTORE:?set CORDED_KEYSTORE and CORDED_KEYSTORE_PASSWORD}"
flutter build apk --release --target-platform android-arm64 --split-per-abi \
  --build-name "$(git describe --tags --match 'v*' --always | sed 's/^v//')-test$n" \
  --build-number "$(( ($(date +%s) - 1767225600) / 600 ))" \
  --dart-define=CORDED_VERSION="$(git describe --tags --match 'v*' --always)"
out="$(mktemp -d)/corded-android-arm64.apk"
cp build/app/outputs/flutter-apk/app-arm64-v8a-release.apk "$out"
git tag "android-test-$n"
git push -q origin "android-test-$n"
# Uploads sometimes time out on a slow line: try again, starting clean each time.
for attempt in 1 2 3 4; do
  if gh release create "android-test-$n" "$out" --prerelease --title "Android test build $n" --notes-file "$notes" \
      && gh release view "android-test-$n" --json assets --jq '.assets[].name' | grep -q apk; then
    break
  fi
  gh release delete "android-test-$n" --yes >/dev/null 2>&1 || true
  [ "$attempt" = 4 ] && { echo "could not publish the release" >&2; exit 1; }
  sleep 20
done
