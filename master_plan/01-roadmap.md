# Roadmap: Stages and Steps

Each stage below lists its goal, what it delivers, when it can start, when it is done, and
its numbered steps. Step numbers link to the detailed plan in [stages/](stages/).

Sizes are rough, for one developer working with focus: **S** about a week, **M** two to
four weeks, **L** one to two months, **XL** longer. They are for ordering and scoping, not
commitments.

## How the blueprint phases map to stages

| Blueprint phase | This plan |
|---|---|
| (none) | Stage 0 Foundation |
| (none) | Stage 1 Protocol specification |
| Phase 1: Core Networking and Server Foundation | Stage 2 Server daemon |
| Phase 2: Client Core Engine Architecture | Stage 3 Client engine |
| Phase 3: Cryptography Engine Integration | Stage 4 Cryptography |
| Phase 4: C-ABI Standardization and TUI Validation | Stage 5 C ABI and TUI |
| Phase 5: Language Bindings and GUI Ecosystem | Stage 6 Bindings and GUIs, Stage 7 Hardening and release |

## Dependency graph

```
 Stage 0  Foundation
    |
 Stage 1  Protocol spec
    |  \
    |   \---------------------------+
 Stage 2  Server daemon             |
    |                            Stage 4 (steps 4.1 to 4.9)
 Stage 3  Client engine             |  crypto library, no engine needed
    |                               |
    +---------------+---------------+
                    |
          Stage 4 (steps 4.10, 4.11)  integrate crypto into engine
                    |
                 Stage 5  C ABI and TUI   ---> feature milestones F1..F6
                    |
                 Stage 6  Bindings and GUIs
                    |
                 Stage 7  Hardening and release (7.1, 7.2 can start after Stage 5)
```

## Milestones

Points where something can be demonstrated end to end.

| Milestone | After | What you can show |
|---|---|---|
| M0 Green pipeline | Stage 0 | A commit builds and tests on Linux x86_64, Linux ARM64, macOS and Windows |
| M1 Protocol v1 draft | Stage 1 | A reviewable spec and compiled schemas |
| M2 Blind relay | Stage 2 | Two scripted clients exchange opaque payloads through `cordedd` on a Raspberry Pi, including while one is offline |
| M3 Headless client | Stage 3 | `corded-cli` logs in, joins a room, sends and receives events, survives restart and reconnect (test-only crypto) |
| M4 Real encryption | Stage 4 | The same flow, end-to-end encrypted, with the server database containing only ciphertext |
| M5 First usable Corded | Stage 5 | Two people chat in `corded-tui` with replies and reactions |
| M6 Ecosystem | Stage 6 | A Python bot, a Rust example and a GUI client in the same room |
| M7 1.0 | Stage 7 | Audited, benchmarked, packaged release |

---

## Stage 0: Foundation

**Goal.** A repository where adding code is routine: one command configures, builds, tests
and lints on every supported platform.
**Size.** M. **Depends on.** Nothing.
**Detailed plan.** [stage-0-foundation.md](stages/stage-0-foundation.md)

| Step | Title |
|---|---|
| 0.1 | Repository skeleton and conventions |
| 0.2 | CMake project, presets and targets |
| 0.3 | vcpkg manifest, baseline and triplets |
| 0.4 | Compiler warnings, formatting, static analysis, sanitizers |
| 0.5 | Test and fuzz harness |
| 0.6 | Continuous integration matrix and caching |
| 0.7 | Developer documentation |

**Exit criteria.** Placeholder `libcorded`, `cordedd` and `corded-tui` targets build with
all dependencies linked, one trivial test and one trivial fuzz target run, CI is green on
all four platforms, and a new contributor can build from the README alone.

## Stage 1: Protocol specification

**Goal.** Write the protocol down before writing the code that speaks it: threat model,
identities, framing, authentication, the event model, rooms, sync and versioning.
**Size.** M. **Depends on.** Stage 0 (schemas compile in CI).
**Detailed plan.** [stage-1-protocol-spec.md](stages/stage-1-protocol-spec.md)

