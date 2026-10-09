# Stage 3: Client Engine

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Previous: [Stage 2](stage-2-server-daemon.md) | Next: [Stage 4](stage-4-cryptography.md)

> **Changed by decision D-27.** Each server is now one Discord-style community with
> channels, an owner and roles. Where this document talks about rooms, per-room roles or
> server administrators, read [../04-community-model.md](../04-community-model.md), which
> takes precedence. Section 7 there lists what changes in this stage.

## Goal

Build the body of `libcorded` as a C++ library: the engine loop, the encrypted vault, the
connection state machine, sync, the outbox and the event store. At the end of this stage
a headless client logs in to a real `cordedd`, joins rooms, sends and receives events,
and survives being killed at any moment.

Real end-to-end encryption arrives in Stage 4. Here the engine talks to a crypto
*interface*, backed by a test-only provider, so engine and crypto can be built and tested
independently.

## Scope

In scope: threading, queues, vault and its schema, networking client, sync, send
pipeline, event processing including relations and state, the internal C++ API, a
developer command-line client.

Out of scope: the C ABI (Stage 5), real cryptography (Stage 4), any user interface.

## Prerequisites

Stage 1 (specification) and Stage 2 (a server to test against). Blueprint source: section
1 (core engine), section 6 (client vault schema), roadmap Phase 2.

## Design

### Threading model

```
 caller threads (frontend)
        |  post Command                         ^  deliver Event
        v                                       |
  [command queue: MPSC] ---> ENGINE THREAD ---> [event queue: SPSC]
                              |        ^
                 strand hop   |        |  completion
                              v        |
                      IO THREAD (asio io_context)
                      sockets, TLS, timers, frame codec
```

- **Engine thread.** One `std::jthread` owns all engine state: vault connection, crypto
  sessions, room and sync state. Nothing else touches them, so engine logic needs no
  locks. It runs coroutines, so a multi-step operation (fetch bundle, establish session,
  encrypt, send) reads as straight-line code.
- **I/O thread.** One thread runs the asio `io_context` for sockets, TLS and timers.
  Decoded frames are handed to the engine thread; outbound frames are handed back.
- **Worker pool (optional, size 1 by default).** For Argon2id at unlock and for blob
  encryption, so neither blocks the engine thread.
- **Dispatch.** Events for the frontend go into the event queue. How they leave it
  (callback thread or pull) is Stage 5's business.

The rule: **all mutable engine state belongs to the engine thread.** It is checked by a
debug assertion in every engine entry point and by TSan in CI.

### Module map

| Module | Responsibility |
|---|---|
| `engine/Engine` | Lifecycle, owns everything, runs the loop |
| `engine/Commands`, `engine/Events` | Typed command and event definitions |
| `vault/Vault` | Open, unlock, key hierarchy, migrations |
| `vault/*Store` | Typed access: `EventStore`, `RoomStore`, `SessionStore`, `OutboxStore`, `ServerStore` |
| `net/Connection` | One TLS connection, frame codec, keepalive |
| `net/ConnectionManager` | State machine per server, reconnect and backoff |
| `engine/Sync` | Cursors, batches, gap detection, history |
| `engine/Outbox` | Durable send queue, retry, deduplication |
| `engine/EventPipeline` | Validate, store, index relations, resolve state, aggregate |
| `crypto/CryptoProvider` | Interface the engine encrypts and decrypts through |

### Vault key hierarchy (decision D-11)

```
 passphrase --Argon2id(salt, params)--> KEK (32 bytes)
 KEK --XChaCha20-Poly1305 unwrap--> vault key (32 random bytes)
 vault key --> SQLCipher raw key  (PRAGMA key = "x'...'")
```

The salt, Argon2id parameters and wrapped vault key live in a small plaintext header file
next to the database (`<vault>.hdr`), since they are needed before the database can be
opened. The header is authenticated: tampering makes the unwrap fail.

### Vault schema

Replaces the blueprint's `servers`, `ratchet_states` and `cached_messages`. The
`cached_messages.decrypted_text` model could not represent anything but plain text
(gap G-09); this is an event store.

