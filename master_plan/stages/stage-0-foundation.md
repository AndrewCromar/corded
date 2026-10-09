# Stage 0: Foundation

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Next: [Stage 1](stage-1-protocol-spec.md)

## Goal

A repository where adding code is routine. One command configures, one builds, one tests,
on every supported platform, locally and in CI. No product logic is written in this stage.

## Scope

In scope: directory layout, CMake, vcpkg, compiler settings, formatting and static
analysis, sanitizers, test and fuzz harnesses, CI, developer documentation.

Out of scope: any networking, crypto, storage or protocol code beyond "hello world"
placeholders that prove each dependency links.

## Prerequisites

None. Blueprint source: section 2 (build system and dependency stack).

## Design

### Targets

| Target | Kind | Purpose |
|---|---|---|
| `corded_common` | static library | Code shared by core and server: ids, framing, logging setup |
| `corded_proto` | interface library | Headers generated from `proto/*.fbs` |
| `corded_core` | static library | All engine code, linked into the shared library and into tests |
| `corded` | shared library | `libcorded.so` / `corded.dll` / `libcorded.dylib`; only the C ABI is exported |
| `cordedd` | executable | Server daemon |
| `corded-tui` | executable | Terminal client, links only `corded` |
| `corded-cli` | executable | Headless developer client, links `corded_core` |
| `corded_tests_*` | executables | Catch2 test binaries, one per component |
| `corded_fuzz_*` | executables | libFuzzer targets, Clang only |

Keeping `corded_core` as a static library separate from the `corded` shared library lets
tests reach internal C++ classes while the shipped library exports only C symbols.

### Dependencies (vcpkg ports)

| Port | Used by | Stage first needed |
|---|---|---|
| `asio` | core, server | 2 |
| `openssl` | core, server (TLS), SQLCipher backend | 2 |
| `libsodium` | core, server (signature checks) | 2 |
| `flatbuffers` | core, server, build-time `flatc` | 1 |
| `sqlite3` | server | 2 |
| `sqlcipher` | core | 3 |
| `spdlog` | core, server | 2 |
| `concurrentqueue` | core | 3 |
| `nlohmann-json` | core | 3 |
| `ftxui` | tui | 5 |
| `catch2` | tests | 0 |

All are added to the manifest in this stage so that dependency problems surface now, not
in the middle of a later stage.

### Build presets

`CMakePresets.json` defines configure, build and test presets:

| Preset | Compiler | Notes |
|---|---|---|
| `dev` | system default | Debug, shared libs, tests on |
| `release` | system default | RelWithDebInfo, LTO |
| `asan` | Clang or GCC | Address and undefined behaviour sanitizers |
| `tsan` | Clang | Thread sanitizer |
| `fuzz` | Clang | libFuzzer plus ASan, builds fuzz targets |
| `static-server` | GCC or Clang | Fully static `cordedd`, musl on Linux |
| `ci-windows` | MSVC | `x64-windows-static` triplet |
| `coverage` | Clang | Source-based coverage |

## Steps

### 0.1 Repository skeleton and conventions

Tasks:
- Create the directory tree from [the overview](../00-overview.md), section 5, with a
  `README.md` stub in each top-level directory saying what belongs there.
- Add `CONTRIBUTING.md`: branch and commit conventions (reference step numbers), code
  style, how to run checks locally, the rule that decision changes go through
  `master_plan/02-decisions.md`.
- Add `CODE_OF_CONDUCT.md` (Contributor Covenant).
- Add issue and pull request templates under `.github/`.

Acceptance: the tree matches the overview; every directory explains itself.

### 0.2 CMake project, presets and targets

Tasks:
- Root `CMakeLists.txt`: `cmake_minimum_required(VERSION 3.25)`, project version `0.1.0`,
  C++20 required with extensions off, `CMAKE_EXPORT_COMPILE_COMMANDS` on,
  position-independent code on.
