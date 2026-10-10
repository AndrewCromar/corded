#!/usr/bin/env bash
# Puts the Android builds of libcorded where the app's build looks for them.
# They come from the newest successful CI run of the given branch (default: the
# current one). Needs the GitHub CLI, signed in.
set -euo pipefail
cd "$(dirname "$0")"
branch="${1:-$(git rev-parse --abbrev-ref HEAD)}"
# The newest finished run that has both Android builds, whatever became of its other jobs.
run=""
for id in $(gh run list --workflow CI --branch "$branch" --status completed --limit 8 --json databaseId --jq '.[].databaseId'); do
  n=$(gh api "repos/{owner}/{repo}/actions/runs/$id/artifacts" --jq '[.artifacts[].name | select(startswith("libcorded-android-"))] | length')
  if [ "$n" = "2" ]; then run="$id"; break; fi
done
[ -n "$run" ] || { echo "no CI run on $branch has the Android builds" >&2; exit 1; }
echo "taking the core from CI run $run ($(gh run view "$run" --json headSha --jq '.headSha[0:7]'))"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for preset in android-arm64 android-x64; do
  gh run download "$run" --name "libcorded-$preset" --dir "$tmp/$preset"
  cp -r "$tmp/$preset/jniLibs/." app/android/app/src/main/jniLibs/
done
find app/android/app/src/main/jniLibs -name '*.so' -exec ls -l {} +