```sql
CREATE TABLE meta (key TEXT PRIMARY KEY, value BLOB NOT NULL) WITHOUT ROWID;

CREATE TABLE identity (                       -- exactly one row
    id                INTEGER PRIMARY KEY CHECK (id = 1),
    user_id           BLOB NOT NULL,
    user_sign_secret  BLOB,                   -- NULL on devices that do not hold it
    device_id         BLOB NOT NULL,
    device_sign_secret BLOB NOT NULL,
    device_dh_secret  BLOB NOT NULL,
    device_cert       BLOB NOT NULL,
    created_at        INTEGER NOT NULL
);

CREATE TABLE servers (
    server_id         INTEGER PRIMARY KEY,
    url               TEXT NOT NULL UNIQUE,
    tls_pin           BLOB,                   -- pinned SPKI fingerprint, if any
    nickname          TEXT,
    registered        INTEGER NOT NULL DEFAULT 0,
    capabilities      TEXT,                   -- last seen, JSON
    added_at          INTEGER NOT NULL
);                                            -- no auth_token: auth is by signature (G-06)

CREATE TABLE rooms (
    server_id         INTEGER NOT NULL REFERENCES servers(server_id) ON DELETE CASCADE,
    room_id           BLOB NOT NULL,
    crypto_mode       INTEGER NOT NULL,
    membership        INTEGER NOT NULL,       -- invited, joined, left
    joined_seq        INTEGER,
    acked_seq         INTEGER NOT NULL DEFAULT 0,   -- sync cursor
    oldest_seq        INTEGER,                -- how far back history has been fetched
    read_seq          INTEGER NOT NULL DEFAULT 0,   -- local read marker
    unread_count      INTEGER NOT NULL DEFAULT 0,
    mention_count     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (server_id, room_id)
) WITHOUT ROWID;

CREATE TABLE members (
    server_id INTEGER NOT NULL, room_id BLOB NOT NULL, user_id BLOB NOT NULL,
    role INTEGER NOT NULL, state INTEGER NOT NULL,
    PRIMARY KEY (server_id, room_id, user_id)
) WITHOUT ROWID;

CREATE TABLE events (
    server_id         INTEGER NOT NULL,
    room_id           BLOB NOT NULL,
    event_id          BLOB NOT NULL,
    seq               INTEGER,                -- NULL while only in the outbox
    type              TEXT NOT NULL,
    type_version      INTEGER NOT NULL,
    sender_user       BLOB NOT NULL,
    sender_device     BLOB NOT NULL,
    origin_ts         INTEGER NOT NULL,
    server_ts         INTEGER,
    state_key         TEXT,
    content_encoding  INTEGER NOT NULL,
    content           BLOB,                   -- NULL after redaction
    fallback_text     TEXT,
    expires_at        INTEGER,
    status            INTEGER NOT NULL,       -- ok, pending, failed, undecryptable, redacted
    known_type        INTEGER NOT NULL,       -- 0 if the core has no handler for this type
    PRIMARY KEY (server_id, room_id, event_id)
) WITHOUT ROWID;
CREATE UNIQUE INDEX events_by_seq ON events(server_id, room_id, seq) WHERE seq IS NOT NULL;
CREATE INDEX events_by_type ON events(server_id, room_id, type, seq);
CREATE INDEX events_expiry ON events(expires_at) WHERE expires_at IS NOT NULL;

CREATE TABLE relations (                      -- generic: knows nothing about kinds
    server_id         INTEGER NOT NULL,
    room_id           BLOB NOT NULL,
    target_event_id   BLOB NOT NULL,
    kind              TEXT NOT NULL,
    event_id          BLOB NOT NULL,
    rel_key           TEXT,
    seq               INTEGER,
    PRIMARY KEY (server_id, room_id, target_event_id, kind, event_id)
) WITHOUT ROWID;

CREATE TABLE aggregates (                     -- cached, rebuildable from relations
    server_id INTEGER NOT NULL, room_id BLOB NOT NULL, target_event_id BLOB NOT NULL,
    kind TEXT NOT NULL, rel_key TEXT NOT NULL DEFAULT '',
    count INTEGER NOT NULL, latest_event_id BLOB, latest_seq INTEGER, summary BLOB,
    PRIMARY KEY (server_id, room_id, target_event_id, kind, rel_key)
) WITHOUT ROWID;

CREATE TABLE room_state (                     -- current value per (type, state_key)
    server_id INTEGER NOT NULL, room_id BLOB NOT NULL,
    type TEXT NOT NULL, state_key TEXT NOT NULL,
    event_id BLOB NOT NULL, seq INTEGER NOT NULL,
    PRIMARY KEY (server_id, room_id, type, state_key)
) WITHOUT ROWID;

CREATE TABLE outbox (
    local_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    server_id         INTEGER NOT NULL,
    room_id           BLOB,
    event_id          BLOB,
    kind              INTEGER NOT NULL,       -- room event, to-device, ack, ...
    payload           BLOB NOT NULL,          -- plaintext event or prepared frame
    attempts          INTEGER NOT NULL DEFAULT 0,
    next_attempt_at   INTEGER NOT NULL,
    created_at        INTEGER NOT NULL
);

CREATE TABLE undecryptable (                  -- ciphertext waiting for a key
    server_id INTEGER NOT NULL, room_id BLOB NOT NULL, seq INTEGER NOT NULL,
    session_ref BLOB, frame BLOB NOT NULL, received_at INTEGER NOT NULL,
    PRIMARY KEY (server_id, room_id, seq)
) WITHOUT ROWID;
CREATE INDEX undecryptable_by_session ON undecryptable(session_ref);

-- Tables owned by Stage 4, created by its migrations:
--   peer_devices, pairwise_sessions, skipped_keys, prekeys,
--   outbound_group_sessions, inbound_group_sessions
```

