# Feature Roadmap: Rich Chat Features

Corded is meant to grow well past plain text: threads, replies, reactions and whatever
comes next. None of these are built in the core stages, but the core is designed so each
one is an addition, never a redesign. This document is the check on that claim. If a
feature here cannot be expressed with the mechanisms in section 1, the design is wrong
and Stage 1 must change before code is written.

## 1. The mechanisms features are built from

Specified in [stage-1-protocol-spec.md](stages/stage-1-protocol-spec.md), steps 1.6 to 1.9.

| Mechanism | What it is | Who sees it |
|---|---|---|
| **Event type** | A namespaced string plus a version, e.g. `m.text` v1. Decides how `content` is read. | Clients only |
| **Relation** | An optional `{ kind, target_event_id }` on any event | Clients only |
| **State event** | An event with a `state_key`; the latest one per `(type, state_key)` is the room's current value | Clients only |
| **Ephemeral frame** | Routed to online members, never stored | Server routes it, cannot read it |
| **Encrypted blob** | Opaque bytes uploaded to the server; the key travels inside an event | Server stores ciphertext |
| **Capability flags** | Client and server advertise what they support at connect | Both |
| **Relations index** | A vault table mapping target event to related events, plus cached aggregates | Client only |

Rules that make this safe to extend:

1. **Unknown is not an error.** A client stores events with unknown types or relation
   kinds untouched, shows the event's optional `fallback_text` or a neutral placeholder,
   and never drops or rewrites them.
2. **Relations are generic.** The vault indexes every relation by `(target, kind)` without
   knowing what the kind means. New kinds need no migration.
3. **Types are namespaced.** `m.*` is reserved for the Corded specification. Anything
   else, for example `com.example.dice`, is free for frontends and bots.
4. **The server is feature-blind.** No feature in this document needs the server to learn
   an event type. Where the server must help, it helps with opaque bytes.
5. **The C ABI is generic.** Frontends send and receive events by type through one
   `corded_send_event` call and one event stream. Convenience helpers are optional sugar.

Relation kinds reserved in protocol version 1:

| Kind | Meaning |
|---|---|
| `reply` | This event answers the target; shown in the main timeline |
| `thread` | This event belongs to the thread rooted at the target |
| `annotation` | A small addition to the target, aggregated by key (reactions, poll votes) |
| `replace` | This event supersedes the content of the target (edits) |
| `redact` | The target's content should be removed |
| `reference` | A plain pointer to the target with no display rule (pins, receipts, forwards) |

## 2. Feature catalogue

For each feature: the events it uses, what the server must do, what the client core must
do beyond storing the event, and where the groundwork is laid.

### Text messages (core, Stage 3 to 5)
- **Events.** `m.text` with `{ body, format?, formatted_body? }`.
- **Server.** Nothing beyond routing.
- **Core.** Nothing beyond the event store.

### Replies (F1, built in Stage 5)
- **Events.** Any message event with relation `reply` to the answered event.
- **Server.** Nothing.
- **Core.** Relations index lookup; if the target is not in the vault, request history
  around it or show "original message unavailable".
- **Frontend.** Renders a quoted preview above the message.

### Reactions (F1, built in Stage 5)
- **Events.** `m.reaction` with relation `annotation` and `{ key: "<emoji or shortcode>" }`.
  Removing a reaction is an `m.redaction` of the reaction event.
- **Server.** Nothing.
- **Core.** Aggregate by `(target, key)` to a count and a list of senders; one reaction
  per sender per key, enforced on the client when aggregating.

### Edits (F2)
- **Events.** `m.edit` with relation `replace` and the full new content.
- **Server.** Nothing.
- **Core.** Only the original sender's edits apply. The latest valid edit by `origin_ts`,
  tie-broken by `event_id`, is the displayed content. All versions are kept so history can
  be shown.

### Deletions (F2)
- **Events.** `m.redaction` with relation `redact`.
- **Server.** Optional: a signed delete request lets the sender or a room admin ask the
  server to drop the stored ciphertext early. Specified in Stage 1, not required.
- **Core.** Sender or a room moderator may redact. The target's content is erased from the
  vault and replaced by a tombstone; relations to it are kept.
- **Honest limit.** Deletion is a request. A recipient who already has the plaintext
  cannot be forced to forget it.

### Mentions (F2)
- **Events.** A `mentions` array of user ids inside the content of any message event,
  plus an optional `room: true`.
- **Server.** Nothing. The server cannot see who was mentioned.
- **Core.** Sets a "mentions me" flag on the event and on the room's unread counters.

### Threads (F3)
- **Events.** Any message event with relation `thread` to the root. A reply inside a
  thread uses `thread` to the root and carries `in_reply_to` in content.
- **Server.** Nothing.
- **Core.** Per-root aggregate: reply count, participants, latest event. Queries for
  "events in thread X" and "threads in room Y with unread".
- **Why no schema change.** Threads are rows in the relations index with
  `kind = 'thread'`.

### Attachments and media (F4)
- **Events.** `m.file`, `m.image`, `m.audio`, `m.video`. Content holds
  `{ blob_id, key, nonce, sha256, size, mime, name, thumbnail? }`.
- **Server.** Blob store: chunked upload, download, quota, expiry. Built in Stage 2
  (step 2.10) and unused until this milestone.
- **Core.** Encrypt and chunk on upload, verify hash and decrypt on download, a cache
  with a size limit, progress events.
- **Privacy.** File names, MIME types and thumbnails are inside the encrypted event. The
  server sees only blob size and who uploaded it.

### Link previews (F4)
- **Events.** A `previews` array inside message content, generated by the sender's client.
- **Server.** Nothing. The server never fetches URLs.
- **Core.** Nothing. Fetching is a frontend policy and off by default, because fetching a
  URL reveals the sender's IP address to that site.