| Step | Title |
|---|---|
| 1.1 | Threat model |
| 1.2 | Identities, devices and identifiers |
| 1.3 | Transport and framing |
| 1.4 | Authentication handshake and session lifecycle |
| 1.5 | Frame catalogue and error codes |
| 1.6 | Event model: types, relations, compatibility rules |
| 1.7 | Encrypted envelope |
| 1.8 | Rooms, membership, ordering and sync |
| 1.9 | Ephemeral events and encrypted blobs |
| 1.10 | Versioning, capability negotiation and schema files |

**Exit criteria.** `docs/protocol/` is complete and internally consistent, `proto/*.fbs`
compiles, every feature in [03-feature-roadmap.md](03-feature-roadmap.md) is shown to fit
the event model, and every gap assigned to Stage 1 in
[02-decisions.md](02-decisions.md) is closed.

## Stage 2: Server daemon

**Goal.** `cordedd`: a single static binary that authenticates devices, serves prekeys,
enforces membership, and stores and forwards ciphertext within the blueprint's footprint.
**Size.** L. **Depends on.** Stage 1.
**Detailed plan.** [stage-2-server-daemon.md](stages/stage-2-server-daemon.md)

| Step | Title |
|---|---|
| 2.1 | Process skeleton: configuration, logging, signals |
| 2.2 | Listener, TLS and connection lifecycle |
| 2.3 | Frame codec and dispatcher |
| 2.4 | Storage layer and migrations |
| 2.5 | Registration and authentication |
| 2.6 | Prekey directory |
| 2.7 | Rooms, memberships, invites and access control |
| 2.8 | Room event routing and store-and-forward |
| 2.9 | Per-device inbox and ephemeral routing |
| 2.10 | Encrypted blob store |
| 2.11 | Rate limits, quotas and abuse controls |
| 2.12 | Administration and operations |
| 2.13 | Static build, footprint and load testing |

**Exit criteria.** Milestone M2. Idle memory under 30 MB on ARM64, the frame parser has
been fuzzed, and the load test targets in the stage plan are met.

## Stage 3: Client engine

**Goal.** `libcorded` as a C++ library: engine loop, encrypted vault, connection state
machine, sync, outbox and the event store, proven against a real `cordedd`.
**Size.** L. **Depends on.** Stages 1 and 2.
**Detailed plan.** [stage-3-client-engine.md](stages/stage-3-client-engine.md)

| Step | Title |
|---|---|
| 3.1 | Engine skeleton and threading model |
| 3.2 | Command and event queues |
| 3.3 | Vault: SQLCipher, key hierarchy, unlock |
| 3.4 | Vault schema: the event store |
| 3.5 | Connection manager and state machine |
| 3.6 | Sync engine |
| 3.7 | Outbox and send pipeline |
| 3.8 | Event processing: relations and aggregation |
| 3.9 | Crypto provider interface and test provider |
| 3.10 | Internal C++ API and `corded-cli` |
| 3.11 | Integration test suite |

**Exit criteria.** Milestone M3. The engine survives kill and restart at any point without
losing or duplicating events, and reply, reaction and edit events round-trip through the
event store even though no frontend renders them yet.

## Stage 4: Cryptography

**Goal.** Real end-to-end encryption: X3DH, Double Ratchet, sender-key group sessions, key
verification, all tested against known answers and wired into the engine.
**Size.** L. **Depends on.** Stages 0 and 1 for steps 4.1 to 4.9; Stage 3 for 4.10 and 4.11.
**Detailed plan.** [stage-4-cryptography.md](stages/stage-4-cryptography.md)

| Step | Title |
|---|---|
| 4.1 | Primitive wrappers and secure memory |
| 4.2 | Key types, identity and device keys |
| 4.3 | Prekeys and X3DH |
| 4.4 | Double Ratchet |
| 4.5 | Message envelope and padding |
| 4.6 | Group sessions (sender keys) |
| 4.7 | Key distribution and rotation policy |
| 4.8 | Session persistence |
| 4.9 | Identity verification and trust state |
| 4.10 | Engine integration |
| 4.11 | Vectors, property tests, fuzzing and the crypto specification |

**Exit criteria.** Milestone M4. The test-only crypto provider cannot be compiled into a
release build, and the crypto specification in `docs/crypto/` matches the code.