- `cmake/` modules: `CordedWarnings.cmake`, `CordedSanitizers.cmake`,
  `CordedFlatbuffers.cmake` (a function that runs `flatc` on `proto/*.fbs` and exposes the
  output as `corded_proto`), `CordedVersion.cmake` (git describe into a generated header).
- One `CMakeLists.txt` per component defining the targets in the table above. Settings are
  applied per target through an interface library `corded_build_options`, never through
  global flags.
- Symbol visibility hidden by default on the `corded` shared library; an export macro
  header generated with `GenerateExportHeader`.
- Options: `CORDED_BUILD_TESTS`, `CORDED_BUILD_FUZZERS`, `CORDED_BUILD_TUI`,
  `CORDED_BUILD_SERVER`, `CORDED_STATIC_SERVER`, `CORDED_ENABLE_INSECURE_TEST_CRYPTO`
  (default off, see Stage 3 step 3.9).
- `CMakePresets.json` with the presets listed in Design.

Acceptance: `cmake --preset dev && cmake --build --preset dev` produces every target on
Linux; `nm -D libcorded.so` shows no C++ symbols.

### 0.3 vcpkg manifest, baseline and triplets

Tasks:
- `vcpkg.json` listing every port in the dependency table, with features where needed
  (for example `sqlite3[json1]`), and a pinned `builtin-baseline` commit.
- `vcpkg-configuration.json` with the default registry at the same baseline.
- Manifest features so that optional parts pull only what they need: `server`, `tui`,
  `tests`.
- Decide how vcpkg is obtained: a git submodule at `external/vcpkg` pinned to the
  baseline, with `CMAKE_TOOLCHAIN_FILE` set in the presets. This keeps builds hermetic.
- Custom triplets under `cmake/triplets/` if needed: `x64-linux-musl-static` and
  `arm64-linux-musl-static` for the static server.
- Verify that `sqlite3` (server) and `sqlcipher` (client) coexist in one manifest. They
  are linked into different binaries but both install SQLite headers. If they conflict,
  split them with manifest features so each configure installs only one.
- A "link check" source file per target that includes one header from each dependency and
  calls one function, to prove linking works.

Acceptance: a clean clone builds with no pre-installed libraries beyond a compiler, CMake,
Ninja and git; the same commit produces the same dependency versions on two machines.

Risk: the `sqlite3` and `sqlcipher` coexistence above; `libsodium` must be 1.0.19 or newer
for HKDF, so check the baseline.

### 0.4 Compiler warnings, formatting, static analysis, sanitizers

Tasks:
- Warnings: GCC and Clang `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wformat=2
  -Wnull-dereference`; MSVC `/W4 /permissive-`. Warnings are errors when
  `CORDED_WARNINGS_AS_ERRORS` is on, which CI sets.
- Hardening flags for release builds: `-D_FORTIFY_SOURCE=3`, `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-fcf-protection` (x86), full RELRO, PIE; MSVC `/guard:cf`,
  `/Qspectre`.
- `.clang-format` (LLVM base, 100 columns, 4-space indent) and a `format` target plus a
  `format-check` target for CI.
- `.clang-tidy` with `bugprone-*`, `cert-*`, `concurrency-*`, `performance-*`,
  `modernize-*` (minus noisy checks), `cppcoreguidelines-*` (curated). A `tidy` preset.
- Sanitizer module providing `asan` (address plus undefined), `tsan`, and `msan` as an
  optional Clang-only preset.
- A pre-commit configuration (`.pre-commit-config.yaml`) running format check and a
  secret scanner (gitleaks).

Acceptance: introducing an unused variable, a misformatted line, or a heap overflow in a
test each fails the corresponding check.

### 0.5 Test and fuzz harness

Tasks:
- Catch2 v3 with `catch_discover_tests`, so each `TEST_CASE` is a CTest test.
- `tests/unit/<component>/`, `tests/integration/`, `tests/vectors/` (data files),
  `tests/fuzz/`, `tests/load/`.
- A test support library `corded_testing`: temporary directory fixture, deterministic
  random source, a helper to start a `cordedd` child process on an ephemeral port (used
  from Stage 2 onward), hex and base64 helpers.