Why this supports later features without migration: a thread view is
`SELECT ... FROM relations WHERE target_event_id = ? AND kind = 'thread'`; a reaction
count is a row in `aggregates`; pins are a row in `room_state`. A new relation kind or
event type adds rows, not columns.

## Steps

### 3.1 Engine skeleton and threading model

Tasks:
- `Engine` class with explicit lifecycle states: `Created`, `Locked` (vault path known,
  not open), `Unlocked`, `ShuttingDown`, `Destroyed`.
- Start the engine thread and the I/O thread with `std::jthread`; stop tokens drive
  shutdown; destruction order is fixed and documented (stop accepting commands, close
  connections, flush outbox state, close vault, zero keys, join threads).
- A minimal coroutine task type (`Task<T>`) and an executor that resumes on the engine
  thread; helper `co_await on_io(...)` and `co_await on_engine()` for hops.
- Engine-thread assertion macro used at the top of every state-touching function.
- Clock and random-source interfaces injected at construction so tests are deterministic.

Acceptance: create and destroy the engine 10,000 times in a loop under ASan and TSan with
no leak or race; destruction completes within a bounded time even with a hung connection.

### 3.2 Command and event queues

Tasks:
- `Command`: a `std::variant` of typed command structs, each with a `request_id` assigned
  on submission.
- `EngineEvent`: a `std::variant` of typed event structs (see the list under step 3.10).
- Command queue: moodycamel `ConcurrentQueue` (multi-producer), woken through an
  eventfd / pipe / asio post so the engine thread sleeps when idle.
- Event queue: moodycamel `ReaderWriterQueue` or the blocking variant, bounded.
- Overflow policy for the event queue: never drop durable events. When the bound is
  reached, the engine emits one `EventsDropped`-style marker telling the frontend to
  re-query state, and coalesces ephemeral events (typing, presence, progress).
- JSON serialisation for every `EngineEvent` with nlohmann-json, in one place, with a
  schema document generated from it (used by Stage 5 and Stage 6).

Acceptance: a stress test with 8 producer threads posting a million commands loses none
and preserves per-producer order; the idle engine uses no CPU.

### 3.3 Vault: SQLCipher, key hierarchy, unlock

Blueprint Phase 2, second bullet.

Tasks:
- `SecureBytes` type (from Stage 4 step 4.1 if already available; otherwise a minimal
  version here, replaced later): `sodium_malloc` backed, zeroed on destruction,
  non-copyable.
- Create flow: generate salt and vault key, derive the KEK with Argon2id, wrap the vault
  key, write the header atomically (write temp, fsync, rename), create the database with
  the raw key, run migrations.
- Unlock flow: read header, derive KEK on the worker thread, unwrap, open, verify with a
  cheap query, run migrations. A wrong passphrase is reported as a distinct error with no
  timing difference beyond Argon2id itself.
