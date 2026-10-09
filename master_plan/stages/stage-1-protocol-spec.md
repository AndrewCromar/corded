# Stage 1: Protocol Specification

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Previous: [Stage 0](stage-0-foundation.md) | Next: [Stage 2](stage-2-server-daemon.md)

## Goal

Write the protocol down before writing the code that speaks it. The blueprint goes from
architecture straight to sockets; this stage fills the space in between. Its output is a
set of specification documents in `docs/protocol/` and schema files in `proto/` that the
server (Stage 2), the engine (Stage 3) and the crypto layer (Stage 4) all implement.

This stage also owns the requirement that rich chat features fit later without rework.
The event model in step 1.6 is where that is won or lost.

## Scope

In scope: threat model, identifiers, transport, authentication, frame catalogue, event
model, the shape of the encrypted envelope, rooms and sync, ephemeral events, blobs,
versioning.

Out of scope: the internals of X3DH, Double Ratchet and group sessions. This stage fixes
what bytes they exchange and where those bytes sit in a frame; Stage 4 specifies and
implements the algorithms. No server or client code is written here beyond compiling the
schemas.

## Prerequisites

Stage 0 (so that `proto/*.fbs` compiles in CI). Blueprint source: sections 3 and 4.

## Deliverables

```
docs/protocol/
  00-threat-model.md
  01-identities.md
  02-transport.md
  03-authentication.md
  04-frames.md
  05-events.md
  06-envelope.md
  07-rooms-and-sync.md
  08-ephemeral-and-blobs.md
  09-versioning.md
  events/            one file per m.* event type
proto/
  common.fbs  frame.fbs  auth.fbs  keys.fbs  rooms.fbs  sync.fbs  blobs.fbs  event.fbs
```

## Design

The decisions below are the plan's proposals. Writing the specification means turning
each into precise text, finding the cases this outline misses, and recording any change in
[02-decisions.md](../02-decisions.md).

### Identifiers

| Name | Form | Source |
|---|---|---|
| `user_id` | base64url of the 32-byte Ed25519 user identity public key | Client generated |
| `device_id` | base64url of the 32-byte Ed25519 device signing public key | Client generated |
| `room_id` | 16 random bytes, base64url | Server generated |
| `event_id` | 16 bytes: 48-bit millisecond timestamp, 80 random bits; base64url | Sending client |
| `seq` | unsigned 64-bit, per room, strictly increasing | Server assigned |
| `blob_id` | 16 random bytes, base64url | Server generated |
| `request_id` | unsigned 32-bit, per connection | Client chosen |

Usernames are display names, not identifiers. They are unique per server only if the
server operator turns that on.

### Frame layout

```
+-----------------+---------------------------------------------+
| length: u32 LE  | FlatBuffers Frame table (length bytes)      |
+-----------------+---------------------------------------------+

table Frame {
  request_id: uint32;      // 0 for server pushes
  body: FrameBody;         // union of every request, response and push
}
```

Limits: a frame is at most 1 MiB by default (server configurable, advertised at connect).
Larger payloads go through the blob store in chunks.

### Event envelope (plaintext, before encryption)

```
table Event {
  event_id: [ubyte];             // 16 bytes
  type: string;                  // "m.text", "com.example.dice"
  type_version: uint16;
  sender_user: [ubyte];          // 32 bytes
  sender_device: [ubyte];        // 32 bytes
  origin_ts: uint64;             // ms since epoch, sender's clock
  relation: Relation;            // optional
  state_key: string;             // present only on state events
  content_encoding: ubyte;       // 0 = JSON
  content: [ubyte];
  fallback_text: string;         // optional, shown by clients that do not know the type
  expires_at: uint64;            // optional
}

table Relation {
  kind: string;                  // "reply", "thread", "annotation", "replace", "redact", "reference"
  target: [ubyte];               // event_id
  key: string;                   // optional, used by annotations
}
```

