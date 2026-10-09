# Stage 2: Server Daemon

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Previous: [Stage 1](stage-1-protocol-spec.md) | Next: [Stage 3](stage-3-client-engine.md)

## Goal

Build `cordedd`: a single static binary that authenticates devices, serves prekeys,
enforces room membership, and stores and forwards ciphertext it cannot read. It must run
comfortably on a Raspberry Pi 4 and idle under 30 MB of memory.

## Scope

In scope: everything the server does in protocol version 1, including the ephemeral
routing and blob store that no client uses until the feature milestones.

Out of scope: any cryptography beyond verifying Ed25519 signatures and terminating TLS;
federation; a web admin interface; push notifications to mobile platforms.

## Prerequisites

Stage 1 complete (specification and schemas). Blueprint source: section 3, section 6
(server schema), roadmap Phase 1.

## Design

### Process model

One process. One asio `io_context` running on a small fixed pool (default: 2 threads, 1
on single-core hosts). Each connection is a coroutine chain on its own strand. All
database access goes through one dedicated writer thread and a small pool of reader
connections, which suits SQLite's single-writer design and keeps I/O threads from
blocking on disk.

```
 accept loop --> Connection (coroutine, per device)
                    |  read frame -> verify -> dispatch
                    v
              Dispatcher --> handlers (auth, keys, rooms, events, sync, blobs)
                    |                   |
                    v                   v
            SessionRegistry        Storage facade
         (device_id -> connection)   writer thread (1) + readers (N)
                    ^                   |
                    +---- Router <------+
                  (fan-out to online members)
```

### Memory budget

| Item | Budget |
|---|---|
| Binary and static data | about 6 MB |
| SQLite page cache | 4 MB (configurable) |
| Per idle connection | under 16 KB (buffers allocated on demand) |
| Logging, queues, misc | 2 MB |
| Idle total, 100 connections | under 20 MB, leaving headroom below 30 MB |

Per-connection read buffers start small and grow only for large frames, then shrink.

### Database schema

Replaces the blueprint's three tables, which could not support prekeys, membership,
invites or delivery tracking (gap G-02). All ids are stored as BLOBs. Timestamps are
integer milliseconds.

```sql
CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);

CREATE TABLE users (
    user_id        BLOB PRIMARY KEY,            -- Ed25519 identity public key
    username       TEXT NOT NULL,
    created_at     INTEGER NOT NULL,
    access_level   INTEGER NOT NULL DEFAULT 0,  -- 0 user, 1 server admin, -1 banned
    device_list    BLOB NOT NULL,               -- signed device list, opaque
    device_list_version INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE devices (
    device_id      BLOB PRIMARY KEY,            -- Ed25519 device signing public key
    user_id        BLOB NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    dh_key         BLOB NOT NULL,               -- X25519 device DH public key
    created_at     INTEGER NOT NULL,
    last_seen_at   INTEGER
) WITHOUT ROWID;
CREATE INDEX devices_by_user ON devices(user_id);

CREATE TABLE signed_prekeys (
    device_id      BLOB PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    key_id         INTEGER NOT NULL,
    public_key     BLOB NOT NULL,
    signature      BLOB NOT NULL,
    uploaded_at    INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE one_time_prekeys (
    device_id      BLOB NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    key_id         INTEGER NOT NULL,
    public_key     BLOB NOT NULL,
    PRIMARY KEY (device_id, key_id)
) WITHOUT ROWID;

CREATE TABLE rooms (
    room_id        BLOB PRIMARY KEY,
    crypto_mode    INTEGER NOT NULL,            -- 1 pairwise, 2 group
    is_private     INTEGER NOT NULL DEFAULT 1,
    clear_name     TEXT,                        -- NULL when the name is encrypted state
    created_at     INTEGER NOT NULL,
    next_seq       INTEGER NOT NULL DEFAULT 1
) WITHOUT ROWID;

CREATE TABLE memberships (
    room_id        BLOB NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
    user_id        BLOB NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    role           INTEGER NOT NULL,            -- 0 member, 1 admin, 2 owner
    state          INTEGER NOT NULL,            -- 0 invited, 1 joined, 2 left, 3 banned
    joined_seq     INTEGER,                     -- first seq this member may read
    updated_at     INTEGER NOT NULL,
    PRIMARY KEY (room_id, user_id)
) WITHOUT ROWID;
CREATE INDEX memberships_by_user ON memberships(user_id, state);

CREATE TABLE invites (
    token_hash     BLOB PRIMARY KEY,            -- SHA-256 of the token; token never stored
    kind           INTEGER NOT NULL,            -- 0 server registration, 1 room
    room_id        BLOB REFERENCES rooms(room_id) ON DELETE CASCADE,
    created_by     BLOB,
    expires_at     INTEGER,
    uses_left      INTEGER
) WITHOUT ROWID;

CREATE TABLE room_events (
    room_id        BLOB NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
    seq            INTEGER NOT NULL,
    event_id       BLOB NOT NULL,
    sender_device  BLOB NOT NULL,
    server_ts      INTEGER NOT NULL,
    expires_at     INTEGER,
    crypto         INTEGER NOT NULL,
    session_ref    BLOB,
    ciphertext     BLOB NOT NULL,
    PRIMARY KEY (room_id, seq)
) WITHOUT ROWID;
CREATE UNIQUE INDEX room_events_by_id ON room_events(room_id, event_id);
CREATE INDEX room_events_expiry ON room_events(expires_at) WHERE expires_at IS NOT NULL;

CREATE TABLE device_cursors (
    device_id      BLOB NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    room_id        BLOB NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
    acked_seq      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (device_id, room_id)
) WITHOUT ROWID;

CREATE TABLE device_inbox (                     -- to-device messages, deleted on ack
    device_id      BLOB NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    msg_id         INTEGER NOT NULL,
    sender_device  BLOB NOT NULL,
    server_ts      INTEGER NOT NULL,
    payload        BLOB NOT NULL,
    PRIMARY KEY (device_id, msg_id)
) WITHOUT ROWID;

CREATE TABLE blobs (
    blob_id        BLOB PRIMARY KEY,
    room_id        BLOB NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
    uploader       BLOB NOT NULL,
    size           INTEGER NOT NULL,
    complete       INTEGER NOT NULL DEFAULT 0,
    created_at     INTEGER NOT NULL,
    expires_at     INTEGER
) WITHOUT ROWID;
```