- Argon2id parameters: start from libsodium's `MODERATE` limits; store the parameters in
  the header so they can be raised later; a calibration helper for low-memory devices
  with a hard floor.
- Change passphrase: re-wrap only.
- SQLCipher settings: `cipher_page_size`, `kdf_iter` irrelevant in raw-key mode,
  `cipher_memory_security = ON`, `secure_delete = ON`, `journal_mode = WAL`,
  `foreign_keys = ON`.
- Lock: close the database, zero the vault key, return to `Locked`.
- File permissions 0600 on creation; refuse to open a vault that is group or world
  readable on POSIX, with a clear error.
- A single-instance lock file so two processes cannot open the same vault.

Acceptance: the vault file contains no recognisable plaintext (a test greps the raw file
for known strings after writing them); wrong passphrase fails cleanly; a header with one
flipped bit fails cleanly; killing the process mid-create leaves either no vault or a
valid one.

### 3.4 Vault schema: the event store

Tasks:
- Reuse the `Database` / `Statement` / `Transaction` wrappers from Stage 2 (moved to
  `corded_common`, compiled against SQLCipher here).
- Migration runner identical in behaviour to the server's; migrations embedded in the
  library; `0001_initial.sql` with the schema above.
- Store classes with typed methods. Examples on `EventStore`: `insert_remote`,
  `insert_pending`, `confirm_pending(event_id, seq, server_ts)`, `get`, `timeline(room,
  before_seq, limit)`, `related(target, kind, limit)`, `redact(event_id)`,
  `sweep_expired(now)`.
- Every engine operation that changes more than one table runs in one transaction, so a
  crash never leaves an event stored without its relations and cursor update.
- A consistency checker used by tests: rebuild `aggregates` and `room_state` from
  `events` and `relations` and compare with the stored tables.

Acceptance: store unit tests; the consistency checker passes after randomised operation
sequences; a migration test from an empty vault.

### 3.5 Connection manager and state machine

Blueprint Phase 2, third bullet.

Tasks:
- `Connection`: asio TCP connect with happy-eyeballs style address iteration, TLS 1.3
  client handshake, certificate validation or SPKI pin check, the shared frame codec,
  keepalive, request/response correlation by `request_id` with timeouts.
- `ConnectionManager`: one state machine per configured server, implementing the states
  from the specification:

```
 Disconnected -> Connecting -> TlsHandshake -> Authenticating -> Syncing -> Live
       ^              |              |               |              |        |
       +---- Backoff <+--------------+---------------+--------------+--------+
```

- Backoff: exponential with full jitter, 1 s to 5 min, reset after 60 s of stable `Live`.
  Authentication failures that cannot be fixed by retrying (unknown device, banned) stop
  retrying and surface an error.
- Registration path for a server the device has never used (invite token from the link).
- Authentication: build and sign the challenge message through the `CryptoProvider`
  (device signing is real from the start; see step 3.9).
- Network change handling: a hook the frontend can call (`network changed`) to skip the
  backoff timer and retry now.
- Every transition emits a `ConnectionStateChanged` event.
- Version and capability negotiation recorded per server.

Acceptance: tests against a real `cordedd` and against a fault-injecting proxy (drop,
delay, truncate, reset) show the client always returns to `Live` when the network does;
a wrong TLS pin is refused; a server speaking an unknown version is refused with a clear
error.

### 3.6 Sync engine

Tasks:
- On entering `Syncing`: send cursors for all joined rooms; process to-device messages
  first, then room batches; on `SyncComplete` move to `Live`.
- Each batch is processed and its cursor advanced in one vault transaction, then acked.
  Crash safety follows: an unacked batch is simply delivered again, and inserts are
  idempotent on `event_id`.
- Gap detection: a `seq` jump within a room that sync does not fill raises a
  `HistoryGap` event and records the range.
- Room list reconciliation: rooms joined or left on the server while offline.
- `FetchHistory` paging backwards, tracked by `oldest_seq`, bounded by the server's
  retention; "start of available history" is surfaced to the frontend.
- Fetch-around: given an event id that is a relation target but is missing locally,
  fetch the page containing it (needed for replies to old messages).