### Encrypted room event on the wire (what the server sees)

```
table RoomEventFrame {
  room_id: [ubyte];
  event_id: [ubyte];             // lets the server deduplicate retries
  crypto: ubyte;                 // 1 = pairwise fan-out, 2 = group session
  session_ref: [ubyte];          // opaque to the server
  ciphertext: [ubyte];           // padded
  expires_at: uint64;            // optional hint, see step 1.9
  // added by the server on delivery:
  seq: uint64;
  sender_device: [ubyte];
  server_ts: uint64;
}
```

## Steps

### 1.1 Threat model

Write `00-threat-model.md`.

Tasks:
- List assets: message content, relations and metadata inside events, attachment
  contents, long-term keys, session state, the social graph, the vault at rest.
- List adversaries and what each is assumed able to do:

| Adversary | Capability | Must not learn or do |
|---|---|---|
| Network observer | Reads and modifies traffic | Content, or anything inside TLS |
| Malicious or compromised server | Full control of server and database | Content, event types, relations; must not forge events from a user |
| Removed room member | Has past keys | Events sent after the rotation that follows removal |
| Device thief, device locked | Has the vault file | Anything, without the passphrase |
| Device thief, device unlocked | Has memory | Out of scope, stated explicitly |
| Malicious frontend | Calls the C ABI | Out of scope for confidentiality (it sees plaintext by design); must not be able to extract private keys through the ABI |
| Other user on the server | Normal client | Membership of rooms they are not in; content of those rooms |

- State security goals in testable form: confidentiality, authenticity, forward secrecy,
  post-compromise security (pairwise only), deniability (not a goal for group messages
  because they are signed).
- State what the server does learn. This closes gap G-04 by replacing the blueprint's
  claim with an accurate list (connection times and addresses, membership, who sends to
  which room and when, padded sizes).
- List what a malicious server can still do: drop, delay or reorder events; withhold
  prekeys; add a fake device to a user's device list. For each, say how a client detects
  it (sequence gaps, verification, signed device lists) or that it cannot.
- List known limitations: no sealed sender, no traffic-analysis resistance, cooperative
  deletion, no key recovery.

Acceptance: every later design choice in this stage can cite the adversary it defends
against; limitations are stated in plain words.

### 1.2 Identities, devices and identifiers

Write `01-identities.md`.

Tasks:
- Define the key hierarchy: user identity key (Ed25519), device signing key (Ed25519),
  device DH key (X25519), signed prekey (X25519), one-time prekeys (X25519).
- Define the **device certificate**: the user identity key signs
  `(device signing key, device DH key, created_at, label)`.
- Define the **device list**: the set of certificates for a user, with a version counter,
  signed as a whole by the user identity key so a server cannot add or silently remove
  devices.
- Define every identifier in the table above, with encodings and length checks.
- Define where the user identity private key lives. Proposal: on the first device; moved
  to new devices only by the linking flow (feature milestone F7).
- Specify single-device behaviour for the first releases as the special case of a device
  list with one entry.

Acceptance: a reader can construct and validate a device list by hand from the document.

### 1.3 Transport and framing

Write `02-transport.md`.

Tasks:
- TLS 1.3 only, with the allowed cipher suites and ALPN identifier `corded/1`.
- Certificate validation rules: public CA validation by default; pinned SPKI fingerprint
  when the server address came from an invite link that carries one.
- The frame layout above, the maximum frame size, and what a receiver does with an
  oversized or unverifiable frame (close with an error code, never attempt recovery).
- Keepalive: ping and pong frames, intervals, idle timeout.
- Backpressure: bounded send queues per connection and what happens when one fills.
- Server address format and the invite link format:
  `corded://host[:port]/?fp=<spki fingerprint>&invite=<token>`.

Acceptance: two independent implementers would produce byte-compatible framing.

### 1.4 Authentication handshake and session lifecycle