Blob bytes are stored as files under `data/blobs/ab/cd/<id>`, not in SQLite, so large
uploads do not bloat the database or the WAL.

Pragmas: `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`,
`busy_timeout=5000`, `cache_size=-4096`, `wal_autocheckpoint=1000`.

### Configuration

A TOML file (`/etc/cordedd/cordedd.toml`), with every key overridable by environment
variable `CORDEDD_*` and by command-line flag.

| Key | Default | Meaning |
|---|---|---|
| `listen` | `0.0.0.0:7443` | Address and port |
| `tls.cert`, `tls.key` | none | PEM paths; `tls.self_signed = true` generates and persists one |
| `data_dir` | `/var/lib/cordedd` | Database and blobs |
| `registration` | `invite` | `open`, `invite` or `closed` |
| `retention_days` | 30 | Room ciphertext retention |
| `max_frame_bytes` | 1048576 | |
| `max_connections` | 1000 | |
| `blob.max_bytes` | 104857600 | Per blob |
| `blob.quota_bytes` | 1073741824 | Per user |
| `threads` | auto | I/O threads |
| `log.level` | `info` | |

## Steps

### 2.1 Process skeleton: configuration, logging, signals

Tasks:
- `main`: parse flags, load configuration, validate, print the effective configuration
  with secrets redacted.
- spdlog setup: stderr sink (systemd journal friendly), optional rotating file sink,
  structured key=value fields. A lint rule or wrapper type that makes it a compile error
  to log a `Ciphertext` or key type.
- Signals: SIGINT and SIGTERM trigger graceful shutdown (stop accepting, send a
  `shutting down` error to clients, flush the database, exit); SIGHUP reloads the log
  level and TLS certificate.
- Subcommands on the same binary: `cordedd run`, `cordedd init` (create data directory,
  generate a self-signed certificate if asked, print the fingerprint and a first admin
  invite link), `cordedd admin ...` (step 2.12), `cordedd version`.
- systemd unit file with sandboxing directives (`DynamicUser`, `ProtectSystem=strict`,
  `StateDirectory`, `NoNewPrivileges`, `MemoryMax`).

Acceptance: `cordedd init && cordedd run` starts, logs its listen address, and exits
cleanly on SIGTERM within two seconds.

