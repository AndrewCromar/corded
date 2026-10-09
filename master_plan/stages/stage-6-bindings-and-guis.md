# Stage 6: Bindings and GUIs

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Previous: [Stage 5](stage-5-c-abi-and-tui.md) | Next: [Stage 7](stage-7-hardening-and-release.md)

## Goal

Make the core usable outside C and C++. Ship reference wrappers for Python, Rust and C#,
build the core for mobile, and deliver one graphical client. The blueprint's promise is
that frontends are cheap to write; this stage is where that is tested by writing several.

## Scope

In scope: three language bindings, mobile builds of `libcorded`, one reference GUI,
example programs including a bot, documentation for binding authors.

Out of scope: a polished consumer app for every platform; app store distribution; push
notification infrastructure for mobile (noted under risks, since it needs server-side
design).

## Prerequisites

Stage 5 (frozen ABI 1.0 and the conformance scenarios). Blueprint source: section 5
(binding integration strategy), roadmap Phase 5 first two bullets.

## Design

### Every binding has the same three layers

```
 Layer 3: idiomatic API     Client, Room, Event classes; async/await; typed events
 Layer 2: safe wrapper      owns the handle, converts errors, runs the event pump
 Layer 1: raw FFI           one declaration per C function, generated where possible
```

Layer 1 is mechanical. Layer 2 is where each language's rules about threads and memory
are handled once. Layer 3 is optional sugar and may lag behind the core: because the ABI
has a generic command channel and string-named events, a binding at Layer 2 can already
use every feature the core has, including ones released after the binding.

### Event pump per language

All bindings use **pull mode** with the event file descriptor, not the C callback. This
avoids calling into a managed runtime from a foreign thread, which is the usual source of
crashes in FFI code.

| Language | Pump | Async model |
|---|---|---|
| Python | `loop.add_reader(corded_event_fd)` on POSIX; a helper thread plus `call_soon_threadsafe` on Windows | `asyncio` |
| Rust | A dedicated thread calling `corded_next_event`, forwarding into a channel | `Stream` of events, runtime-agnostic; optional Tokio feature |
| C# | A background task calling `corded_next_event`, publishing to `Channel<T>` | `IAsyncEnumerable<CordedEvent>` and `Task`-returning commands |
| Dart / Flutter | A helper isolate calling `corded_next_event`, sending over a `SendPort` | `Stream` and `Future` |

### Shared assets

- `docs/abi/commands.json` and `docs/abi/events.json` (from Stage 5) are the source for
  generated typed command builders and event classes in each language.
- The conformance scenarios from step 5.6 are replayed by each binding through a small
  scenario runner written in that language.

## Steps

### 6.1 Binding conventions and shared event schema

Tasks:
- Write `docs/bindings/guide.md`: the three layers, the pull-mode pump, how to match
  `command_result` events to requests, error mapping, passphrase handling (byte buffers,
  cleared after the call, never language-native immutable strings where avoidable),
  handle lifetime and destruction ordering, how to treat unknown events.
- Finalise the JSON Schema for commands and events, and write a small code generator
  (`tools/gen-bindings`) that emits typed event and command classes for Python, Rust and
  C# from it.
- Naming and versioning: each binding is versioned `<abi major>.<abi minor>.<binding
  patch>` and checks `corded_abi_version()` at load.
- Decide how each binding finds the native library: bundled in the package per platform
  (preferred) with an environment variable override for development.
- A shared scenario runner specification so each language's runner behaves the same.

Acceptance: the guide is sufficient for someone to write a binding in a fourth language;
the generator produces compiling output for all three targets.

### 6.2 Python binding

Tasks:
- Package `corded` in `bindings/python/`, built with `cffi` in ABI mode from the header
  (no compiler needed at install time).
- Layer 2: `Engine` class owning the handle, context-manager support, the asyncio pump,
  futures resolved by `command_result`.
- Layer 3: `Client`, `Room`, typed events (generated dataclasses), `async for event in
  client.events()`, `await room.send_text(...)`, `await room.send_event(type, content,
  relation=...)`.
- Wheels with the native library bundled for Linux (manylinux x86_64 and aarch64), macOS
  (universal2) and Windows, built by `cibuildwheel` in CI.
- Type hints throughout, `py.typed`, tested with mypy.
- Run the conformance scenarios with pytest.
- Example: `examples/python/echo_bot.py`.

Acceptance: `pip install` of a built wheel on a clean machine runs the echo bot against a
server; conformance scenarios pass on all three platforms.

### 6.3 Rust binding

Tasks:
- Crate `corded-sys` in `bindings/rust/corded-sys/`: `bindgen` output from the header,
  checked in and regenerated in CI to detect drift; `build.rs` locating or building the
  native library; a `vendored` feature that builds the core with CMake.
- Crate `corded`: safe wrapper. `Engine` is `Send + Sync`, drops cleanly, and exposes
  `Result<T, Error>`; passphrases taken as `zeroize`-backed secrets; events as an enum
  generated from the schema with an `Unknown { name, json }` variant.
- Async surface that does not force a runtime; optional `tokio` feature.
- No `unsafe` outside `corded-sys` and one small FFI module; `cargo miri` where it can
  run; `cargo clippy` clean.
- Run the conformance scenarios with `cargo test`.
- Example: a small command-line client.
- The blueprint mentions `cxx` as an alternative. Not used: `cxx` binds C++ directly and
  would bypass the C ABI this project exists to validate.

Acceptance: `cargo test` passes on all platforms; the example works against a server;
dropping the engine at any point in the test suite produces no leak or crash.

### 6.4 C# / .NET binding