Write `03-authentication.md`. Closes gaps G-05 and G-06.

Tasks:
- Specify the connect sequence:

```
client                                   server
  | ---- TLS 1.3 handshake -------------> |
  | <--- Hello { protocol versions,       |
  |      capabilities, limits,            |
  |      challenge (32 random bytes) } -- |
  | ---- Authenticate { device_id,        |
  |      user_id, protocol version,       |
  |      capabilities, signature } -----> |
  |      signature = Ed25519(device key,  |
  |        "corded-auth-v1" || challenge  |
  |        || tls_exporter || server name)|
  | <--- AuthOk { server time, device     |
  |      state } or Error --------------- |
```

- Binding the signature to the TLS exporter value prevents a malicious server from
  relaying the challenge to another server.
- Registration: a first-time device sends `Register` with its device list and
  certificate, plus an invite token if the server requires one.
- No passwords and no bearer tokens exist anywhere in the protocol.
- Session rules: one connection per device; a second connection replaces the first.
- Define the client connection states (disconnected, connecting, handshaking,
  authenticating, syncing, live, backing off) that Stage 3 implements.

Acceptance: the sequence resists replay and relay as argued in the document; a state
diagram is included.

### 1.5 Frame catalogue and error codes

Write `04-frames.md`, and the corresponding `.fbs` files.

Tasks:
- Enumerate every frame, grouped by area. Initial set:

| Area | Client to server | Server to client |
|---|---|---|
| Session | `Authenticate`, `Register`, `Ping` | `Hello`, `AuthOk`, `Pong`, `Error` |
| Keys | `PublishDeviceList`, `PublishPrekeys`, `FetchPrekeyBundle`, `FetchDeviceList` | `PrekeyBundle`, `DeviceList`, `PrekeysLow` |
| Rooms | `CreateRoom`, `Invite`, `AcceptInvite`, `Leave`, `Kick`, `SetRole`, `ListRooms`, `ListMembers` | `RoomInfo`, `MemberList`, `MembershipChanged` |
| Events | `SendRoomEvent`, `SendToDevice`, `SendEphemeral`, `Ack`, `DeleteStored` | `RoomEvent`, `ToDevice`, `Ephemeral`, `SendOk` |
| Sync | `Sync`, `FetchHistory` | `SyncBatch`, `HistoryBatch`, `SyncComplete` |
| Blobs | `BlobBegin`, `BlobChunk`, `BlobEnd`, `BlobGet` | `BlobReady`, `BlobData` |

- For each frame: fields, who may send it, preconditions, the response, and the errors it
  can produce.
- Error codes as a stable numeric enum with categories: protocol (malformed, too large,
  unsupported version), auth (bad signature, unknown device, registration closed),
  permission (not a member, insufficient role), resource (rate limited, quota exceeded,
  not found), server (internal, shutting down). Each error says whether a retry makes
  sense and after how long.
- Idempotency: `SendRoomEvent` is deduplicated by `(room_id, event_id)`; retries return
  the original `seq`.

Acceptance: every frame has a schema, and no frame requires the server to read encrypted
content.

### 1.6 Event model: types, relations, compatibility rules

Write `05-events.md` and one file per type under `events/`. Closes gap G-09. This is the
step the [feature roadmap](../03-feature-roadmap.md) depends on.

Tasks:
- Specify the `Event` and `Relation` tables above field by field.
- **Type namespace.** `m.*` reserved for this specification; reverse-domain names for
  everything else. Type names are case-sensitive ASCII.
- **Type versioning.** `type_version` increments only for incompatible content changes;
  adding optional fields does not bump it.
- **Relation kinds.** Define `reply`, `thread`, `annotation`, `replace`, `redact` and
  `reference`, with the validity rules for each (for example `replace` is only honoured
  from the original sender; `redact` from the sender or a moderator).