### 2.2 Listener, TLS and connection lifecycle

Tasks:
- Accept loop as a coroutine; a connection limit; per-IP connection limit.
- TLS 1.3 context through `asio::ssl`: minimum version pinned, ALPN `corded/1`,
  certificate reload on SIGHUP.
- `Connection` class: owns the stream, a strand, a bounded outbound queue, timers.
- Timeouts: TLS handshake 10 s, authentication 10 s, idle 90 s with ping at 30 s.
- Backpressure: if a connection's outbound queue exceeds its bound, the connection is
  closed; the device will resync from its cursor.
- Graceful close with an `Error` frame carrying a reason.
- Export the TLS exporter value for the authentication signature (step 2.5).

Acceptance: a test client can connect, receive `Hello`, idle, be pinged, and be timed out;
a client that never reads is disconnected rather than growing server memory; 1,000 idle
connections stay inside the memory budget.

### 2.3 Frame codec and dispatcher

Tasks:
- Frame reader in `corded_common` (shared with the client): read the length, reject over
  the limit before allocating, read the body, run the FlatBuffers verifier, then expose
  the typed root.
- Frame writer with buffer reuse.
- Dispatcher: a table from `FrameBody` union type to handler coroutine, with a declared
  minimum connection state for each (pre-auth or authenticated).
- Uniform error path: handlers return a result type; errors become `Error` frames with
  the request id.
- Per-request structured log line: frame type, device, duration, result code. Never the
  payload.
- Fuzz target `corded_fuzz_frame_reader` fed by the Stage 1 vectors as seed corpus.

Acceptance: all Stage 1 frame vectors round-trip; malformed, truncated and oversized
frames produce the specified error and close; the fuzz target runs 10 minutes clean under
ASan.

### 2.4 Storage layer and migrations

Tasks:
- `Database`, `Statement`, `Transaction` RAII wrappers over the SQLite C API (decision
  D-12), with prepared-statement caching and typed bind and column helpers for blobs.
- Migration runner: numbered files in `server/migrations/NNNN_name.sql`, embedded into
  the binary at build time, applied in a transaction, recorded in `schema_migrations`.
  The server refuses to start on a database newer than it understands.
- Migration `0001_initial.sql` with the schema above.
- Writer thread with a work queue; handlers `co_await` a storage call that posts work to
  the writer and resumes on the connection's strand. Reader pool for sync and history.
- Batching: writes arriving within a short window (about 2 ms) are committed in one
  transaction to amortise fsync.
- Periodic tasks: WAL checkpoint, retention sweep (step 2.8), expiry sweep, `PRAGMA
  optimize`.
- Repository classes per area: `UserRepo`, `KeyRepo`, `RoomRepo`, `EventRepo`,
  `InboxRepo`, `BlobRepo`.

Acceptance: repository unit tests run against a temporary database; a migration test
upgrades an empty database to head and checks the schema; a crash test (kill -9 during
writes) reopens without corruption or loss of acknowledged writes.

### 2.5 Registration and authentication

Tasks:
- `Hello`: versions, capabilities, limits, fresh 32-byte challenge.
- `Authenticate` handler: look up the device, rebuild the signed message exactly as in
  the specification (context string, challenge, TLS exporter, server name), verify with
  libsodium, reject banned users, register in the `SessionRegistry`, replace any existing
  connection for that device.
- `Register` handler: enforce the registration mode, consume the invite token (constant
  time compare on the hash, decrement uses atomically), verify the device certificate
  chain to the user identity key, store user, device and device list.
- `PublishDeviceList`: verify the user identity signature and that the version increases.
- `FetchDeviceList`.
- Constant-time comparisons for anything secret-derived; uniform error for "unknown
  device" and "bad signature" to avoid an enumeration oracle.

Acceptance: integration tests for successful registration and login, replayed
`Authenticate`, wrong key, expired and exhausted invite, closed registration, banned
user, and a device list with a bad signature or stale version.

### 2.6 Prekey directory

Tasks:
- `PublishPrekeys`: replace the signed prekey (verify its signature against the device
  signing key), append one-time prekeys up to a cap per device (default 200).
- `FetchPrekeyBundle(user)`: for each device of the user, return the device certificate,
  signed prekey, and one one-time prekey, which is deleted in the same transaction. If
  none remain, return the bundle without one (X3DH allows this).
