#!/usr/bin/env bash
# Puts the Android builds of libcorded where the app's build looks for them.
# They come from the newest successful CI run of the given branch (default: the
# current one). Needs the GitHub CLI, signed in.
set -euo pipefail
cd "$(dirname "$0")"
branch="${1:-$(git rev-parse --abbrev-ref HEAD)}"
run=$(gh run list --workflow CI --branch "$branch" --status success --limit 1 --json databaseId --jq '.[0].databaseId')
[ -n "$run" ] && [ "$run" != "null" ] || { echo "no successful CI run on $branch" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for preset in android-arm64 android-x64; do
  gh run download "$run" --name "libcorded-$preset" --dir "$tmp/$preset"
  cp -r "$tmp/$preset/jniLibs/." app/android/app/src/main/jniLibs/
done
find app/android/app/src/main/jniLibs -name '*.so' -exec ls -l {} +
