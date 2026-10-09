# Corded: Master Plan Overview

This is the top of the plan. It says what Corded is, how the pieces fit, what is locked,
and how the work is staged. The other documents hang off it:

| Document | Purpose |
|---|---|
| [01-roadmap.md](01-roadmap.md) | Every stage broken into numbered steps, with dependencies and milestones |
| [02-decisions.md](02-decisions.md) | Decisions made, and gaps or contradictions found in the blueprint |
| [03-feature-roadmap.md](03-feature-roadmap.md) | Chat features (threads, replies, reactions, ...) mapped onto the design |
| [04-community-model.md](04-community-model.md) | Each server is one Discord-style community: channels, an owner, roles and permissions. Includes the change plan. **Overrides older text where they differ** |
| [05-operations-and-platforms.md](05-operations-and-platforms.md) | Running a server remotely (settings, maintenance, updates), one person on several devices, which platforms clients can run on, a browser client |
| [stages/](stages/) | One detailed plan per stage |
| [Blueprint PDF](Modular%20E2EE%20Messaging%20Platform%20-%20Technical%20Architecture%20Blueprint.pdf) | The original technical blueprint this plan is built from |

## 1. What Corded is

Corded is an end-to-end-encrypted messaging platform built as three separable parts:

1. **`libcorded`, the core engine.** A headless C++20 library that owns networking, the
   wire protocol, all cryptography, and an encrypted local vault. It exposes a C ABI and
   nothing else.
2. **`cordedd`, the server daemon.** One running server is one community, in the way a
   Discord server is: it has channels, an owner, and roles with permissions. Someone who
   wants two communities runs two servers. The server authenticates devices, enforces
   roles and permissions, and stores and forwards ciphertext it cannot read. See
   [04-community-model.md](04-community-model.md).
3. **Frontends.** Terminal, desktop, mobile and bot applications that load `libcorded` and
   turn its event stream into an interface. The first is `corded-tui`.

The guiding rule from the blueprint is separation of mechanism and policy. The core decides
how messages are secured and delivered. Frontends decide how they look and feel.

### Goals

- Anyone can self-host a server on very small hardware (Raspberry Pi 4/5, $3 VPS, NAS).
- Whoever sets up a server owns it. The owner has every permission and hands out the
  rest through roles. A person can be a member of many servers from one client.
- The server learns as little as possible, and that amount is written down honestly.
- A frontend author never handles keys, sockets or ratchets.
- Rich chat features (threads, replies, reactions, edits, attachments and more) can be
  added later without changing the wire framing, the server, or the C ABI.
- Builds are reproducible across Linux (x86_64, ARM64), macOS and Windows.

### Non-goals for 1.0

- Federation between servers. A client can be a member of many servers; servers do not
  talk to each other, and direct messages stay within one server.
- Voice and video calls.
- Anonymity against a global network observer. Corded hides content, not the fact that a
  device is talking to a server.
- Server-side search, link unfurling or any feature that needs the server to see plaintext.

## 2. Architecture

```
 +--------------------------------------------------------------+
 |  Frontends ("distros")                                       |
 |  corded-tui (FTXUI)   Python   Rust   C#   GUI   bots        |
 +---------------------------+----------------------------------+
                             | C ABI: include/corded/corded.h
                             | commands in, events out (JSON)
 +---------------------------v----------------------------------+
 |  libcorded (core engine, C++20)                              |
 |                                                              |
 |  C-ABI shim -> command queue -> engine loop -> event queue   |
 |                                                              |
 |  +-----------+  +------------+  +-----------+  +----------+  |
 |  | Sync and  |  | Event      |  | Crypto    |  | Vault    |  |
 |  | outbox    |  | store and  |  | X3DH, DR, |  | SQLCipher|  |
 |  |           |  | relations  |  | group     |  | Argon2id |  |
 |  +-----------+  +------------+  +-----------+  +----------+  |
 |  +--------------------------------------------------------+  |
 |  | Connection manager: asio, TLS 1.3, frame codec         |  |
 |  +--------------------------+-----------------------------+  |
 +-----------------------------|--------------------------------+
                               | length-prefixed FlatBuffers frames
                               | over TLS 1.3
 +-----------------------------v--------------------------------+
 |  cordedd (server daemon, single static binary)               |
 |  listener -> auth -> dispatcher -> router                    |
 |  prekey directory | rooms and ACLs | mailbox | blob store    |
 |  SQLite (WAL)                                                |
 +--------------------------------------------------------------+
```

