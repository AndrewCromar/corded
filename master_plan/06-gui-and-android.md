# The Graphical Client, Android First

Status: proposed. Nothing in this document is built yet. It records how the graphical
client will be made, in what order, and what has to change underneath it.

The owner's direction (D-38): Flutter for the reference GUI, the community free to build
their own clients, the browser client set aside. New on 2026-10-09: the owner wants it
running on an Android phone soon, so Android moves ahead of the desktop GUI in the order
of work (D-42).

Contents

1. [Shape](#1-shape)
2. [What the core must gain first](#2-what-the-core-must-gain-first)
3. [Building the core for Android](#3-building-the-core-for-android)
4. [The Dart binding](#4-the-dart-binding)
5. [The app](#5-the-app)
6. [What a phone changes](#6-what-a-phone-changes)
7. [Other people's clients](#7-other-peoples-clients)
8. [Milestones](#8-milestones)
9. [Risks](#9-risks)
10. [Open questions for the owner](#10-open-questions-for-the-owner)

## 1. Shape

```
+--------------------------------------------------+
|  Flutter app (Dart)            frontends/flutter/app
|  screens, navigation, theme                       |
+--------------------------------------------------+
|  corded_dart (Dart package)    frontends/flutter/packages/corded_dart
|  typed commands and events over dart:ffi          |
+--------------------------------------------------+
|  libcorded (C++)  via  include/corded/corded.h    |
|  identity, vault, encryption, connections, sync   |
+--------------------------------------------------+
```

The rule that already holds for the terminal client holds here: the frontend draws and
asks; the core decides and remembers. The app keeps no message store of its own, does no
cryptography, and opens no sockets. Everything goes through the same JSON commands and
events the terminal client uses, so the two clients cannot drift apart in behaviour.

The terminal client stays. It is the fastest way to try a core change and it is what the
automated tests drive.

## 2. What the core must gain first

The terminal client does a few things itself that every frontend would otherwise have to
copy. They move into the core before the GUI starts, so the GUI is thin from day one.

| Gap | Today | Change |
|---|---|---|
| Unread counts | Counted in the terminal client, lost on restart | The core derives them from the person's own read marker (now stored, since read receipts were built) and reports `unread` on every room |
| Paging | `fetch_timeline` returns the newest N | Add `before` (a message id) so a phone can load a screenful and fetch more on scroll |
| Typing expiry | Each frontend runs its own five-second timer | The core emits `typing` with `active: true/false` |
| Display data per message | Sender name is resolved when the event is emitted | Also carry `sender_user_id` everywhere and emit `member_updated` when a display name changes, so open screens update |
| Interface version | None | `corded_abi_version()` and a `"api"` number in `status`, so a client can refuse a core it does not understand |
| Unlock without typing the passphrase | Passphrase only | See section 6: a second way to open the vault with a key held by the phone's secure storage |
| Being told the network changed | The core notices a dead connection by timeout | A `network_changed` command so the app can ask for an immediate reconnect when the phone moves between Wi-Fi and mobile data |

None of these change the wire protocol or the server.

## 3. Building the core for Android

- **Toolchain.** The Android NDK's CMake toolchain file with the existing `CMakeLists.txt`,
  as a new preset `android-arm64` (phones) and `android-x64` (the emulator). Static C++
  runtime, one output: `libcorded.so`.
- **Dependencies.** vcpkg already provides the others for Android through its
  `arm64-android` and `x64-android` triplets (OpenSSL, asio, FlatBuffers, nlohmann-json,
  spdlog). The encrypted SQLite is a single source file we already compile ourselves.
  libsodium is built by our own bundled step, which needs the cross-compile host flag
  added. `flatc` runs on the build machine, as it already does for the Windows build.
- **Not built for Android:** the server, the terminal client, the tests that start a
  server. Only `corded_core`.
- **Where it is built.** In GitHub's build system first, which already has the NDK
  installed, so this needs nothing on the owner's PC. The result is published as a build
  artifact and, at a release, as `libcorded-android-arm64.zip` (library plus header).
- **How the app gets it.** The app's Android project picks up prebuilt
  `jniLibs/<abi>/libcorded.so`. Building C++ from inside Gradle is avoided on purpose: it
  would drag vcpkg into every app build.
- **Check.** The unit crypto tests are compiled for `android-x64` and run on an emulator
  in the build system, so a broken Android build fails there and not on a phone.

## 4. The Dart binding

`corded_dart` is a plain Dart package (no Flutter dependency), so it can be tested on a
desktop without a phone or an emulator.

- **Loading.** `DynamicLibrary.open` on `libcorded.so` (Android, Linux), `corded.dll`
  (Windows), the process itself on iOS and macOS.
- **Events.** One background isolate loops on `corded_next_event` with a short timeout
  and posts each JSON string to the main isolate, which exposes a `Stream<CordedEvent>`.
  The engine is created on the main isolate and its address is handed to the worker; the
  interface is documented as callable from any thread, which this relies on.
- **Commands.** `Future<Map> command(Map)` sends the JSON, keeps the request id, and
  completes when the matching `command_result` arrives. Typed wrappers sit on top
  (`sendText`, `markRead`, `createChannel`, and so on).
- **Models.** Small immutable classes for server, room, member, message; a `RoomStore`
  that applies events to them. This store is the part other Flutter clients would reuse.
- **Test.** A Dart test starts `cordedd`, creates two vaults, and exchanges a message.
  It runs on Linux in the build system on every change.

## 5. The app

One codebase, two layouts chosen by width.

- **Phone:** a stack. Servers and chats list, then a chat, then details. A drawer for
  switching server.
- **Desktop and tablet:** three columns, as in the terminal client: servers, chats,
  conversation, with members on the right when asked for.

Screens, in the order they will be built:

1. Welcome: create an identity (name, passphrase) or set up as an existing person with a
   recovery key.
2. Unlock.
3. Add a server: paste or scan a `corded://` link, or type an address; confirm the
   server key on first contact.
4. Chats: channels, groups and direct messages of the selected server, with unread
   counts.
5. Conversation: messages, composer, typing notice, read markers. Long-press (right-click
   on desktop) a message for reply, react, edit, delete, start a thread, copy.
6. Thread view.
7. Members: the list; for those permitted, roles, display names, kick, ban, remove.
8. Settings: personal (display name, read receipts, sharing earlier messages, recovery
   key, lock) and, for those permitted, the server's settings, status, restart, invites.
9. Verification: safety numbers, as digits and as a code to scan.

State management is kept plain: the binding's stores are `ChangeNotifier`s and screens
listen to them. No code generation, no large framework, so a newcomer can read it.

Android specifics: `corded://` links open the app; the app asks for no permissions beyond
network access until the camera is needed for scanning a code; minimum Android 9, the
first version whose system library has everything the core's dependencies call.

## 6. What a phone changes

Three things a desktop never had to care about. Each has a cheap first answer and a
proper later one.

**Staying connected.** Android stops an app's network use soon after it leaves the screen.
- First: the app is connected while it is open and catches up when reopened. The core
  already reconnects and syncs what was missed. No notifications while closed.
- Later, a choice for the owner: a foreground service (a permanent notification keeps
  the connection alive; works on any phone with no Google involvement; costs battery),
  or wake-up pushes (the server asks a push service to wake the app, with no message
  content in the push; needs a small addition to the server and either Google's service
  or a self-hosted UnifiedPush distributor).

**Unlocking.** Typing a long passphrase on every launch is not acceptable on a phone.
- First: passphrase on each cold start, as now.
- Later: the vault key is additionally wrapped by a key held in the Android Keystore and
  released by fingerprint or screen lock. The passphrase still works and is still what
  protects a copied vault file.

**Reaching the server.** A phone on mobile data cannot reach a server whose scope is
`machine` or `network`. Testing on the same Wi-Fi works with `network` scope. Using it
away from that Wi-Fi needs the server reachable from the internet, which is the hosting
question in issue #3.

## 7. Other people's clients

The owner wants the community to be able to make their own clients, in whatever toolkit
suits a platform. Three layers make that practical, from least to most effort:

1. **Restyle the reference app.** Colours, fonts, density and layout come from one theme
   object; a fork changes that and nothing else.
2. **Reuse `corded_dart`.** A different Flutter app on the same binding and stores.
3. **Any language on the C interface.** `corded.h` plus a prebuilt `libcorded` for each
   platform are published with every release, so a client in Swift, Kotlin, C#, Rust or
   Python needs no C++ build. The terminal client is the worked example.

What this asks of the project: the command and event list becomes a documented, versioned
contract (`docs/frontend-guide.md`, generated from the list in `corded.h`), changes to it
are additive within a version, and a client can ask which version it is talking to.

## 8. Milestones

| | Milestone | Done when | Needs from the owner |
|---|---|---|---|
| G0 | Core gaps from section 2 | Unread counts, paging and the version number exist, tested, used by the terminal client | nothing |
| G1 | Core builds for Android | The build system produces `libcorded.so` for arm64 and x64 and runs the crypto tests on an emulator | nothing |
| G2 | Dart binding | The two-client Dart test passes on Linux in the build system | nothing |
| G3 | App on the Linux desktop | Create or unlock, add a server, list chats, send and receive text | Flutter installed on the PC, or accept a slower loop through the build system only |
| G4 | **App on Android** | The same app as an installable APK attached to a release; the owner chats from the phone with the PC | A phone on the same Wi-Fi as the server, or a server on the internet |
| G5 | Catch up with the terminal client | Replies, reactions, edits, deletions, threads, members, settings, several servers, second device, scanning codes | nothing |
| G6 | Phone behaviour | Fingerprint unlock; notifications by the route chosen in section 6 | the choice in section 6 |
| G7 | Windows desktop, then macOS and iOS | Installers in releases | Apple fee only for iOS and signed macOS |

G4 is the first point at which there is something to hold. G0 to G2 need nothing from the
owner and can start at once.

This replaces the order in step C14 (desktop GUI, then Android). The Linux desktop build
in G3 exists because it is the quickest way to develop the app, not as a product.

## 9. Risks

- **OpenSSL for Android through vcpkg** is the dependency most likely to cost time. If it
  does, the fallback is a prebuilt OpenSSL for Android fetched by hash.
- **APK signing.** Android only installs an update over an app signed with the same key.
  Debug-signed builds are fine for the first tests; before anyone else installs it there
  must be one stable signing key, which is the same "who holds the key" question as
  release signing (issue #5).
- **Library size.** OpenSSL, libsodium and SQLite together should land near 5 to 8 MB per
  architecture, which is acceptable.
- **Isolate and thread behaviour** of the binding is new ground; the Dart test in G2
  exists to find problems there before any screen is drawn.

## 10. Open questions for the owner

1. **May Flutter, the Android SDK and a Java runtime be installed on the PC?** About 8 GB,
   all inside the home directory, removable by deleting two folders. Without it, every
   app build goes through GitHub's build system: workable, but each try takes minutes.
2. **Which phone and Android version?** Decides the minimum version to support and
   whether an arm64-only APK is enough.
3. **Notifications when the app is closed:** foreground service, push wake-ups, or leave
   it for later? Recommended: leave it until the app is otherwise usable.
4. **APK signing key:** same answer as issue #5, or a separate decision?