### Typing indicators (F5)
- **Events.** Ephemeral `m.typing`, encrypted with the room's group session.
- **Server.** Route ephemeral frames to connected members and drop them (step 2.9).
- **Core.** A per-room timer map; no vault writes.

### Read receipts (F5)
- **Events.** `m.receipt` with relation `reference` to the last read event. Private by
  default (sent only to the user's own devices); shared if the user opts in.
- **Server.** Nothing beyond routing.
- **Core.** A read marker per room, unread counts, and per-event "read by" when shared.

### Presence (F5)
- **Events.** Ephemeral `m.presence`, sent only to rooms the user chooses.
- **Server.** The server already knows who is connected. It does not publish that to
  other users; presence is a client-sent event so it stays opt-in.
- **Core.** A last-seen map in memory.

### Pins (F6)
- **Events.** State event `m.room.pins` with a list of event ids.
- **Server.** Nothing.
- **Core.** Uses generic state resolution; a room permission controls who may set it.

### Polls (F6)
- **Events.** `m.poll.start` with question and options; `m.poll.response` with relation
  `annotation` and key = option id; `m.poll.end` with relation `reference`.
- **Server.** Nothing.
- **Core.** The same aggregation as reactions, with one vote per sender (latest wins).

### Disappearing messages (F6)
- **Events.** State event `m.room.retention` with a time to live; each message may also
  carry `expires_at`.
- **Server.** Honours a per-event expiry hint in the unencrypted frame header so it can
  drop ciphertext on time. This reveals that an expiry exists, not the content.
- **Core.** A sweeper that erases expired events from the vault.
- **Honest limit.** Like deletion, this is cooperative.

### Room metadata: name, topic, avatar, permissions (core plus F2)
- **Events.** State events `m.room.name`, `m.room.topic`, `m.room.avatar`,
  `m.room.power_levels`.
- **Server.** Holds membership and roles for access control. Whether it also holds the
  room name in the clear is a per-room choice; encrypted is the default.
- **Core.** Generic state resolution.

### Multi-device and encrypted backup (F7)
- **Events.** Device list updates signed by the user identity key; `m.key.share` to-device
  messages for sending group sessions to a new device.
- **Server.** Already stores a device list per user (Stage 2).
- **Core.** Device linking flow (QR code or code phrase), per-device fan-out, optional
  history key sharing, and an exportable encrypted backup of keys and history.
- **Why later.** It needs care rather than new mechanisms; every structure already
  carries a device id (D-15).

### Bots and custom integrations (Stage 6)
- **Events.** Custom namespaced types, for example `com.example.dice.roll`.
- **Server.** Nothing. A bot is an ordinary device.
- **Core.** Nothing. Unknown types flow through to the frontend or bot that understands them.

### Further out
Voice messages (an attachment type), custom emoji and stickers (attachment plus a state
event listing packs), message forwarding (`reference` relation), spaces or room
categories (state events linking rooms), and calls (signalling as ephemeral events, media
out of band) all fit the same mechanisms. Calls are a non-goal for 1.0.

## 3. Coverage check

| Feature | Event type | Relation | State | Ephemeral | Blob | Server change |
|---|---|---|---|---|---|---|
| Text | yes | | | | | none |
| Replies | | reply | | | | none |
| Reactions | yes | annotation | | | | none |
| Edits | yes | replace | | | | none |
| Deletions | yes | redact | | | | optional early delete |
| Mentions | content field | | | | | none |
| Threads | | thread | | | | none |
| Attachments | yes | | | | yes | blob store (built in 2.10) |
| Link previews | content field | | | | | none |
| Typing | yes | | | yes | | ephemeral routing (built in 2.9) |
| Read receipts | yes | reference | | | | none |
| Presence | yes | | | yes | | ephemeral routing |
| Pins | yes | | yes | | | none |
| Polls | yes | annotation, reference | | | | none |
| Disappearing | yes | | yes | | | expiry hint (specified in 1.5) |
| Room metadata | yes | | yes | | | none |
| Multi-device | yes | | | | | none (device lists built in 2.5) |
| Bots | custom | any | any | any | any | none |

Every server capability a feature needs is built during Stage 2, so shipping a feature
later is a client-side change plus a specification entry.

## 4. Feature milestones

Each milestone follows the same recipe: add the event type to `docs/protocol/events/`,
add engine logic if the feature needs aggregation, expose any convenience helper in
`corded.h` as a minor ABI version bump, render it in `corded-tui`, add conformance
scenarios.

| Milestone | Contents | Earliest start | Size |
|---|---|---|---|
| F1 | Replies, reactions | Inside Stage 5 (step 5.9) | S |
| F2 | Edits, deletions, mentions, room metadata UI | After Stage 5 | M |
| F3 | Threads | After F1 | M |
| F4 | Attachments, images, link previews | After Stage 5 | M |
| F5 | Typing, read receipts, presence | After Stage 5 | S |
| F6 | Pins, polls, disappearing messages | After F2 | M |
| F7 | Multi-device linking, encrypted backup | After Stage 5; before 1.0 is desirable | L |

## 5. Design obligations this places on the core stages

- **Stage 1** specifies the event envelope, relation kinds, state events, ephemeral
  frames, blobs, the unknown-event rules and capability flags.
- **Stage 2** builds ephemeral routing, the blob store and the expiry hint even though no
  client uses them yet.
- **Stage 3** builds the vault as an event store with a generic relations index and
  generic state resolution, and tests them with reply, reaction and edit events.
- **Stage 5** exposes a generic `corded_send_event`, delivers unknown events to frontends
  unchanged, and proves the approach by shipping replies and reactions.