- `PrekeysLow` push to a device when its count drops below a threshold (default 20).
- Rate limit bundle fetches per requester and per target, since draining someone's
  one-time prekeys is a known abuse.

Acceptance: concurrent fetches never hand out the same one-time prekey twice (tested with
many parallel requests); exhaustion falls back correctly; the low-water push fires.

### 2.7 Rooms, memberships, invites and access control

Tasks:
- `CreateRoom`, `Invite`, `AcceptInvite`, `Leave`, `Kick`, `SetRole`, ban and unban,
  `ListRooms`, `ListMembers`.
- A single `can(actor, action, room, target)` authorisation function with a table-driven
  policy, called by every handler. No handler does its own ad hoc check.
- Room invite links: token generation, hashed storage, expiry, use count.
- `MembershipChanged` pushed to all joined members' online devices, and stored as a
  server-generated entry in the room's sequence so offline devices learn about it in
  order. This is what triggers group key rotation on clients.
- `joined_seq` recorded on join and enforced on sync and history.
- Member cache: an in-memory map from room to member set and their online connections,
  kept consistent with the database, used by the router.

Acceptance: a permission matrix test covering every action for every role and membership
state; a non-member cannot learn that a private room exists (same error as "not found").

### 2.8 Room event routing and store-and-forward

The heart of the server and of blueprint Phase 1.

Tasks:
- `SendRoomEvent` handler:
  1. Check membership and rate limit.
  2. Deduplicate on `(room_id, event_id)`; on a duplicate return the original `seq`.
  3. In one write transaction: assign `seq` from `rooms.next_seq`, insert into
     `room_events`.
  4. Reply `SendOk { seq, server_ts }` only after the transaction commits.
  5. Hand the stored frame to the router.
- Router: for each joined member's online device except the sender's sending device,
  enqueue a `RoomEvent` push. The frame is serialised once and the buffer shared.
- `Ack(room, seq)`: advance `device_cursors`.
- `Sync`: given the client's cursors, stream `SyncBatch` frames from the reader pool,
  respecting `joined_seq`, with flow control (next batch sent when the previous is
  written), then `SyncComplete`. Events that arrive live during sync are queued and
  delivered after, in order.
- `FetchHistory(room, before_seq, limit)`.
- Retention sweep: delete `room_events` older than `retention_days`, or past
  `expires_at`, in small batches so the writer is never held long.
- `DeleteStored`: verify the signature and role, replace the ciphertext with a tombstone
  row so `seq` stays contiguous.

Acceptance:
- Two scripted clients exchange opaque payloads.
- A client offline during 10,000 sends receives all of them, in order, exactly once, on
  reconnect.
- Killing the server at random points never loses an event that was acknowledged with
  `SendOk`, and never delivers a duplicate `seq`.
- A retry of the same `event_id` does not create a second event.

### 2.9 Per-device inbox and ephemeral routing

Tasks:
- `SendToDevice`: store in `device_inbox`, push if online, delete on ack. Caps on inbox
  depth per device and payload size. Batch form for sending to many devices at once (key
  shares to a whole room).
- Allowed-recipient rule: a device may send to-device messages only to devices of users
  it shares a room with or has a pending invite with, to limit spam.
- `SendEphemeral`: membership check, strict rate limit, route to online members, never
  touch the database.
- Inbox items are included in `Sync` before room events, because they may carry the keys
  needed to decrypt those events.

Acceptance: to-device messages survive a server restart until acked; ephemeral frames
never appear in the database (asserted by test); inbox is delivered before room events.

### 2.10 Encrypted blob store

Built now so that attachments later need no server release.

Tasks:
- `BlobBegin(room, size)`: check membership, per-blob limit and user quota; allocate id.
- `BlobChunk`: append to a temporary file; enforce declared size.
- `BlobEnd`: fsync, rename into place, mark complete.
- `BlobGet(blob_id, offset, length)`: membership check on the blob's room; stream with
  flow control.
- Cleanup of incomplete uploads after a timeout; expiry sweep aligned with retention.
- Quota accounting per user.

Acceptance: upload and download of a 100 MB blob keeps server memory flat; interrupted
uploads are cleaned up; a non-member cannot fetch.

### 2.11 Rate limits, quotas and abuse controls