## Stage 5: C ABI and TUI

**Goal.** A stable, documented C interface and a terminal client that proves a frontend
can be built on it without touching anything else.
**Size.** L. **Depends on.** Stages 3 and 4.
**Detailed plan.** [stage-5-c-abi-and-tui.md](stages/stage-5-c-abi-and-tui.md)

| Step | Title |
|---|---|
| 5.1 | ABI design rules |
| 5.2 | The public header `corded.h` |
| 5.3 | ABI shim and error handling |
| 5.4 | Event delivery: callback and pull |
| 5.5 | Symbol visibility, versioning and ABI checks |
| 5.6 | C conformance test suite |
| 5.7 | TUI architecture |
| 5.8 | TUI screens |
| 5.9 | Extensibility proof: replies and reactions |
| 5.10 | Stress, leak and isolation testing |
| 5.11 | Packaging the library and TUI |

**Exit criteria.** Milestone M5. `corded-tui` links only against `corded.h`, and the ABI
is frozen at version 1 with an automated compatibility check in CI.

## Stage 6: Bindings and GUIs

**Goal.** Make the core usable from Python, Rust and C#, and ship one graphical client.
**Size.** L. **Depends on.** Stage 5.
**Detailed plan.** [stage-6-bindings-and-guis.md](stages/stage-6-bindings-and-guis.md)

| Step | Title |
|---|---|
| 6.1 | Binding conventions and shared event schema |
| 6.2 | Python binding |
| 6.3 | Rust binding |
| 6.4 | C# / .NET binding |
| 6.5 | Mobile builds of the core |
| 6.6 | Reference GUI client |
| 6.7 | Examples, bot kit and binding documentation |

**Exit criteria.** Milestone M6. Each binding passes the same conformance scenarios as the
C suite and is published as a package.

## Stage 7: Hardening and release

**Goal.** Turn a working system into one that can be trusted and installed: measured,
fuzzed, audited, reproducible, packaged, documented.
**Size.** XL (dominated by audit lead time). **Depends on.** Stage 5; packaging of bindings
needs Stage 6.
**Detailed plan.** [stage-7-hardening-and-release.md](stages/stage-7-hardening-and-release.md)

| Step | Title |
|---|---|
| 7.1 | Benchmarks and performance budgets |
| 7.2 | Continuous fuzzing |
| 7.3 | Internal security review |
| 7.4 | External audit |
| 7.5 | Reproducible builds, signing and SBOM |
| 7.6 | Release packaging |
| 7.7 | Operator and user documentation |
| 7.8 | Protocol freeze and 1.0 |

**Exit criteria.** Milestone M7. All high and critical audit findings are fixed, the
README's beta warning is replaced, and protocol version 1 is frozen.

---

## Community model change plan

Decision D-27 makes each server one Discord-style community. The steps to change the
existing code are C1 to C10 in [04-community-model.md](04-community-model.md), section 6:

| Step | Change |
|---|---|
| C1 | Owner and roles on the server |
| C2 | Channels |
| C3 | Channel permission overrides (private and read-only channels) |
| C4 | Client and TUI for running a community |
| C5 | Several servers per vault |
| C6 | Invites in the protocol |
| C7 | Moderation (deleting others' messages, pins) |
| C8 | Sender keys for channels |
| C9 | History for newcomers (depends on D-28) |
| C10 | Categories, topics, nicknames, role colours, role mentions, ownership transfer |

These cut across Stages 1 to 5; section 7 of that document says what moves in each stage.

## Feature milestones

Chat features beyond plain text are built one at a time after Stage 5, in parallel with
Stages 6 and 7. Each is small because the event model already carries it. Full detail in
[03-feature-roadmap.md](03-feature-roadmap.md).

| Milestone | Features |
|---|---|
| F1 | Replies and reactions (delivered inside Stage 5 as the extensibility proof) |
| F2 | Edits, deletions, mentions |
| F3 | Threads |
| F4 | Attachments and media |
| F5 | Typing indicators, read receipts, presence |
| F6 | Pins, polls, disappearing messages |
| F7 | Multi-device linking and encrypted backup |
