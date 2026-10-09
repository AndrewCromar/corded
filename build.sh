#!/usr/bin/env bash
# Builds everything: fetches vcpkg, builds the dependencies (slow the first
# time, cached afterwards), then compiles Corded into build/dev/bin.
set -euo pipefail
cd "$(dirname "$0")"

git submodule update --init --depth 1 external/vcpkg
if [ ! -x external/vcpkg/vcpkg ]; then
  ./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
fi

cmake --preset dev
cmake --build --preset dev -j"$(nproc)"

echo
echo "Built: build/dev/bin/cordedd  build/dev/bin/corded-tui  build/dev/bin/corded-cli"
echo "Run the tests with: ctest --preset dev"