- CTest labels: `unit`, `integration`, `slow`, `vectors`. Default local run is `unit`.
- Fuzz harness: one sample target `corded_fuzz_sample`; a `tests/fuzz/corpus/<target>/`
  directory convention; a script `tools/fuzz.sh <target> <seconds>`.
- Coverage preset producing an `lcov` report with `llvm-cov`.

Acceptance: `ctest --preset dev` runs one passing sample test; `tools/fuzz.sh
corded_fuzz_sample 10` runs and exits cleanly; the coverage preset writes a report.

### 0.6 Continuous integration matrix and caching

Tasks:
- GitHub Actions workflow `ci.yml`, triggered on push and pull request:

| Job | Runner | Preset | Runs |
|---|---|---|---|
| linux-gcc | `ubuntu-24.04` | `dev`, `release` | build, unit, integration |
| linux-clang-asan | `ubuntu-24.04` | `asan` | build, unit, integration |
| linux-clang-tsan | `ubuntu-24.04` | `tsan` | build, unit |
| linux-arm64 | `ubuntu-24.04-arm` | `release` | build, unit, integration |
| macos | `macos-14` | `release` | build, unit, integration |
| windows | `windows-2022` | `ci-windows` | build, unit, integration |
| lint | `ubuntu-24.04` | n/a | format check, clang-tidy, gitleaks |
| static-server | `ubuntu-24.04` | `static-server` | build, `ldd` check, size report |
| fuzz-smoke | `ubuntu-24.04` | `fuzz` | each fuzz target for 60 seconds |

- vcpkg binary caching through the GitHub Actions cache, keyed on `vcpkg.json` and the
  baseline. ccache or sccache for compiler output.
- Workflow `nightly.yml`: longer fuzz runs, coverage upload, `slow` tests.
- Branch protection on `main`: required checks, linear history.
- Dependabot for GitHub Actions versions; a monthly reminder issue to bump the vcpkg
  baseline.
- CodeQL workflow for C++.

Acceptance: a pull request shows all jobs green; a warm-cache run finishes in under 15
minutes; a deliberately broken commit is blocked from merging.

### 0.7 Developer documentation

Tasks:
- `docs/developer/building.md`: prerequisites per OS, presets, common problems.
- `docs/developer/testing.md`: how to run each test class and each sanitizer.
- `docs/developer/layout.md`: the directory tree and the dependency rules between
  components (`common` depends on nothing; `core` and `server` depend on `common` and
  `proto`; frontends depend only on the public header).
- Update the root `README.md` with a "Building" section.

Acceptance: someone who has not seen the repository builds and runs the tests on a clean
machine following only the documentation.

## Test plan

This stage is mostly tested by CI existing. Specific checks:

- A unit test that asserts the generated version header matches the CMake project version.
- A test that loads `libcorded` with `dlopen` / `LoadLibrary` and resolves one exported
  placeholder symbol, to prove the shared library and visibility settings work.
- A CI step that fails if `cordedd` from the `static-server` preset has any dynamic
  dependency.

## Risks and open questions

- **Windows and coroutines with asio.** MSVC support is good but less travelled. Mitigated
  by having Windows in CI from the first commit.
- **musl static builds of OpenSSL and SQLCipher.** Known to work but can need triplet
  tweaks. If it proves painful, fall back to static glibc linking of everything except
  libc and note it in the decisions file.
- **ARM64 runner availability.** If hosted ARM runners are unavailable, cross-compile with
  the `arm64-linux` triplet and run tests under QEMU user emulation.
- **vcpkg submodule versus system vcpkg.** The submodule is proposed for hermetic builds;
  revisit if clone size is a complaint.

## Definition of done

- All seven steps meet their acceptance criteria.
- CI is green on Linux x86_64, Linux ARM64, macOS and Windows.
- Every dependency in the manifest is linked by at least one placeholder target.
- Milestone M0 is demonstrated: a trivial change goes from pull request to merged with all
  checks passing.
