# Stage 5: C ABI and TUI

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Previous: [Stage 4](stage-4-cryptography.md) | Next: [Stage 6](stage-6-bindings-and-guis.md)

> **Changed by decision D-27.** Each server is now one Discord-style community with
> channels, an owner and roles. Where this document talks about rooms, per-room roles or
> server administrators, read [../04-community-model.md](../04-community-model.md), which
> takes precedence. Section 7 there lists what changes in this stage.

## Goal

Give the core its public face, and prove it. Design and freeze a C interface
(`include/corded/corded.h`) that any language can bind, then build a terminal client,
`corded-tui`, that uses nothing else. If the TUI needs to reach around the header for
anything, the header is wrong.

This stage also delivers the first features beyond plain text, replies and reactions, as
a test that the event model and the ABI really are extensible.

## Scope

In scope: the C header, the shim that implements it, event delivery, versioning and ABI
checks, a C conformance suite, the TUI, stress and leak testing, packaging of the shared
library and TUI.

Out of scope: bindings for other languages (Stage 6), graphical clients (Stage 6).

## Prerequisites

Stages 3 and 4. Blueprint source: section 5, roadmap Phase 4.

## Design

### How the blueprint's API changes

The blueprint's header is a good sketch with real problems (gaps G-07, G-08, G-09, G-18).

| Blueprint | Problem | This plan |
|---|---|---|
| `engine_unlock_vault(ctx, const char* passphrase)` | NUL-terminated secret, cannot be zeroed reliably, no length | Pointer plus length; copied into locked memory immediately |
| `engine_send_message(ctx, channel_id, plaintext)` | Only plain text can ever be sent | Generic `corded_send_event` with type, content and relation; text is a helper |
| `engine_connect_server(ctx, url, auth_key)` | Bearer key; returns a second opaque handle with unclear ownership | `corded_server_add` from an invite link; servers referenced by id |
| `int` return, meaning unspecified | No error model | `corded_status` enum on every call, plus per-request results as events |
| One callback, `int event_type` | No threading contract, no alternative for runtimes that cannot take foreign-thread callbacks | Callback **or** pull, documented threading, string event names |
| No version | Cannot evolve | `corded_abi_version()`, size-prefixed structs, symbol versioning |
| Prefix `engine_`, macro `EXPORT` | Collides with other libraries | Prefix `corded_`, macro `CORDED_API` |

### ABI rules

1. Plain C99 in the header. No C++ types, no `bool`, no bitfields, no enums in struct
   fields or parameters (fixed-width integers instead), no `long`.
2. Opaque handles only. Callers never see a struct layout except small, size-prefixed
   option structs.
3. Every option struct begins with `uint32_t struct_size`, so fields can be appended.
4. Every function returns `corded_status` except pure getters and destroy functions.
5. Strings in are UTF-8 with explicit length where they may contain secrets or NULs, and
   NUL-terminated otherwise. Strings out are owned by the library and valid only for the
   duration stated in the documentation (the callback, or until the matching free call).
6. No function blocks on the network. Anything slow is asynchronous: it returns a request
   id, and the result arrives as an event.
7. All functions are thread-safe unless documented otherwise. None may be called from
   inside the event callback except those explicitly marked re-entrant.
8. No exception or C++ unwind ever crosses the boundary.
9. Functions are only added, never removed or changed, within a major ABI version.

### Header sketch

Illustrative, to fix the shape. The real header is written in step 5.2.

