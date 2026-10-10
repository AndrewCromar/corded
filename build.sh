#!/usr/bin/env bash
# Builds everything: fetches vcpkg, builds the dependencies (slow the first
# time, cached afterwards), then compiles Corded into build/dev/bin.
set -euo pipefail
cd "$(dirname "$0")"

# On ARM (a Raspberry Pi, for example) vcpkg has no prebuilt helper tools and
# must use the system's cmake and ninja.
case "$(uname -m)" in
  aarch64|arm64|armv7l) export VCPKG_FORCE_SYSTEM_BINARIES=1 ;;
esac

git submodule update --init --depth 1 external/vcpkg
if [ ! -x external/vcpkg/vcpkg ]; then
  ./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
fi

cmake --preset dev "$@"   # extra arguments are passed to CMake
cmake --build --preset dev -j"$(nproc)"

echo
echo "Built: build/dev/bin/cordedd  build/dev/bin/corded-tui  build/dev/bin/corded-cli"
echo "Run the tests with: ctest --preset dev"