- **State events.** An event with a `state_key` sets room state for `(type, state_key)`.
  Define conflict resolution: highest server `seq` wins, since the server orders the room.
  Define which roles may send which state types, enforced by clients against
  `m.room.power_levels`.
- **Unknown-event rules.** Clients must store unknown types and unknown relation kinds
  unmodified, must deliver them to the frontend marked as unknown, must not send error
  responses for them, and should display `fallback_text` if present.
- **Validation.** What a receiving client checks before accepting an event: the decrypted
  `sender_user` and `sender_device` match the cryptographic sender, `event_id` matches the
  frame, sizes are within limits, `content` parses.
- **Clock handling.** `origin_ts` is the sender's claim and is used for display only.
  Ordering uses the server `seq`.
- **Initial types for version 1.** Specify fully: `m.text`, `m.reaction`, `m.edit`,
  `m.redaction`, `m.room.name`, `m.room.topic`, `m.room.power_levels`, `m.room.member`
  (profile within a room). Specify to draft level, for later milestones: `m.file`,
  `m.image`, `m.typing`, `m.receipt`, `m.presence`, `m.room.pins`, `m.poll.*`,
  `m.room.retention`, `m.key.share`.
- **Coverage walk-through.** For each feature in the feature roadmap, write the example
  events and confirm no new mechanism is needed. Record the result in section 3 of that
  document.

Acceptance: the coverage walk-through passes for every listed feature; the unknown-event
rules are stated as testable requirements with identifiers that Stage 3 tests can cite.

### 1.7 Encrypted envelope

Write `06-envelope.md`.

Tasks:
- Define the two crypto modes named in `RoomEventFrame.crypto` and what `session_ref`
  holds for each.
- Define the associated data that the AEAD authenticates: protocol version, room id,
  event id, sender device, crypto mode. This stops a server moving ciphertext between
  rooms or relabelling its sender.
- Define padding: plaintext is padded to the next bucket (proposal: 256, 512, 1024, 2048,
  then multiples of 4096 bytes) before encryption.
- Define the `ToDevice` payload types carried over pairwise sessions: session
  establishment (the X3DH initial message), group session key shares, and later device
  linking messages.
- Leave explicit placeholders that Stage 4 fills: header format of a Double Ratchet
  message, format of a group message, format of a prekey bundle.

Acceptance: Stage 4 can define its formats without changing any frame in `04-frames.md`.

### 1.8 Rooms, membership, ordering and sync

Write `07-rooms-and-sync.md`. Closes gap G-17.

Tasks:
- Room model: every conversation is a room, including two-person direct messages.
  Attributes the server holds: id, crypto mode, privacy flag, creation time, optional
  clear-text name.
- Roles the server enforces: owner, admin, member. Finer permissions live in the
  encrypted `m.room.power_levels` state and are enforced by clients.