### Trust boundaries

| Boundary | What crosses it | What must never cross it |
|---|---|---|
| Frontend to core (C ABI) | Commands, decrypted events, the vault passphrase (once, as bytes) | Private keys, ratchet state, raw sockets |
| Core to disk | SQLCipher-encrypted pages | Any plaintext or key outside the vault |
| Core to server (TLS) | Public keys, signed prekeys, ciphertext, routing data | Plaintext content, private keys |
| Server to disk | Public keys, membership, ciphertext, timestamps | Anything that would decrypt content |

### What the server can see

Stated plainly, because the blueprint's "no metadata beyond routing tokens" is stronger
than what the first version delivers (see [02-decisions.md](02-decisions.md), G-04):

- Which device is connected, from which IP address, and when.
- Which rooms exist, who is a member of each, and room names if the room is not
  configured to encrypt its name.
- Who sent an event to which room, when, and how big the padded ciphertext was.
- Never: message content, event type, relations (who replied to what), reactions,
  attachment contents or file names.

Reducing the first three is future work (sealed sender, padding policies) and is tracked
in the feature roadmap.

## 3. The event model in one page

Everything a user does is an **event**. This is the single most important design choice
for future features, and it replaces the blueprint's "a message is a string" model.

```
Event (plaintext, inside the encrypted payload)
  event_id         128-bit, client generated, time sortable
  type             string, e.g. "m.text", "m.reaction", "m.edit"
  type_version     integer
  sender           user id and device id
  origin_ts        client timestamp, milliseconds
  relation         optional: { kind, target_event_id }
  content          bytes, JSON in protocol version 1
```

- A text message is an event of type `m.text` with no relation.
- A reply is an `m.text` with relation `reply`. A thread message has relation `thread`.
- A reaction is `m.reaction` with relation `annotation`. An edit is `m.edit` with relation
  `replace`. A deletion is `m.redaction` with relation `redact`.
- A client that does not know a type or relation kind stores it, shows a fallback, and
  keeps working.

The server sees none of this. It sees a room id, a sender device, a sequence number it
assigns, and padded ciphertext. The full specification is the main output of
[Stage 1](stages/stage-1-protocol-spec.md), and
[03-feature-roadmap.md](03-feature-roadmap.md) checks each planned feature against it.

## 4. Locked technology choices

| Area | Choice | Notes |
|---|---|---|
| Language | C++20 | Coroutines, concepts, `std::jthread`, `<format>` |
| Compilers | GCC 13+, Clang 17+, MSVC 19.38+ | Lowest versions with solid coroutine support |
| Build | CMake 3.25+, presets, Ninja / MSVC | Target-based, no global flags |
| Packages | vcpkg, manifest mode, pinned baseline | Static triplets for the server |
| Networking | standalone asio with C++20 coroutines | |
| Transport security | TLS 1.3 (OpenSSL via asio) plus signed challenge auth | Pinning for self-signed servers |
| Crypto | libsodium | X25519, Ed25519, XChaCha20-Poly1305, HKDF-SHA256, Argon2id |
| Wire format | Length-prefixed FlatBuffers frames | Verified on every untrusted read |
| Event content | JSON inside the encrypted payload | Human-debuggable, easy for bindings |
| Server storage | SQLite 3, WAL mode | Thin hand-written statement layer |
| Client storage | SQLCipher, key wrapped by Argon2id | |
| Logging | spdlog | Never logs secrets or plaintext |
| Queues | moodycamel concurrentqueue | Command queue and event queue |
| Tests | Catch2, CTest, libFuzzer | ASan, UBSan, TSan in CI |
| TUI | FTXUI | |
| License | Apache-2.0 | |
| Group crypto | Megolm-style sender keys | MLS considered and deferred |

The reasoning for each is in [02-decisions.md](02-decisions.md).

## 5. Target repository layout

Created in Stage 0. Nothing below exists yet except `master_plan/`.