```c
#define CORDED_ABI_VERSION_MAJOR 1
#define CORDED_ABI_VERSION_MINOR 0

typedef struct corded_engine corded_engine;
typedef int32_t  corded_status;      /* 0 = CORDED_OK, negatives are errors */
typedef uint64_t corded_request;     /* 0 = none */

typedef void (*corded_event_cb)(const char* event_json, size_t len, void* user_data);

typedef struct corded_config {
    uint32_t    struct_size;
    const char* vault_path;
    const char* cache_dir;           /* optional */
    const char* log_path;            /* optional */
    int32_t     log_level;
    uint32_t    event_queue_capacity;
} corded_config;

/* Library */
CORDED_API uint32_t      corded_abi_version(void);
CORDED_API const char*   corded_version_string(void);
CORDED_API const char*   corded_status_message(corded_status status);

/* Lifecycle */
CORDED_API corded_status corded_engine_create(const corded_config* cfg, corded_engine** out);
CORDED_API void          corded_engine_destroy(corded_engine* e);

/* Events: use the callback, or pull, not both */
CORDED_API corded_status corded_set_event_callback(corded_engine* e, corded_event_cb cb, void* user_data);
CORDED_API corded_status corded_next_event(corded_engine* e, int32_t timeout_ms,
                                           const char** out_json, size_t* out_len);
CORDED_API void          corded_event_free(corded_engine* e, const char* json);
CORDED_API int           corded_event_fd(corded_engine* e);   /* readable when events wait; -1 if unsupported */

/* Vault */
CORDED_API corded_status corded_vault_exists(corded_engine* e, int32_t* out_exists);
CORDED_API corded_status corded_vault_create(corded_engine* e, const uint8_t* pass, size_t pass_len,
                                             const char* display_name, corded_request* out_req);
CORDED_API corded_status corded_vault_unlock(corded_engine* e, const uint8_t* pass, size_t pass_len,
                                             corded_request* out_req);
CORDED_API corded_status corded_vault_lock(corded_engine* e);
CORDED_API corded_status corded_vault_change_passphrase(corded_engine* e,
                              const uint8_t* old_pass, size_t old_len,
                              const uint8_t* new_pass, size_t new_len, corded_request* out_req);

/* Servers */
CORDED_API corded_status corded_server_add(corded_engine* e, const char* invite_link, corded_request* out_req);
CORDED_API corded_status corded_server_remove(corded_engine* e, int64_t server_id, corded_request* out_req);
CORDED_API corded_status corded_network_changed(corded_engine* e);

/* Generic command channel: every operation is reachable through this */
CORDED_API corded_status corded_command(corded_engine* e, const char* command_json, size_t len,
                                        corded_request* out_req);

/* Convenience wrappers over corded_command for the commonest operations */
CORDED_API corded_status corded_send_event(corded_engine* e, int64_t server_id, const char* room_id,
                              const char* type, const char* content_json,
                              const char* relation_kind, const char* relation_target, /* may be NULL */
                              corded_request* out_req);
CORDED_API corded_status corded_send_text(corded_engine* e, int64_t server_id, const char* room_id,
                              const char* body, corded_request* out_req);
CORDED_API corded_status corded_fetch_timeline(corded_engine* e, int64_t server_id, const char* room_id,
                              uint64_t before_seq, uint32_t limit, corded_request* out_req);
```

### Why a generic `corded_command`

Every engine command is a JSON object `{ "cmd": "...", ... }` accepted by
`corded_command`. The typed functions are thin wrappers around it. This gives two things:

- **Features do not need new symbols.** A thread query, a poll vote or a pin is a new
  command name or event type, usable by every existing binding the day it ships.
- **Bindings stay small.** A binding needs about ten functions to be complete, and can
  add typed helpers at its own pace.

The command and event vocabulary is documented in a machine-readable schema
(`docs/abi/commands.json`, `docs/abi/events.json`) generated from the Stage 3 C++
definitions, so header, engine and documentation cannot drift apart.

### Event shape

```json
{ "event": "event_received", "seq": 1412,
  "server_id": 1, "room_id": "…",
  "data": { "event_id": "…", "type": "m.text", "sender_user": "…",
            "origin_ts": 1760000000000, "content": { "body": "hi" },
            "relation": { "kind": "reply", "target": "…" },
            "known_type": true, "status": "ok" } }
```

Events are named by string, not by integer, so new ones cannot collide and unknown ones
are easy to ignore. A `command_result` event carries the `request` id it answers.

### TUI architecture

```
 corded-tui (C++20, FTXUI)
   main thread: FTXUI event loop, owns all UI state
   bridge: corded_event_fd / poll thread -> posts into FTXUI's loop
   store: in-memory view model (rooms, timelines, drafts), fed only by events
   views: Unlock, Onboarding, ServerList, RoomList, Timeline, Composer,
          Thread, Members, Verification, Settings
```

