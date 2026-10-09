#!/usr/bin/env bash
# Builds everything: fetches vcpkg, builds the dependencies (slow the first
# time, cached afterwards), then compiles Corded into build/dev/bin.
set -euo pipefail
cd "$(dirname "$0")"

git submodule update --init --depth 1 external/vcpkg
if [ ! -x external/vcpkg/vcpkg ]; then
  ./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
fi

# Corded needs libsodium 1.0.19 or newer. If the system does not have it (Ubuntu
# and Debian ship an older one), build it along with the other dependencies.
if ! pkg-config --atleast-version=1.0.19 libsodium 2>/dev/null; then
  echo "System libsodium is missing or too old; building it with vcpkg."
  if ! command -v autoreconf >/dev/null || [ ! -e /usr/share/aclocal/ax_check_define.m4 ]; then
    echo "That needs autoconf, automake, libtool and autoconf-archive. On Ubuntu or Debian:"
    echo "  sudo apt-get install -y autoconf autoconf-archive automake libtool"
    exit 1
  fi
  set -- -DVCPKG_MANIFEST_FEATURES=vendored-sodium "$@"
fi

cmake --preset dev "$@"   # extra arguments are passed to CMake
cmake --build --preset dev -j"$(nproc)"

echo
echo "Built: build/dev/bin/cordedd  build/dev/bin/corded-tui  build/dev/bin/corded-cli"
echo "Run the tests with: ctest --preset dev"