- Membership pushes update `members` and notify the crypto layer (rotation trigger).

Acceptance: the property test from Stage 2 (random sends, disconnects, kills) run with
real engines instead of scripted clients; invariant: every engine's vault converges to
the same ordered event list per room.

### 3.7 Outbox and send pipeline

Tasks:
- `send_event(room, type, content, relation, ...)`:
  1. Build the `Event`, assign `event_id`.
  2. In one transaction: insert into `events` with status `pending` and into `outbox`.
  3. Emit a local echo event to the frontend immediately.
  4. The outbox worker encrypts (through the `CryptoProvider`), sends `SendRoomEvent`,
     and on `SendOk` confirms the event with its `seq` and removes the outbox row.
- Retry with backoff on transient errors; permanent errors mark the event `failed` and
  tell the frontend, which may retry or discard.
- Strict per-room ordering: one in-flight send per room; later sends wait.
- Idempotent retry: the same `event_id` is resent, and the server deduplicates.
- Own events arriving back through sync are recognised by `event_id` and merged, not
  duplicated.
- Offline sends queue indefinitely and flush on reconnect.
- Encryption happens at send time, not at enqueue time, so membership changes while
  queued are honoured.

Acceptance: sending while offline then reconnecting delivers in order; killing the engine
between any two numbered sub-steps and restarting results in exactly one delivered event;
a permanently rejected event ends in `failed` with no retry loop.

### 3.8 Event processing: relations and aggregation

The step that makes the feature roadmap real inside the core.

Tasks:
- `EventPipeline::process(decrypted event, frame metadata)`:
  1. Validate per the specification (sender matches the cryptographic sender, ids match,
     limits, content parses for known types).
  2. Insert into `events`; set `known_type`.
  3. If it has a relation, insert into `relations` regardless of whether the kind is
     known.
  4. Run the handler for the relation kind, if one is registered.
  5. If it is a state event, apply state resolution into `room_state`.
  6. Update unread and mention counters.
  7. Emit `EventReceived`, plus `EventUpdated` for any target whose aggregate changed.
- Handlers are registered in a table keyed by relation kind and by event type, so adding
  a feature means adding a handler, not editing the pipeline. Initial handlers:
  - `annotation`: maintain `aggregates` count and sender list per `(target, key)`,
    one per sender per key.
  - `replace`: honour only from the original sender; record latest valid edit in
    `aggregates`; keep all versions.
  - `redact`: check permission; null the target's content, set status `redacted`,
    update aggregates if the target was itself a reaction.
  - `reply`, `thread`, `reference`: index only, plus thread summary (count, latest,
    participants) in `aggregates` for `thread`.
- Out-of-order tolerance: a relation whose target has not arrived yet is indexed anyway
  and applied when the target arrives.
- Unknown types and kinds follow the unknown-event rules from specification step 1.6,
  each rule covered by a test that cites its requirement id.
- Expiry sweeper for events with `expires_at`.
- A query layer for frontends: timeline page with aggregates joined in, thread page,
  room list with counters, room state snapshot.

Acceptance: tests feed reply, thread, reaction, edit and redaction events in every order
(including target-after-relation and duplicates) and the consistency checker agrees with
the stored aggregates; an invented type `x.test.unknown` with an invented relation kind
is stored, indexed, delivered marked unknown, and survives restart byte-for-byte.

### 3.9 Crypto provider interface and test provider

Tasks:
- Define the interface the engine uses. It is the contract Stage 4 implements:

```cpp
class CryptoProvider {
public:
    // identity and auth: real from the start (plain Ed25519 via libsodium)
    virtual Identity create_identity() = 0;
    virtual Signature sign_auth_challenge(ByteView message) = 0;

    // key publication
    virtual Task<PrekeyUpload> prekeys_to_publish() = 0;

    // room events
    virtual Task<EncryptResult> encrypt_room_event(RoomRef, ByteView plaintext_event) = 0;
    virtual Task<DecryptResult> decrypt_room_event(RoomRef, const RoomEventFrame&) = 0;

    // to-device traffic produced and consumed by the crypto layer itself
    virtual Task<void> handle_to_device(DeviceId sender, ByteView payload) = 0;

    // membership and device changes
    virtual void on_membership_changed(RoomRef, const MembershipDelta&) = 0;
    virtual void on_device_list_changed(UserId, const DeviceList&) = 0;
};
```

