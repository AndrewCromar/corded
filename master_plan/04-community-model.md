# The Community Model: One Server, One Community

Requirement from the owner (decision D-27): **each Corded server is one community, in the
way a Discord server is.** It has channels. One user owns it. The owner creates roles,
gives them permissions and assigns them to members. Someone who wants to run two
communities sets up two servers.

The owner has since described the feel as closer to Slack than Discord, and really a mix
of the two. That fits what is specified here: one community per server, channels as the
main space, threads hanging off messages, direct messages and small private groups on the
side, and one owner handing out roles. Where the two products differ, the choice is noted
in the text. The main one still open is whether newcomers see earlier history, which
both Slack and Discord allow and end-to-end encryption makes hard (section 3).

This document defines that model and then lays out the plan for changing what has been
built so far to match it. Where it disagrees with an older part of the plan, this document
wins; the affected documents carry a pointer here.

## 1. What changes from the earlier design

| Topic | Earlier plan and prototype | Now |
|---|---|---|
| What a server is | A relay that hosts unrelated rooms for whoever registers | One community with a name, an owner, channels, roles and members |
| Conversations | Rooms created ad hoc by any user, each with its own member list | **Channels** that belong to the community, plus direct messages between members |
| Who is in a channel | Whoever was added to that room | Every member whose roles let them view it |
| Authority | A server-admin flag, plus per-room owner/admin/member | One **owner**, and **roles** carrying permissions |
| Joining | Register on the server, then get added to rooms | Join the community (usually by invite) and see its channels at once |
| Several communities | Not defined | A client is a member of several servers at the same time; each server is separate |

What does not change: the kernel and frontends split, the C ABI, end-to-end encryption,
typed events with relations, the vault, TLS with pinning, and the absence of federation.

## 2. The model

### Entities

```
 Server (one community)
   name, icon, owner
   |
   +-- Roles          name, colour, position, permissions
   |     "@everyone"  the role every member has
   |
   +-- Members        user, nickname, roles, joined_at
   |
   +-- Categories     name, position                (optional grouping of channels)
   |
   +-- Channels       name, topic, category, position, kind (text; voice later)
   |     permission overrides per role: allow / deny
   |
   +-- Invites        code, created by, uses left, expiry
   |
   +-- Bans           user, reason, by whom

 Direct messages      between two members of the same server, outside the channel list
 Group DMs            small ad-hoc groups of members (what the prototype calls groups)
```

### The owner

- Exactly one account owns a server. The owner has every permission, always, and cannot
  be banned, kicked or stripped of anything by anyone else.