The TUI includes exactly one Corded header, `corded/corded.h`, and links only the shared
library. A CI check enforces that it compiles with no other Corded include path.

## Steps

### 5.1 ABI design rules

Tasks:
- Write `docs/abi/design.md` with the rules above, the ownership and lifetime of every
  kind of pointer, the threading contract, and the error model.
- Define the `corded_status` codes: `OK`, `INVALID_ARGUMENT`, `INVALID_STATE`,
  `VAULT_LOCKED`, `WRONG_PASSPHRASE`, `NOT_FOUND`, `NO_EVENT` (pull timeout),
  `BUSY`, `UNSUPPORTED`, `INTERNAL`. Asynchronous failures use string error codes inside
  `command_result`, mapped from the protocol error enum.
- Decide the compatibility policy: major version changes only for breaking changes; minor
  for additions; the library can report both.

Acceptance: the design document is reviewed against the blueprint's binding list
(Python ctypes/cffi, Rust bindgen, C# P/Invoke, Dart FFI) to confirm every rule is
expressible in each.

### 5.2 The public header `corded.h`

Tasks:
- Write the header in full, with a documentation comment on every function stating
  thread-safety, blocking behaviour, ownership, and possible status codes.
- Cover: library info, lifecycle, events, vault, servers, the generic command, the
  convenience wrappers (send event, send text, fetch timeline, fetch thread, mark read,
  create room, invite, accept, leave), verification (safety number, mark verified).
- `CORDED_API` export macro (generated in Stage 0), `extern "C"` guards, include guard.
- The header compiles as C99, C11, C17, and as C++11 through C++23, with
  `-Wall -Wextra -pedantic`. A CI job does exactly that.
- Generate reference documentation from the comments (Doxygen) into `docs/abi/reference/`.

Acceptance: the compile matrix passes; the header contains no type whose size differs
between the supported platforms other than pointers and `size_t`.

### 5.3 ABI shim and error handling

Tasks:
- `core/src/abi/`: one translation unit per header section. Each function validates
  arguments, translates to a Stage 3 `Command`, and returns. No engine logic lives here.
- A single wrapper applied to every exported function body that catches all C++
  exceptions and converts them to `CORDED_ERR_INTERNAL`, logging the detail.
- Argument validation is strict and uniform: NULL where not allowed, invalid UTF-8,
  lengths over limits, calls in the wrong lifecycle state.
- Passphrase handling: copy into `SecureBytes` in the first line; never log; never keep
  the caller's pointer.
- Handle validity: a magic value in the engine object checked on entry, so a stale or
  wild pointer is more likely to produce an error than a crash (best effort, documented
  as such).
- `corded_command` JSON parsing with depth and size limits; unknown commands return a
  `command_result` error rather than failing the call.
- Lock-order and re-entrancy rules: calling a non-re-entrant function from inside the
  callback returns `CORDED_ERR_BUSY` rather than deadlocking.

Acceptance: every exported function has a test for each documented error status; a fuzz
target feeds arbitrary bytes to `corded_command`.

### 5.4 Event delivery: callback and pull

Resolves gap G-18.

Tasks:
- Callback mode: a dedicated dispatch thread drains the engine's event queue and invokes
  the callback. One callback at a time, in order. The JSON pointer is valid only during
  the call.
- Pull mode: `corded_next_event` blocks up to the timeout; the returned string is owned
  by the caller until `corded_event_free`. `corded_event_fd` exposes a readable file
  descriptor (eventfd, pipe, or a socket pair on Windows) for integration with `poll`,
  `epoll`, libuv, asyncio and GUI main loops.
- The two modes are mutually exclusive; setting a callback after pulling (or the
  reverse) returns `INVALID_STATE`.
- Shutdown semantics: `corded_engine_destroy` guarantees no callback is running or will
  run after it returns; calling it from inside the callback is detected and refused.
- Backpressure as designed in Stage 3: durable events are never dropped; when the queue
  is full a single `resync_required` event tells the frontend to re-query; ephemeral
  events are coalesced.
- A monotonically increasing `seq` on every delivered event so a frontend can detect
  loss.

Acceptance: a stress test with a slow consumer shows bounded memory and a correct
`resync_required`; destroying the engine while events are flowing never calls back
afterwards (checked under TSan).

### 5.5 Symbol visibility, versioning and ABI checks

Tasks:
- Confirm only `corded_*` symbols are exported on all three platforms (`nm`, `dumpbin`,
  `nm -gU`), as a CI check with an allow-list file `abi/exports.txt`.
- ELF symbol versioning with a linker version script (`CORDED_1.0`); `SOVERSION 1`;
  macOS `compatibility_version`; a Windows `.def` file.
- ABI compatibility job: build the library, dump its ABI with `abidiff` / `abi-dumper`
  against the stored baseline in `abi/baseline/`, fail on any incompatible change.
- Static-link the C++ runtime into the shared library where the platform allows
  (`-static-libstdc++ -static-libgcc`), so frontends in other languages do not need a
  matching libstdc++. Verify no C++ runtime symbols leak.
- A runtime check helper for bindings: `corded_abi_version()` compared with the version
  the binding was built for.

Acceptance: adding a parameter to an exported function fails CI; the library loads into
a process that already has a different libstdc++ loaded.

### 5.6 C conformance test suite

Tasks:
- `tests/abi/`: tests written in plain C that drive the library exactly as a foreign
  binding would, against a real `cordedd`.
- Scenario files (a simple declarative format: steps and expected events) so the same
  scenarios can be replayed by each language binding in Stage 6.
- Scenarios: create vault, unlock, wrong passphrase, add server, create room, invite and
  accept, send and receive text, reply, reaction, edit, unknown event type passthrough,
  offline send, history paging, lock and unlock, verification, both event modes.
- Misuse tests: every function with NULL, with a destroyed handle (where safely
  testable), from many threads at once, from inside the callback.

Acceptance: the suite passes on all platforms under ASan and TSan; the scenario runner is
documented for reuse.

### 5.7 TUI architecture

Tasks:
- Project skeleton in `frontends/tui/` linking `corded` and `ftxui` only.
- Event bridge: a thread waiting on `corded_event_fd`, parsing JSON and posting typed
  UI messages into FTXUI's event loop.
- View model: plain structs updated only by those messages; views are pure functions of
  the view model. This keeps the TUI honest as a reference for other frontend authors.
- Command helper wrapping request ids into callbacks or futures.
- Configuration file (`~/.config/corded/tui.toml`): vault path, theme, key bindings,
  notification settings.
- Keybinding map with a help overlay; mouse support optional.
- Terminal handling: resize, 256 and true colour detection, Unicode width for emoji and
  East Asian text, clean restore on crash or signal.

Acceptance: the skeleton starts, shows connection state from a live engine, and exits
cleanly, with no Corded include other than `corded.h`.

### 5.8 TUI screens

Tasks, one view each:
- **Onboarding.** Create a vault (passphrase twice, strength hint, warning that it cannot
  be recovered), choose a display name.
- **Unlock.** Passphrase prompt with masked input; input buffer zeroed after use.
- **Servers.** Add from an invite link, list with connection state, remove.
- **Room list.** Rooms grouped by server, unread and mention counts, sorted by activity,
  create room, accept invite.
- **Timeline.** Scrollback with lazy history loading, day separators, sender colouring,
  send status (pending, sent, failed with retry), undecryptable and unknown-type
  placeholders, gap markers.
- **Composer.** Multi-line input, draft per room, paste handling, slash commands
  (`/invite`, `/leave`, `/verify`, `/me`).
- **Members.** List with role and trust state; invite, kick.
- **Verification.** Show safety number, mark verified; prominent warning banner when a
  contact's identity key changes.
- **Settings.** Change passphrase, lock now, auto-lock timeout, theme, about.
- Desktop notification hook (terminal bell and an optional command to run).

Acceptance: milestone M5 basics: two people on different machines hold a conversation
through a self-hosted server using only the TUI.

### 5.9 Extensibility proof: replies and reactions

Feature milestone F1, done here on purpose.

Tasks:
- Replies: select a message, reply; render a quoted preview; jump to the original;
  handle a missing original (fetch-around, or a placeholder).
- Reactions: add and remove on a selected message; render aggregated counts; show who
  reacted.
- Do both using only `corded_send_event` and the generic event stream. Record every
  place the header or the engine had to change to make this possible. The target is
  zero changes; any that were needed are design defects to fix before the ABI freezes.
- Write `docs/developer/adding-a-feature.md` from the experience: the recipe future
  feature milestones follow.
- Add conformance scenarios for both.

Acceptance: replies and reactions work between two TUI clients; an older build of the
TUI (from step 5.8, before this step) in the same room shows fallback text for reactions
and plain messages for replies, and does not misbehave.

### 5.10 Stress, leak and isolation testing

Blueprint Phase 4, third bullet.

Tasks:
- Soak: a scripted TUI-less C client sending and receiving for 24 hours; memory, file
  descriptors and thread count must be flat.
- Churn: create, unlock, use and destroy engines in a loop; valgrind or LeakSanitizer
  reports nothing.
- Concurrency: many threads calling every thread-safe function at random; TSan clean.
- Callback abuse: slow callbacks, callbacks that call back in, callbacks that throw (from
  a C++ host) and are contained by the host.
- Key isolation: a test that dumps the process's exported symbols and the strings
  reachable through the ABI and confirms no function returns private key material; a
  review that no command exposes it.
- Memory scan (best effort): after `corded_vault_lock`, scan the process heap for the
  passphrase and known key bytes.
- Fault injection: disk full, read-only vault directory, network loss, server restart,
  clock jumps. The engine reports errors and recovers.
- Fuzz `corded_command` and the event JSON a frontend would parse.

Acceptance: all of the above pass and are scheduled in the nightly workflow.

### 5.11 Packaging the library and TUI

Tasks:
- `cmake --install` layout: library, header, `corded.pc` for pkg-config, a CMake package
  config (`find_package(corded)`).
- Release artefacts per platform: a tarball or zip with library, header, licence, and
  the TUI binary. Built by a release workflow on tag.
- An Arch `PKGBUILD` and a Homebrew formula as first distribution packages (more in
  Stage 7).
- A `corded-tui --self-test` command for bug reports (versions, ABI version, terminal
  capabilities; never any vault content).

Acceptance: a C program outside the repository builds against an installed package using
only pkg-config or `find_package`; tagged builds produce artefacts on all platforms.

## Test plan

Summarised from the steps: header compile matrix; per-function error tests; the C
conformance suite and reusable scenarios; ABI diff in CI; TUI view-model unit tests and a
scripted pseudo-terminal test for the main flows; soak, churn, concurrency and fault
injection runs nightly.

## Risks and open questions

- **Freezing the ABI too early.** Mitigation: the generic command channel absorbs most
  change without new symbols; the ABI is version 1.0 only at the end of this stage,
  after the extensibility proof.
- **JSON at the boundary** costs allocation and parsing per event. Fine for chat volumes;
  timeline pages are batched into one event. A binary alternative can be added as a new
  function if profiling ever demands it.
- **Foreign-thread callbacks** are awkward in Python, C# and Dart. The pull mode and the
  file descriptor exist for exactly that reason; Stage 6 validates them.
- **Windows** has no eventfd; the socket-pair fallback needs testing with real GUI loops.
- **Terminal diversity.** Emoji width and colour support vary. Reactions make this
  visible early.
- **Open:** whether `corded-tui` should support several vaults (accounts) in one session.

## Definition of done

- All eleven steps meet their acceptance criteria.
- `corded.h` is frozen at ABI 1.0 with a baseline stored and enforced.
- `corded-tui` builds against the installed header and library only.
- Replies and reactions shipped with no ABI or protocol change after the freeze
  candidate.
- Gaps G-07, G-08, G-09 (API side) and G-18 are marked resolved.
- Milestone M5.