- `EncryptResult` may say "send these to-device messages first" (key shares), which the
  outbox handles generically. `DecryptResult` may say "key not available yet", in which
  case the frame goes to `undecryptable` and is retried when a key arrives.
- `InsecureTestCryptoProvider`: real identity keys and signatures, but room events are
  "encrypted" with a fixed test key and clearly tagged. It exists only when
  `CORDED_ENABLE_INSECURE_TEST_CRYPTO` is on, which release presets force off; it logs a
  loud warning at start-up; the shared library refuses to export when it is linked.
- The undecryptable queue and its retry path, exercised by a test provider mode that
  withholds keys for a while.

Acceptance: the engine compiles and runs with the test provider; a CI job proves a
release build with the test provider enabled fails to configure.

### 3.10 Internal C++ API and `corded-cli`

Tasks:
- A C++ facade `corded::Client` over the command and event queues. This is what the C
  ABI will wrap in Stage 5, so its shape is designed now. Commands: create or open
  vault, unlock, lock, change passphrase, add server (from invite link), remove server,
  list rooms, create room, invite, accept invite, leave, send event (generic), fetch
  timeline page, fetch thread, fetch history, mark read, get room state.
- Engine events: `VaultStateChanged`, `ConnectionStateChanged`, `SyncProgress`,
  `RoomListChanged`, `RoomUpdated`, `EventReceived`, `EventUpdated`, `EventSendStatus`,
  `MembershipChanged`, `HistoryGap`, `CommandResult` (success or error for a
  `request_id`), `Warning`.
- `tools/corded-cli`: a line-oriented developer client that prints events as JSON lines
  and accepts commands on stdin. It doubles as the driver for integration tests and as
  the first bot-like client.

Acceptance: milestone M3 demonstrated with two `corded-cli` processes and a `cordedd`.

### 3.11 Integration test suite

Tasks:
- Test harness that starts a `cordedd` child and N in-process engines with temporary
  vaults, a deterministic clock and scripted network faults.
- Scenarios: first run and registration; restart and unlock; create room, invite,
  accept; text exchange; offline catch-up; history paging; reply, reaction, edit and
  redaction round trips; unknown event round trip; send while offline; concurrent sends
  from three members; kick and its effect on sync; server restart mid-conversation.
- Kill tests: terminate an engine process at random points; reopen; check invariants.
- Memory and thread checks: the full suite under ASan, UBSan and TSan.
- Fuzz targets: vault header parser; event content parser; `Event` decoding from
  decrypted bytes.

Acceptance: the suite is green on all CI platforms and is the regression net for Stages
4 and 5.

## Test plan

Covered per step above. In summary: unit tests for stores, state machine, backoff,
pipeline handlers; property tests for convergence; kill tests for durability; fuzzing for
parsers; sanitizers throughout.

## Risks and open questions

- **Coroutines across two threads** are easy to get subtly wrong (resuming on the wrong
  thread, lifetime of awaited objects). Mitigation: only two hop helpers, the
  engine-thread assertion, TSan, and no detached coroutines without an owning scope.
- **SQLCipher on Windows and mobile** builds can be awkward. Surfaced in Stage 0;
  fallback is building the amalgamation ourselves.
- **Event queue backpressure** with a slow frontend. The coalescing rule in step 3.2 is
  the plan; it must be exercised with a deliberately stalled consumer.
- **Vault growth.** Long-lived rooms accumulate events. A local retention setting and
  `VACUUM` policy are needed before 1.0; tracked in Stage 7.
- **Open:** one vault per identity (proposed) or several identities per vault.
- **Open:** whether full-text search over the encrypted vault (SQLite FTS5 inside
  SQLCipher) belongs in the core. Proposed as a later feature, since the schema allows
  adding an FTS table without touching existing ones.

## Definition of done

- All eleven steps meet their acceptance criteria.
- The vault schema matches this document and passes the consistency checker under
  randomised load.
- Reply, thread, reaction, edit, redaction and unknown-type events are proven through the
  store, though nothing renders them yet.
- Gaps G-06 (client side), G-09 (vault side) and G-10 are marked resolved.
- Milestone M3.