- The owner is named when the server is set up (`cordedd --owner <username>`, replacing
  the prototype's `--admin`). If that account does not exist yet, the name is reserved and
  its first registration becomes the owner.
- Ownership can be transferred by the owner to another member.
- This supersedes the "server administrator" of decision D-26. What D-26 called an
  administrator is now either the owner or any member holding a role with the
  `ADMINISTRATOR` permission.

### Roles and permissions

A role is a named set of permissions. A member's permissions are the union of the
permissions of all their roles, plus `@everyone`. Roles are ordered: a member can only
manage roles and members ranked below their own highest role, so a moderator cannot
promote themselves or demote someone above them.

| Permission | Lets a member |
|---|---|
| `VIEW_CHANNEL` | See a channel and receive its messages |
| `SEND_MESSAGES` | Post in a channel |
| `ADD_REACTIONS` | React to messages |
| `ATTACH_FILES` | Upload attachments (when attachments exist) |
| `MENTION_EVERYONE` | Use `@everyone` |
| `MANAGE_MESSAGES` | Delete other people's messages, pin messages |
| `MANAGE_CHANNELS` | Create, rename, reorder and delete channels and categories |
| `MANAGE_ROLES` | Create and edit roles below their own, and assign them |
| `MANAGE_NICKNAMES` | Change other members' nicknames |
| `KICK_MEMBERS` | Remove a member from the server |
| `BAN_MEMBERS` | Ban and unban |
| `CREATE_INVITE` | Create invite codes |
| `MANAGE_SERVER` | Change the server's name, icon and registration settings |
| `ADMINISTRATOR` | Everything above, in every channel, ignoring overrides |

Each channel can override permissions per role: allow or deny `VIEW_CHANNEL`,
`SEND_MESSAGES` and so on. That is how private channels (deny `VIEW_CHANNEL` for
`@everyone`, allow for one role) and read-only announcement channels (deny
`SEND_MESSAGES` for `@everyone`) are made.

Resolution order for a member in a channel: owner or `ADMINISTRATOR` means everything;
otherwise start from the union of role permissions, apply the channel's `@everyone`
override, then the union of the overrides of the member's other roles, with allow winning
over deny among those.

### Who enforces what

| Rule | Enforced by | Why |
|---|---|---|
| Who may connect, join, be kicked or banned | Server | It controls the connection |
| Who receives a channel's messages | Server and sender's client | The server only stores and forwards copies for members who can view; the sender only encrypts to them |
| Who may post in a channel | Server | It can refuse the frame |
| Who may manage channels, roles, invites | Server | These are server-side records |
| Deleting someone else's message | Server and every client | The server drops the stored ciphertext; clients accept a redaction from a member who holds `MANAGE_MESSAGES` |
| Role names, channel names, member lists | Server-visible | See section 3 |

## 3. What this means for end-to-end encryption

The community model fits end-to-end encryption, with consequences that should be decided
knowingly. Each is recorded in [02-decisions.md](02-decisions.md).

1. **Message content stays end-to-end encrypted.** The server stores ciphertext for
   channels exactly as it does today. The owner, even though they run the machine, cannot
   read a channel they are not a member of.
2. **But the owner can make themselves a member of anything.** Whoever controls roles can
   grant themselves `VIEW_CHANNEL` on any channel and read everything sent from then on.
   That is inherent in "the owner has all permissions". Members can see who is in a
   channel, so it is visible, but it is not prevented. Direct messages are different: no
   role grants access to someone else's direct messages.
3. **New members cannot read earlier messages (D-28, open).** On Discord a newcomer sees
   the whole history. Here, messages were encrypted only to the people who could view the
   channel when they were sent. Options:
   - *Accept it:* history starts when you join. Simplest, strongest. The plan's default.
   - *History sharing:* an existing member's client hands the newcomer the keys for past
     messages. Possible once channels use sender keys (below); it means any member can
     choose to reveal history to a newcomer, which is also true of screenshots.
   - *Unencrypted channels:* a per-channel switch for public, announcement-style channels
     where the server stores plaintext and newcomers see everything. Honest and simple,
     but it must be very clearly labelled in every client.
4. **Channel names, role names and the member list are visible to the server (D-29).**
   The server has to hand the channel list to a newcomer before they share any keys with
   anyone, and it has to evaluate permissions. Hiding names from a server whose operator
   is the owner buys little. Message content, reactions, edits, threads and attachments
   stay hidden.
5. **Channels need sender keys sooner (D-30).** The prototype encrypts each message once
   per recipient. That is fine for a few people and too costly for a channel with
   hundreds of members. The Megolm-style sender keys already chosen in D-04 move from
   "later" to "needed before communities grow": one encryption per message, with the
   channel key shared to each member's device over their pairwise session and rotated
   whenever someone loses access to the channel.
6. **Losing access must rotate keys.** When a member is kicked, banned, or loses a role
   that let them view a channel, every sender in that channel starts a new key so the
   person cannot read what follows.

## 4. Several communities in one client

A person is often in several communities. Each is a separate server at a separate
address, and servers never talk to each other.

- **One identity, many servers.** The vault holds one user identity key. The same key is
  the person's identity on every server they join, so friends can recognise each other
  across communities by safety number. Nickname and roles are per server.
- **The vault stores a list of servers,** each with its address, pinned TLS fingerprint,
  channel list, membership and sync state. The prototype's one-server-per-vault limit
  goes away.
- **The engine holds one connection per server** and tags every event with the server it
  came from.
- **Frontends show a server list** (the column of icons in Discord), then the channels of
  the selected server, then direct messages.
- **Direct messages are per server** in this plan: you message someone through a server
  you share. Cross-server direct messages would need federation, which stays a non-goal.

## 5. Protocol and storage changes

### Server schema (replaces the room-centric tables)

```sql
server_info   (key, value)                          -- name, owner user id, settings
members       (user_id PK, nickname, joined_at)     -- everyone who has joined
roles         (role_id PK, name, colour, position, permissions, is_everyone)
member_roles  (user_id, role_id)
categories    (category_id PK, name, position)
channels      (channel_id PK, kind, name, topic, category_id, position, created_at, next_seq)
              -- kind: 0 text channel, 1 direct message, 2 group DM
channel_overrides (channel_id, role_id, allow, deny)
channel_members   (channel_id, user_id)             -- only for direct messages and group DMs
invites       (code_hash PK, created_by, uses_left, expires_at)
bans          (user_id PK, reason, banned_by, at)
channel_events (channel_id, seq, recipient_device, ...)   -- as room_events today
```

Membership of a text channel is not stored per channel. It is computed: every member
whose resolved permissions include `VIEW_CHANNEL` there.

### Frames

| Area | Frames |
|---|---|
| Server | `ServerInfo` (name, owner, my permissions), `UpdateServer` |
| Members | `MemberList`, `MemberUpdated` (push), `SetNickname`, `Kick`, `Ban`, `Unban` |
| Roles | `NewRole`, `EditRole`, `RemoveRole`, `GrantRole` (roles are listed in `ServerInfo`) |
| Channels | `ChannelList`, `CreateChannel`, `UpdateChannel`, `DeleteChannel`, `SetOverride`, `ChannelUpdated` (push) |
| Invites | `CreateInvite`, `ListInvites`, `RevokeInvite` |
| Direct messages | `OpenDirect`, `CreateGroup` (today's `CreateRoom`) |
| Events | `SendRoomEvent` and `RoomEvent` keep working, addressed to a channel id |

Every change to roles, members or channels is pushed to online members so their clients
keep an accurate picture of who can see what. Clients use that picture to decide whom to
encrypt to; the server checks it again on every send.

### Event model

Unchanged. Messages, replies, threads, reactions, edits and deletions are the same typed
events with relations, sent into a channel instead of a room. Two additions:

- A redaction is honoured from the original sender **or** from a member with
  `MANAGE_MESSAGES` in that channel.
- Mentions gain `@role` alongside `@user` and `@everyone`.

## 6. Change plan

How to get from what exists on the `next` branch to this model. Each step leaves the tests
passing and the application usable. Steps C1 to C5 are the core of the change; C6 onward
build on it.

| Step | Change | What is touched | Done when |
|---|---|---|---|
| **C1** | **Owner and roles on the server.** Replace the admin flag with an owner (`--owner`), a `roles` table with the permission set above, `@everyone`, and role assignment. Kick and ban move behind `KICK_MEMBERS` and `BAN_MEMBERS`. | Server storage and handlers; wire schema | Tests: the owner can create a role, grant it, and the holder gains exactly those powers; role hierarchy is enforced; nobody can act on the owner |
| **C2** | **Channels.** Text channels belong to the server, are created by members with `MANAGE_CHANNELS`, and are visible to everyone who can view them. A new server starts with `#general`. Joining the server puts you in every channel you can view. Direct messages and group DMs stay as they are. | Server storage and handlers; wire schema; engine room handling | Tests: a newcomer sees existing channels and can talk at once; a member who cannot view a channel receives nothing from it |
| **C3** | **Channel permission overrides.** Private channels and read-only channels through per-role allow and deny. Losing view access removes the channel from that member's client. | Server permission resolution; engine | Tests: private channel visible only to its role; read-only channel refuses posts from others; revoking a role removes access |
| **C4** | **Client and TUI.** Engine commands and events for server info, roles, channels and members. TUI shows `# channels` and direct messages separately, a member list with roles, and management commands for people who hold the permissions. | Engine, C ABI command list, TUI | A person can run a community from the TUI: create channels and roles, assign them, kick and ban |
| **C5** | **Several servers per vault.** The vault and engine handle many servers at once; the TUI gets a server switcher and `/join <invite link>`. | Vault schema, engine connection handling, TUI | Tests: one client in two servers at once, with separate channels, history and identity pinning |
| **C6** | **Invites in the protocol.** Members with `CREATE_INVITE` create and revoke codes with use counts and expiry. An invite link carries the address, the TLS fingerprint and the code. | Server, engine, TUI | Tests: join by link; expired and used-up codes refused |
| **C7** | **Moderation.** `MANAGE_MESSAGES` lets a moderator delete others' messages: the server drops the ciphertext and clients honour the redaction. Pins. | Server, engine, TUI | Tests: moderator deletion honoured, the same attempt without the permission ignored |
| **C8** | **Sender keys for channels.** One encryption per message, keys shared over pairwise sessions, rotation when anyone loses access. | Core crypto, engine | Tests: membership-change matrix; a removed member's client cannot decrypt anything sent after removal |
| **C9** | **History for newcomers** (D-28): request, server switch, member switch, sharing through another member's client. | Server, engine, TUI | Tests: a newcomer receives earlier messages; nothing is shared when the server or the member has it off; disappearing and deleted messages are never shared |
| **C11** | **Optional unencrypted servers** (D-32): a per-server switch, off by default, with a clear marker in every client. | Server, engine, TUI | Tests: default stays encrypted; with the switch on, a newcomer reads history from the server and every client shows the marker |
| **C12** | **Server scope** (D-33): `cordedd --scope machine`, `network` or `internet`. See "Server scope" below. | Server | Tests: machine scope refuses a connection from another address; network scope refuses a public address; internet scope will not start with open registration unless told to |
| **C13** | **Several devices per person** (D-37): linking and recovery phrase, several devices per account on the server, a session per device. See [05-operations-and-platforms.md](05-operations-and-platforms.md), section 2. | Server, core crypto, engine, TUI | Tests: the same person on two devices receives everything on both; a removed device receives nothing further |
| **C14** | **Windows build, then GUI on every platform** (D-38). Section 3 of the same document. | Build system, CI, new GUI | The terminal client runs natively on Windows; CI builds every platform |
| **C15** | **Browser client** (D-39). Section 4 of the same document. | Server (WebSocket listener), core (WebAssembly build), new web frontend | A browser joins a server and talks to an installed client |
| **C17** | **Isolated home hosting** (D-40): a Pi setup script (unprivileged account, sandboxed service, firewall) and an outgoing-tunnel mode. Section 5 of the same document. | Packaging, server | A Pi set up by the script cannot reach private addresses; a server in tunnel mode is reachable with no port forwarded |
| **C16** | **Remote administration** (D-34, D-35, D-36): settings stored in the server and editable from the client, housekeeping and scheduled restarts, `/reboot`, and signed remote `/update`. Section 1 of the same document. | Server, engine, TUI, release pipeline | Tests: a setting changed from the client survives a restart; a scheduled restart happens and clients return; an update with a bad signature is refused |
| **C10** | **Polish that makes it feel like a community:** categories, channel topics, nicknames, role colours, `@role` mentions, unread and mention counts per channel, ownership transfer. | Server, engine, TUI | Per feature |

### Status

| Step | State |
|---|---|
| C1 Owner and roles | Built on `next` |
| C2 Channels | Built on `next` |
| C3 Channel permission overrides | Built on `next` (private and read-only channels) |
| C4 Client and TUI | Built on `next` (engine commands and TUI commands; no dedicated management screens) |
| C5 Several servers per vault | Built: one session per server sharing one identity and vault; rooms and events tagged with their server; the terminal client lists, joins and switches servers |
| C6 Invites | Built on `next`: codes with use limits and expiry, revocation, `corded://` links carrying address, key and code |
| C7 Moderation | Deleting others' messages is built on `next`. Pins are not. The server does not yet drop the stored ciphertext of a deleted message |
| C4b Members and settings pages | Built: a Members page (roles, online, display names; kick, ban, remove for those permitted), display names per server (`/nick`, and `/setnick` for those with `manage_nicknames`), and a Settings page showing personal settings and, for those with `manage_server`, the server's settings and status |
| C8 Sender keys | Not started |
| C9 History for newcomers | Built on `next`: asked for automatically on joining a channel or group, relayed by the server to up to two online members, shared from their vaults as an encrypted envelope for the newcomer alone, with the server switch and the member switch |
| C10 Polish, C11 Optional unencrypted servers | Not started |
| C12 Server scope | Built: `--scope machine`, `network` or `internet`; machine is the default for a new server; network scope turns away non-local addresses; internet scope makes registration invite-only unless told otherwise. The extra limits for internet scope (accounts per address per day, handshake timeouts) are not built |
| C16 Remote administration | Partly built: settings stored in the server and changed from a client, `/status`, `/reboot`, scheduled restarts, hourly housekeeping with a retention window. Remote update is not built; it waits on the release signing decision |
| C13 Several devices | Built: a recovery key recreates the identity on another device; the server keeps several devices per person; clients keep a session per device and send to every device, including the sender's own others; a new device gets history from the person's other device. Not built: linking by code or QR, listing and removing devices, telling contacts a device was added |
| C14 Windows and GUI | Windows build of the server and terminal client is released. The GUI is planned in [06-gui-and-android.md](06-gui-and-android.md), Android first; not started |
| C15 Browser client | Set aside (owner decision) |
| C17 Isolated home hosting | Not started |

Simplifications in what is built, to be revisited: after any change to roles, channels or
membership the server sends every online member a full fresh picture instead of a small
update; a member's roles are only refreshed in channel member lists; role positions
cannot be reordered after creation.

### Compatibility while changing

- The frozen prototype (`prototype-v0`) is untouched.
- On `next`, server databases and vaults made before C1 and C2 will not carry over. That
  is acceptable now, because nothing is deployed; it stops being acceptable at the first
  release, when real migrations become mandatory.
- The wire schema keeps growing by adding tables and union members only, so the change is
  additive at the protocol level even though behaviour changes.

### Server scope (D-33)

Requirement from the owner: the server should be configurable to its scope (this machine,
the local network, or the internet), without opening security holes.

Today the server listens on every network interface by default and lets anyone register,
which is the widest and least safe combination. It can already be narrowed by hand
(`--host 127.0.0.1` for this machine only; `--invite-only` or `--closed` for
registration), and whether it is reachable from the internet depends on the router. The
change makes the choice explicit and the defaults safe:

| Scope | Listens on | Accepts connections from | Registration default |
|---|---|---|---|
| `machine` (default) | loopback only | this computer | open |
| `network` | all interfaces | private and link-local addresses only; anything else is refused | open |
| `internet` | all interfaces | anywhere | invite-only; open registration must be asked for explicitly |

- The scope is printed at start-up, along with the address others should use.
- `internet` scope tightens the limits that matter when strangers can reach the server:
  lower connection and registration rates per address, a cap on accounts per address per
  day, and handshake and idle timeouts.
- Reaching a home server from the internet still needs the router to forward the port
  (or a tunnel); the operator guide explains how, and the server cannot do that itself.
- Exposure to the internet is where the unaudited state of the code matters most. The
  guide says so plainly until an audit has happened.

## 7. Effect on the staged plan

The stage structure stands. What moves:

- **Stage 1 (protocol specification)** specifies servers as communities: the entities in
  section 2, the permission table and resolution order, and the frames in section 5.
  Its "rooms and membership" step becomes "channels, roles and membership".
- **Stage 2 (server)** builds the schema and frames in section 5. Its authorisation
  function `can(actor, action, target)` becomes the permission resolver described here.
  The "server administrators" section added for D-26 is superseded by the owner and roles.
- **Stage 3 (client engine)** stores several servers per vault, each with channels, roles
  and members.
- **Stage 4 (cryptography)** delivers sender keys earlier than before, because channels
  depend on them, and ties key rotation to loss of `VIEW_CHANNEL`.
- **Stage 5 (C ABI and TUI)** exposes server, role, channel and member management, and
  the TUI becomes a community client with a server list and a channel list.
- **Feature roadmap.** Mentions gain roles; pins and moderation depend on permissions;
  everything else is unaffected.

### History for newcomers (D-28, decided)

The owner's decision: the new client requests old messages. If the server allows that, it
gives them; where it has to get them from other clients, it asks those clients, which may
have sharing turned off. So a newcomer may or may not get old messages, depending on
whether the server or the other members have it disabled.

Because channel content is end-to-end encrypted, the server holds nothing it could hand
over by itself, so in practice the messages always come from another member's client:

```
 newcomer                server                      an existing member
    | -- HistoryRequest --> |                               |
    |   (room, how many)    | checks: is sharing allowed    |
    |                       | on this server? can the       |
    |                       | newcomer view this room?      |
    |                       | -- HistoryWanted -----------> |
    |                       |    (room, who is asking)      | checks: has this member
    |                       |                               | turned sharing off? is the
    |                       |                               | asker really in the room?
    | <====== old messages, encrypted to the newcomer only, sent through the server ======= |
```

- **Server switch.** `cordedd --no-history-sharing` refuses every request. Default: allowed.
- **Member switch.** Each client has a "share history with newcomers" setting. Default: on.
  A member who turns it off simply does not answer.
- **Who answers.** The server passes the request to members who are online and can view
  the room. If nobody is online, the newcomer gets nothing for now and may ask again.
- **What is shared.** Recent messages in that room from the answering member's vault, up
  to a limit. Disappearing messages and deleted messages are never shared.
- **What the newcomer can and cannot trust.** The old messages arrive from the member who
  shared them, not from their original authors, so the newcomer is trusting that member's
  account of who said what. Clients mark such messages as shared history.
- **Requests are not accepted blindly.** A client only accepts shared history it asked
  for, for a room it is in, from someone who is a member of that room.

### Turning end-to-end encryption off (D-32, decided in principle)

The owner's decision: end-to-end encryption is always on by default but can be disabled
if wanted. The shape proposed for it:

- **A setting chosen by the owner**, `cordedd --no-e2ee` or a server setting changed by
  someone with `MANAGE_SERVER`. Default stays encrypted.
- **With it off**, channel messages are sent to the server readable (still inside TLS),
  the server stores them, and newcomers get the full history from the server directly,
  search can work on the server, and large channels cost nothing extra. Direct messages
  can stay end-to-end encrypted regardless.
- **Every client shows it plainly**, per server: a visible "not end-to-end encrypted:
  the server's operator can read channels here" marker, at join time and in the header.
  A client must never present an unencrypted server as if it were private.
- **It cannot be flipped silently.** Changing the setting is announced to all members,
  and turning encryption *off* on a server that had it on does not expose old messages
  (the server never had their keys); it only affects messages from then on.
- **What is lost** on such a server: the operator, anyone who compromises the server, and
  anyone with access to its disk or backups can read channel content.

This is change-plan step C11. On servers that choose it, C8 (sender keys) is unnecessary.

## 8. Open questions for the owner

1. ~~History for new members (D-28).~~ Decided: newcomers request history and get it if
   the server and the members allow. See section 3.
2. **Direct messages across servers.** The plan keeps them within one server. Is that
   acceptable?
3. **Can the owner see who is in direct messages with whom?** The server necessarily
   knows. Listing it in an admin view is a choice; the plan does not expose it.
4. **Ad-hoc group DMs.** Keep them alongside channels (as Discord does), or drop them now
   that channels exist? The plan keeps them.