Tasks:
- Token-bucket limiter keyed by device and by IP address, with separate buckets per
  class: connection attempts, authentication failures, sends, ephemeral, prekey fetches,
  room creation, invites, blob bytes.
- Limits configurable; `Error` responses carry a retry-after.
- Buckets live in memory with bounded size and idle eviction.
- Server-level controls: ban user (drops connections, blocks login), delete user, remove
  room, close registration.
- Temporary IP blocks after repeated authentication failures.
- Resource caps: rooms per user, members per room, devices per user, inbox depth.

Acceptance: each limiter has a test that exceeds it and observes the error and recovery;
a flood from one device does not degrade latency for another (checked in load tests).

### 2.12 Administration and operations

Tasks:
- `cordedd admin` subcommands talking to the running daemon over a Unix domain socket
  (named pipe on Windows): `invite create`, `user list|ban|unban|delete`, `room
  list|delete`, `stats`, `fingerprint`.
- Metrics: an optional plain-text metrics endpoint on localhost in Prometheus format
  (connections, frames by type, send latency histogram, database timings, queue depths,
  memory). Off by default.
- Health check command for container orchestration.
- Online backup: `cordedd admin backup <path>` using SQLite's backup API plus a blob
  directory snapshot.
- A minimal Dockerfile (scratch image with the static binary) and a compose example.

Acceptance: an operator can bootstrap a server, invite a user and ban a user without
touching SQL; backup and restore round-trips in a test.

### 2.13 Static build, footprint and load testing

Tasks:
- Finish the `static-server` preset: musl target, static OpenSSL, libsodium, SQLite; LTO;
  section garbage collection; stripped release artefact with separate debug symbols.
- Cross-compile for ARM64 and run on a real Raspberry Pi 4 or 5.
- Load generator `tools/loadgen`: N simulated devices over real TLS with scripted
  behaviours (idle, chatty rooms, reconnect storms, large rooms).
- Measure and record in `docs/operator/performance.md`:

| Scenario | Target on Raspberry Pi 4 |
|---|---|
| Idle memory, 0 connections | under 15 MB |
| Idle memory, 100 connections | under 30 MB |
| Idle memory, 1,000 connections | under 60 MB |
| Sustained room sends, 50 members | 500 events/s with p99 delivery under 50 ms |
| Reconnect storm, 500 devices syncing | completes without errors, memory returns to baseline |
| 24-hour soak | no memory growth, no file descriptor leak |

- Run the whole server test suite under ASan and TSan.

Acceptance: the targets above are met or the gap is explained and accepted in the
decisions file; the binary has no dynamic dependencies; milestone M2 is demonstrated on a
Raspberry Pi.

## Test plan

- **Unit.** Frame codec, authorisation policy, rate limiter, repositories, configuration
  parsing.
- **Integration.** A scripted test client (C++ in `corded_testing`, speaking frames
  directly with real Ed25519 keys) run against a child `cordedd` process, covering every
  frame in the catalogue, including every error code.
- **Property.** Random interleavings of sends, acks, disconnects and reconnects from
  several devices; invariant: each device eventually sees every event it is entitled to,
  exactly once, in `seq` order.
- **Crash.** Kill the server at random points during writes; verify durability rules.
- **Fuzz.** Frame reader; each handler given a verified but adversarial frame.
- **Load.** The scenarios in step 2.13, run nightly on x86_64 and on demand on ARM64.

## Risks and open questions

- **musl allocator performance and fragmentation.** If memory or throughput suffers,
  link mimalloc statically.
- **SQLite single-writer throughput** on SD-card storage. Batching mitigates; the load
  test is run on a Pi with an SD card as the worst case.
- **Static OpenSSL size.** Likely several megabytes. If it threatens the footprint,
  build OpenSSL with unused algorithms disabled, or revisit the Noise alternative in D-07.
- **Large rooms.** Fan-out is O(members) per event on the I/O threads. Acceptable for
  the target scale (hundreds of members); documented as a limit.
- **Open:** whether the admin socket needs authentication beyond filesystem permissions.
- **Open:** Windows service support, or declare Windows a development-only server target.

## Definition of done

- All thirteen steps meet their acceptance criteria.
- Every frame in the Stage 1 catalogue is implemented and covered by an integration test.
- Gaps G-02, G-12 and G-20 are marked resolved.
- Footprint and load targets are recorded with real measurements.
- Milestone M2.