Tasks:
- Project `Corded.Native` in `bindings/dotnet/`: `LibraryImport` source-generated
  P/Invoke declarations (.NET 8+), `SafeHandle` subclass for the engine.
- Project `Corded`: `CordedClient` with `Task`-returning commands,
  `IAsyncEnumerable<CordedEvent>`, generated record types for events, `IAsyncDisposable`.
- Passphrases as `ReadOnlySpan<byte>` pinned for the call and cleared by the caller.
- NuGet package with native assets under `runtimes/<rid>/native/` for `linux-x64`,
  `linux-arm64`, `osx-x64`, `osx-arm64`, `win-x64`.
- Native AOT and trimming compatibility checked.
- Run the conformance scenarios with xUnit.
- Example: a console client. A note and small sample for Godot (C#), which the blueprint
  names, showing how to pump events from `_Process`.

Acceptance: `dotnet test` passes on all platforms; the NuGet package works in a fresh
project with no manual native library setup.

### 6.5 Mobile builds of the core

Tasks:
- Android: CMake toolchain from the NDK, vcpkg triplets `arm64-android` and
  `x64-android`, a minimal AAR packaging the `.so` files and a thin JNI-free loader
  (Dart FFI and .NET load the library directly).
- iOS: `arm64-ios` and simulator triplets, an XCFramework. Static linking as iOS
  requires.
- Platform integration points in the core, added as ABI minor-version additions:
  - Vault location and file protection class (iOS data protection, Android app-private
    storage).
  - Optional unlock through the platform keystore: the vault key wrapped by a key in
    Keychain / Android Keystore, gated by biometrics, as an alternative to typing the
    passphrase every time. The key hierarchy from decision D-11 was chosen to allow this.
  - Lifecycle hooks: `corded_app_background` and `corded_app_foreground`, to drop and
    restore the connection cleanly.
- CI jobs that build both mobile targets and run the unit tests on an Android emulator
  and an iOS simulator.

Acceptance: the core unit tests pass on an Android emulator and an iOS simulator; a
minimal test app on each platform unlocks a vault and syncs.

### 6.6 Reference GUI client

Decision D-20 must be made before this step. The plan assumes the recommendation,
Flutter; the tasks are similar for Qt.

Tasks:
- Dart package `corded_ffi` in `bindings/dart/`: `ffigen` output, the isolate-based
  pump, typed events from the schema generator (a fourth generator target).
- App `frontends/gui/`: state management fed solely by the event stream, mirroring the
  TUI's view-model approach.
- Screens, at feature parity with the TUI plus what a GUI makes natural: onboarding,
  unlock, server and room list, timeline with replies and reactions, composer, members,
  verification with QR display and scan, settings.
- Desktop builds for Linux, macOS and Windows; mobile builds for Android and iOS using
  step 6.5.
- Notifications while the app is running (desktop and mobile local notifications).
- Accessibility basics: screen reader labels, keyboard navigation, scalable text.
- Golden-image tests for the main screens and an integration test driving the app
  against a real server.

Acceptance: milestone M6: the GUI, the TUI and a Python bot take part in the same
encrypted room, each showing the others' messages, replies and reactions.

### 6.7 Examples, bot kit and binding documentation

Tasks:
- `examples/` with, per language: connect and print events; send a message; an echo bot;
  a bot that defines and handles a custom event type (`com.example.dice.roll`),
  demonstrating that custom features need no core change.
- A small Python bot framework on top of the binding: command decorators, per-room
  state, graceful shutdown.
- API reference generated for each binding (Sphinx, rustdoc, DocFX) and published to
  GitHub Pages together with the C reference.
- A "write your own frontend" tutorial that builds a minimal client in about 150 lines
  of Python.
- Publish the packages: PyPI, crates.io, NuGet, pub.dev, each from a release workflow
  with provenance attestations. Reserve the package names early in the stage.

Acceptance: each published package installs and runs its example on a clean machine;
the tutorial has been followed start to finish by someone other than its author.

## Test plan

- The Stage 5 conformance scenarios, replayed in every language, on every platform that
  language targets.
- Per-language unit tests for Layer 2 behaviour: handle lifetime, error mapping, pump
  shutdown, cancellation.
- Leak checks: Python with `tracemalloc` and a debug allocator build of the core; Rust
  under Miri and LeakSanitizer; .NET with a finaliser-stress test.
- Interoperability matrix: every pair of {TUI, GUI, Python, Rust, C#} exchanging text,
  replies, reactions and an unknown custom event in one room.
- Package install tests in clean containers and virtual machines.

## Risks and open questions

- **Mobile background delivery.** Mobile operating systems kill background connections.
  Real-time notifications need a push service (FCM, APNs) and a server-side component
  that knows a device has mail without knowing what it says. This is a significant
  design item affecting the server and the threat model, and is deliberately not solved
  here. It should get its own design document before any mobile release.
- **Packaging native code** for five ecosystems is tedious and breaks in small ways.
  Budget time for it; automate from the first release.
- **Flutter desktop maturity** is good but uneven on Linux. Qt is the fallback.
- **Maintenance load.** Three bindings and a GUI multiply release work. The generic
  command channel keeps this manageable: Layer 3 helpers can trail the core.
- **Open:** decision D-20, Flutter or Qt.
- **Open:** whether to provide a Go or Node.js binding. Not planned; the guide in 6.1
  is intended to make community bindings practical.

## Definition of done

- All seven steps meet their acceptance criteria.
- Python, Rust and C# packages are published and pass the conformance scenarios.
- The core builds and passes tests on Android and iOS.
- The reference GUI runs on at least Linux, Windows and Android.
- The interoperability matrix is green.
- Milestone M6.