```
corded/
  CMakeLists.txt  CMakePresets.json  vcpkg.json  vcpkg-configuration.json
  cmake/                 toolchain helpers, warnings, sanitizers
  proto/                 FlatBuffers schemas (*.fbs), the wire contract
  include/corded/        public C header: corded.h
  core/                  libcorded
    src/net/             connection manager, frame codec
    src/crypto/          primitives, X3DH, double ratchet, group sessions
    src/vault/           SQLCipher store, migrations
    src/engine/          engine loop, sync, outbox, event pipeline
    src/abi/             C-ABI shim
  server/                cordedd
    src/                 listener, auth, dispatcher, router, storage
    migrations/          numbered SQL files
  common/                code shared by core and server (framing, ids, logging)
  frontends/tui/         corded-tui
  bindings/              python/  rust/  dotnet/
  tools/                 corded-cli (headless test client), load generator
  tests/                 unit/  integration/  fuzz/  vectors/  load/
  docs/                  protocol/  crypto/  operator/  developer/
  master_plan/           this plan
```

## 6. Stages

| Stage | Name | Outcome | Depends on |
|---|---|---|---|
| 0 | [Foundation](stages/stage-0-foundation.md) | Empty targets build, test and lint on all platforms in CI | none |
| 1 | [Protocol specification](stages/stage-1-protocol-spec.md) | Written wire protocol, event model and threat model; `.fbs` schemas | 0 |
| 2 | [Server daemon](stages/stage-2-server-daemon.md) | `cordedd` routes and stores ciphertext, under 30 MB idle | 1 |
| 3 | [Client engine](stages/stage-3-client-engine.md) | `libcorded` connects, syncs and persists events with a test-only crypto provider | 1, 2 |
| 4 | [Cryptography](stages/stage-4-cryptography.md) | Real X3DH, Double Ratchet and group sessions wired into the engine | 0, 1 (integration needs 3) |
| 5 | [C ABI and TUI](stages/stage-5-c-abi-and-tui.md) | Stable `corded.h` and a usable terminal client | 3, 4 |
| 6 | [Bindings and GUIs](stages/stage-6-bindings-and-guis.md) | Python, Rust and C# wrappers, one reference GUI | 5 |
| 7 | [Hardening and release](stages/stage-7-hardening-and-release.md) | Benchmarks, fuzzing, audit, packaging, 1.0 | 5 (some parts 6) |

```
 0 --> 1 --> 2 --> 3 --+--> 5 --> 6 --> 7
        \              |
         +--> 4 -------+
```

Stage 4's library work (primitives through group sessions) depends only on Stages 0 and 1,
so it can proceed alongside Stages 2 and 3. Chat features beyond plain text are built as
separate milestones after Stage 5; see [03-feature-roadmap.md](03-feature-roadmap.md).

The blueprint listed five phases. This plan adds a foundation stage and a protocol
specification stage in front, and splits the blueprint's last phase into bindings and
hardening. The mapping is in [01-roadmap.md](01-roadmap.md).

## 7. Quality bars that apply to every stage

- **No stage is done without tests.** Unit tests for logic, integration tests for anything
  that crosses a process or disk boundary.
- **Sanitizers.** All tests run under ASan and UBSan in CI. Threaded code also runs under TSan.
- **Untrusted input is fuzzed.** Every parser that reads bytes from the network or from
  disk has a libFuzzer target before the stage that introduces it closes.
- **Crypto is checked against known answers.** Primitives against published vectors,
  protocols against a second independent implementation.
- **Secrets hygiene.** Key material lives in locked, zeroed-on-free memory. Logs never
  contain keys, passphrases or plaintext. This is enforced by types, not by convention.
- **Budgets from the blueprint are tested, not assumed.** Server idle memory under 30 MB,
  static single binary, runs on ARM64.
- **Warnings are errors** in CI on all three compilers.
- **The wire protocol and the C ABI are versioned from their first commit.**

## 8. Conventions

- Names: library `libcorded`, server `cordedd`, TUI `corded-tui`, C prefix `corded_`,
  C++ namespace `corded::`.
- Steps are numbered `stage.step` (for example 2.8) and the number is stable; use it in
  commit messages and issues.
- A step is done when its acceptance criteria in the stage file are met and CI is green.
- Any change to a locked decision is made by editing [02-decisions.md](02-decisions.md)
  first, in the same pull request as the change.