- Membership changes: invite (to a user id), accept, leave, kick, ban. Invites by link
  token with expiry and use count (the blueprint's "invite token lifetimes").
- Membership changes are announced to members as server-generated `MembershipChanged`
  pushes, which trigger group session rotation on clients.
- Ordering: the server assigns `seq` at acceptance. Clients present events in `seq`
  order. A gap in `seq` that is not filled by sync is surfaced as a warning.
- Sync: a device holds a cursor per room (highest contiguous `seq` acknowledged). On
  connect it sends its cursors; the server replies with batches until caught up, then
  `SyncComplete`, then live pushes.
- History: `FetchHistory(room, before_seq, limit)` pages backwards within the server's
  retention window.
- Acknowledgement: `Ack(room, seq)` advances the cursor; per-device messages are acked by
  id and deleted.
- New-member rule: a member can fetch only events with `seq` at or after their join, and
  could not decrypt earlier ones anyway.

Acceptance: the document covers offline catch-up, a device that was offline longer than
retention, and concurrent sends from two members, each with a worked example.

### 1.9 Ephemeral events and encrypted blobs

Write `08-ephemeral-and-blobs.md`. Closes gaps G-15 and G-21.

Tasks:
- Ephemeral frames: same encryption as room events, routed only to currently connected
  members, never stored, no `seq`, no ack, stricter rate limit.
- Blob protocol: the client encrypts a file with a random key (XChaCha20-Poly1305
  secretstream, chunked), uploads with `BlobBegin` / `BlobChunk` / `BlobEnd`, receives a
  `blob_id`, and puts the id, key and hash inside an event.
- Blob access control: any member of the room the blob was uploaded for may fetch it.
- Blob limits and lifetime: maximum size, per-user quota, expiry aligned with retention.
- Expiry hint: the optional `expires_at` on `RoomEventFrame` lets the server drop
  ciphertext early for disappearing messages. Document what this reveals.
- Early deletion: `DeleteStored(room, event_id)` signed by the sender or a room admin.

Acceptance: an attachment, a typing indicator and a disappearing message are each walked
through end to end.

### 1.10 Versioning, capability negotiation and schema files

Write `09-versioning.md`; finalise `proto/`.

Tasks:
- Protocol version: a single integer. `Hello` lists the versions the server speaks;
  the client picks the highest common one.
- Capabilities: string flags exchanged in `Hello` and `Authenticate` (for example
  `blobs`, `ephemeral`, `expiry`, `history`). A client hides features the server lacks.
- Schema evolution rules for FlatBuffers: fields are only appended, never removed or
  renumbered; deprecated fields stay reserved; unions only grow.
- A compatibility test: old-schema and new-schema buffers checked against each other in
  CI (set up here with a version 1 baseline, used from then on).
- Reserve extension space: a generic `Extension` frame carrying a namespaced type and
  bytes, so server-side experiments do not need a protocol bump.
- Review pass: read all ten documents together for contradictions; check every gap
  assigned to Stage 1 in the decisions file; have at least one other person, or a
  deliberate adversarial re-read on a different day, review the threat model and
  authentication.

Acceptance: `flatc` compiles every schema in CI; a schema-compatibility job exists; the
specification is tagged `protocol-v1-draft`.

## Test plan

A specification stage has few executable tests. What can be checked is checked:

- Schemas compile for C++ in CI, and for Python and Rust as a smoke test of portability.
- Hand-built example frames for each frame type are committed under
  `tests/vectors/frames/` as binary plus JSON, and a test verifies they parse. Stages 2
  and 3 reuse them.
- Example events for every type in step 1.6 are committed under `tests/vectors/events/`.
- A link checker runs over `docs/protocol/`.

## Risks and open questions

- **Over-specifying too early.** The spec will change when code meets it. Mitigation: it
  is tagged draft until the end of Stage 5 and changes freely until then; it freezes in
  Stage 7.
- **Sender visibility.** Keeping `sender_device` visible to the server is a deliberate
  simplification. Sealed sender would change `SendRoomEvent` and the abuse controls. The
  frame leaves room for it but the work is not planned before 1.0.
- **State resolution by server order** trusts the server to order honestly. A malicious
  server could reorder state events. Acceptable under the threat model if documented;
  signed state chains are a possible later hardening.
- **JSON content** costs bytes and parsing time. Judged acceptable; `content_encoding`
  keeps the door open.
- **Open:** whether room names are encrypted by default for public, discoverable rooms,
  which need a clear-text name to be discoverable at all.
- **Open:** whether user names are unique per server.

## Definition of done

- All ten specification documents exist, are consistent and have been reviewed.
- `proto/*.fbs` compiles and has committed example vectors.
- Gaps G-01 (specification part), G-04, G-05, G-06, G-09, G-13, G-15, G-17, G-21 and G-22
  are marked resolved in [02-decisions.md](../02-decisions.md).
- The coverage table in [03-feature-roadmap.md](../03-feature-roadmap.md) has been
  verified against the written spec.
- Milestone M1.
