# Stage 4: Cryptography

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Previous: [Stage 3](stage-3-client-engine.md) | Next: [Stage 5](stage-5-c-abi-and-tui.md)

## Goal

Real end-to-end encryption. Implement identity and device keys, X3DH session setup,
Double Ratchet for pairwise sessions, sender-key group sessions for rooms, key
verification, and persistence of all of it in the vault. Then replace the test-only
provider from Stage 3 so that the server database holds nothing but ciphertext.

## Ground rules

These apply to every line written in this stage.

1. **No new cryptography.** Only libsodium primitives, composed as published protocols
   describe (the Signal X3DH and Double Ratchet specifications, and the Megolm / sender
   keys pattern). Any deviation is written down and justified in `docs/crypto/`.
2. **Specification first.** Each step writes its section of `docs/crypto/` before or
   alongside the code, and the two are reviewed together.
3. **Secrets are types.** Key material lives in `SecretKey<N>` / `SecureBytes`, which
   cannot be copied, logged, formatted or compared with `==`.
4. **Every protocol has known-answer tests** and is cross-checked against an independent
   implementation.
5. **Fail closed.** Any verification failure discards the message and reports an error.
   There is no "decrypt anyway" path.

## Scope

In scope: everything listed in the goal, plus the vault tables for crypto state.

Out of scope: MLS / TreeKEM (deferred, decision D-04); sealed sender; post-quantum key
agreement (noted as future work); multi-device linking flows (feature milestone F7,
though all structures here are per-device so nothing needs redesign).

## Prerequisites

Steps 4.1 to 4.9 need only Stages 0 and 1, and form a library (`core/src/crypto/`) with
no dependency on the engine. They can be built in parallel with Stages 2 and 3. Steps
4.10 and 4.11 need Stage 3. Blueprint source: section 4, roadmap Phase 3.

## Design

### Primitives (all from libsodium)

| Purpose | Primitive | libsodium API |
|---|---|---|
| Signatures | Ed25519 | `crypto_sign_*` |
| Key agreement | X25519 | `crypto_scalarmult`, `crypto_kx` not used |
| AEAD | XChaCha20-Poly1305 | `crypto_aead_xchacha20poly1305_ietf_*` |
| KDF | HKDF-SHA256 | `crypto_kdf_hkdf_sha256_*` (1.0.19+) |
| MAC for chain steps | HMAC-SHA256 | `crypto_auth_hmacsha256_*` |
| Hash | SHA-256, BLAKE2b | `crypto_hash_sha256`, `crypto_generichash` |
| Password hashing | Argon2id | `crypto_pwhash` |
| File encryption | XChaCha20-Poly1305 stream | `crypto_secretstream_*` |
| Randomness | system CSPRNG | `randombytes_buf` |
| Memory | guarded, locked allocations | `sodium_malloc`, `sodium_mlock`, `sodium_memzero` |

AES-GCM is not used (decision D-10).

### Key inventory

| Key | Type | Lifetime | Stored |
|---|---|---|---|
| User identity key | Ed25519 | Permanent | Vault `identity` |
| Device signing key | Ed25519 | Life of the device | Vault `identity` |
| Device DH key | X25519 | Life of the device | Vault `identity` |
| Signed prekey | X25519 | Rotated weekly, old one kept 2 weeks | Vault `prekeys` |
| One-time prekeys | X25519 | Single use | Vault `prekeys` |
| Pairwise root, chain, message keys | 32 bytes each | Per ratchet step / message | Vault `pairwise_sessions`, `skipped_keys` |
| Outbound group chain key and signing key | 32 bytes, Ed25519 | Until rotation | Vault `outbound_group_sessions` |
| Inbound group chain key | 32 bytes at a known index | Until superseded | Vault `inbound_group_sessions` |
| Vault key, KEK | 32 bytes | See Stage 3 step 3.3 | Header (wrapped) |

### Vault tables added by this stage

```sql
CREATE TABLE peer_devices (
    user_id BLOB NOT NULL, device_id BLOB NOT NULL,
    dh_key BLOB NOT NULL, cert BLOB NOT NULL,
    first_seen_at INTEGER NOT NULL,
    trust INTEGER NOT NULL,              -- 0 unverified (TOFU), 1 verified, 2 changed, 3 blocked
    PRIMARY KEY (user_id, device_id)
) WITHOUT ROWID;

CREATE TABLE peer_users (
    user_id BLOB PRIMARY KEY, device_list BLOB NOT NULL, device_list_version INTEGER NOT NULL,
    trust INTEGER NOT NULL, verified_at INTEGER
) WITHOUT ROWID;

CREATE TABLE prekeys (
    key_id INTEGER PRIMARY KEY, kind INTEGER NOT NULL,   -- 0 signed, 1 one-time
    public_key BLOB NOT NULL, secret_key BLOB NOT NULL,
    created_at INTEGER NOT NULL, published INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE pairwise_sessions (
    peer_device BLOB NOT NULL, session_id BLOB NOT NULL,
    state BLOB NOT NULL,                 -- serialised ratchet state, versioned
    is_active INTEGER NOT NULL, created_at INTEGER NOT NULL, last_used_at INTEGER NOT NULL,
    PRIMARY KEY (peer_device, session_id)
) WITHOUT ROWID;

CREATE TABLE skipped_keys (
    peer_device BLOB NOT NULL, session_id BLOB NOT NULL,
    dh_pub BLOB NOT NULL, msg_n INTEGER NOT NULL,
    message_key BLOB NOT NULL, stored_at INTEGER NOT NULL,
    PRIMARY KEY (peer_device, session_id, dh_pub, msg_n)
) WITHOUT ROWID;

CREATE TABLE outbound_group_sessions (
    server_id INTEGER NOT NULL, room_id BLOB NOT NULL,
    session_id BLOB NOT NULL, chain_key BLOB NOT NULL, msg_index INTEGER NOT NULL,
    sign_secret BLOB NOT NULL, created_at INTEGER NOT NULL, msgs_sent INTEGER NOT NULL,
    shared_with BLOB NOT NULL,           -- set of device ids that have received it
    PRIMARY KEY (server_id, room_id)
) WITHOUT ROWID;

CREATE TABLE inbound_group_sessions (
    server_id INTEGER NOT NULL, room_id BLOB NOT NULL,
    sender_device BLOB NOT NULL, session_id BLOB NOT NULL,
    chain_key BLOB NOT NULL, chain_index INTEGER NOT NULL,   -- earliest index we can decrypt
    sign_public BLOB NOT NULL, received_at INTEGER NOT NULL,
    PRIMARY KEY (server_id, room_id, sender_device, session_id)
) WITHOUT ROWID;
```

The blueprint's `ratchet_states` (one root key, two chain keys, one public key) could not
handle out-of-order messages or even hold its own DH private key (gap G-03). The
serialised `state` blob here holds the full Double Ratchet state defined in step 4.4.

## Steps

### 4.1 Primitive wrappers and secure memory

Tasks:
- `sodium_init` once, checked; a minimum libsodium version assertion.
- `SecureBytes` and `SecretKey<N>`: `sodium_malloc` backed (guard pages, `mlock`), zeroed
  on destruction, move-only, no `operator==`, no stream or `std::format` support.
  Deleting those operations makes accidental logging a compile error.
- `PublicKey<N>`, `Signature`, `Nonce<N>`: ordinary value types with constant-time
  comparison.
- Thin typed wrappers, each a few lines: `sign`, `verify`, `x25519`, `aead_encrypt`,
  `aead_decrypt`, `hkdf_extract`, `hkdf_expand`, `hmac`, `random_bytes`, `argon2id`.
  Wrappers take and return the typed keys, so a signing key cannot be passed where a DH
  key is expected.
- X25519 output check: reject an all-zero shared secret (small-order point).
- Handling for `mlock` limits: if locking fails (common in containers), log once and
  continue with zeroing only, rather than failing.
- Domain separation constants in one header: every KDF `info` string and signature
  context string, each prefixed `corded/v1/`.

Tests: RFC 8032 (Ed25519), RFC 7748 (X25519), RFC 5869 (HKDF), RFC 4231 (HMAC), the
XChaCha20-Poly1305 draft vectors, and the relevant Project Wycheproof sets, loaded from
`tests/vectors/`.

Acceptance: all vectors pass on every CI platform; a compile-fail test proves a
`SecretKey` cannot be logged or copied.

### 4.2 Key types, identity and device keys

Tasks:
- Generate the user identity key, device signing key and device DH key.
- Build and verify the device certificate and the signed device list exactly as the
  Stage 1 identities document specifies.
- Canonical serialisation for everything that is signed: fixed field order, length
  prefixes, context string first. No signing of JSON or of FlatBuffers bytes, whose
  encodings are not canonical.
- Fingerprints: a stable, human-comparable rendering of a user identity key (see 4.9).
- Replace the minimal identity code Stage 3 used for authentication with this module.

Tests: certificate and device list round trips; rejection of wrong signer, altered
fields, stale version, truncated input.

Acceptance: a device list produced here is accepted by the Stage 2 server and vice versa
with the Stage 1 vectors.

### 4.3 Prekeys and X3DH

Closes gap G-01. The blueprint's "3DH" has no signed prekey or one-time prekeys, so it
could not start a session with an offline peer.

Tasks:
- Prekey management: generate a signed prekey (signed by the device signing key) and
  batches of 100 one-time prekeys; track which are published; rotate the signed prekey
  weekly and keep the previous one for two weeks; refill one-time prekeys when the
  server sends `PrekeysLow`; delete a one-time prekey's secret as soon as it is used.
- Initiator (Alice, with Bob's bundle): verify Bob's device certificate against his user
  identity key and the signed prekey signature; generate an ephemeral key `EK`; compute

```
  DH1 = X25519(IK_A, SPK_B)
  DH2 = X25519(EK_A, IK_B)
  DH3 = X25519(EK_A, SPK_B)
  DH4 = X25519(EK_A, OPK_B)        (omitted if the bundle had no one-time prekey)
  SK  = HKDF(salt = 0, ikm = 0xFF*32 || DH1 || DH2 || DH3 [|| DH4],
             info = "corded/v1/x3dh")
  AD  = IK_A_pub || IK_B_pub || device ids
```

  where `IK` is the device DH key. Erase `EK` secret and the DH outputs.
- Initial message: carries `IK_A`, `EK_A`, the ids of the prekeys used, and the first
  Double Ratchet message. Sent as a `ToDevice` payload.
- Responder (Bob): load the referenced prekey secrets, compute the same `SK`, delete the
  one-time prekey, initialise the ratchet, decrypt the first message. If decryption
  fails, nothing is persisted.
- Simultaneous initiation (both sides start a session at once): keep both, mark the most
  recently used as active, as libsignal and Olm do.
- Document the known X3DH properties and limits: replay of the initial message when no
  one-time prekey was used (mitigated by the ratchet immediately advancing), and
  deniability.

Tests: both roles against each other; with and without a one-time prekey; bad signature
on the signed prekey; unknown prekey id; replayed initial message; cross-check of `SK`
against an independent Python implementation committed under `tests/reference/`.

Acceptance: two instances derive the same `SK` and `AD` for every vector; every negative
test fails closed.

### 4.4 Double Ratchet

Tasks:
- Implement the Double Ratchet as specified by Signal, with these concrete choices:
  - `KDF_RK(rk, dh_out)`: HKDF-SHA256, salt = `rk`, info = `corded/v1/dr/root`, output 64
    bytes split into new root key and chain key.
  - `KDF_CK(ck)`: message key = HMAC-SHA256(ck, 0x01), next chain key =
    HMAC-SHA256(ck, 0x02).
  - Message encryption: XChaCha20-Poly1305. The key and nonce are both derived from the
    message key by HKDF (info `corded/v1/dr/msg`), so the nonce is never reused.
  - Associated data: the X3DH `AD` plus the serialised header.
- State (this is what `pairwise_sessions.state` holds): `DHs` (own ratchet key pair),
  `DHr`, `RK`, `CKs`, `CKr`, `Ns`, `Nr`, `PN`, plus session id and version. Skipped
  message keys are stored in their own table.
- Header: `dh_pub`, `pn`, `n`. Sent in the clear inside the pairwise ciphertext frame
  (header encryption is noted as possible future work).
- Skipped message keys: at most 1,000 per session and at most 1,000 skipped in a single
  step (larger gaps are rejected), expired after 30 days. This bounds the damage from a
  malicious peer sending huge counters.
- Decrypt is transactional: work on a copy of the state, and commit the new state and
  consumed keys only if authentication succeeds.
- Versioned serialisation of the state with strict length checks on load.

Tests:
- Conversation simulator: random sequences of sends from both sides with random
  delivery order, loss and duplication; every delivered message decrypts exactly once.
- Forward secrecy: after a message is decrypted, its key cannot be recomputed from the
  stored state (checked by snapshot and attempted re-decrypt).
- Post-compromise recovery: clone a state (simulated compromise), run one full round
  trip, confirm the clone can no longer decrypt.
- Limits: skipped-key caps, replay rejection, wrong associated data.
- Differential test against an independent implementation (the Python `doubleratchet`
  package or a reference written for the purpose) using shared deterministic randomness.

Acceptance: the simulator runs a million messages with no failure; differential tests
agree byte for byte.

### 4.5 Message envelope and padding

Fills the placeholders left in the Stage 1 envelope document.

Tasks:
- Define the wire layout of a pairwise message and of a group message (version byte,
  header, ciphertext) and add them to `docs/protocol/06-envelope.md`.
- Compute associated data exactly as Stage 1 requires: protocol version, room id, event
  id, sender device, crypto mode, then the scheme's own header. A test moves a ciphertext
  to another room and to another event id and confirms decryption fails.
- Padding: pad the plaintext `Event` to the bucket sizes from the specification with an
  unambiguous scheme (ISO/IEC 7816-4: 0x80 then zeros), remove after decryption with
  strict checking.
- After decryption, check that the `sender_user` and `sender_device` inside the event
  equal the cryptographically authenticated sender. Mismatch is a hard failure.

Acceptance: ciphertext lengths fall only on bucket sizes; the relabelling tests fail
closed.

### 4.6 Group sessions (sender keys)

Blueprint Phase 3, third bullet; decision D-04.

Tasks:
- Outbound session per `(room, own device)`: random 32-byte chain key, message index 0,
  a fresh Ed25519 signing key pair, a random session id.
- Per message: derive the message key from the chain key (HMAC with 0x01), advance the
  chain (HMAC with 0x02), encrypt with XChaCha20-Poly1305 (key and nonce from HKDF as in
  4.4), sign `(session id, index, ciphertext, associated data)` with the session signing
  key.
- Group message layout: version, session id, index, ciphertext, signature.
- Inbound session: stores the chain key at the index it was shared at, and the signing
  public key. To decrypt index `i`, advance a copy of the chain from the stored index;
  refuse indices below the stored one; refuse to advance more than a cap (2,000).
- Replay protection: a record of seen indices per session (a sliding bitmap), since the
  chain key alone can re-derive later keys.
- Out-of-order handling: because the stored chain key is kept at the earliest index
  rather than ratcheted on every receive, late messages still decrypt. The cost is weaker
  forward secrecy within a session, which is bounded by rotation. State this in the
  crypto specification. Option to ratchet the stored key forward after a delay.
- Export for sharing: `(session id, chain key at current index, index, signing public
  key)`.

Tests: N simulated devices in a room, random send order and delivery order; forged
signature; index below start; replay; a member who received the session at index 50
cannot decrypt index 10.

Acceptance: simulation with 50 devices and 100,000 messages passes; every negative test
fails closed.

### 4.7 Key distribution and rotation policy

Tasks:
- Before sending to a room, make sure every current member device has the outbound
  session: for each device not in `shared_with`, ensure a pairwise session exists
  (running X3DH if not), then send an `m.key.share` to-device message over it.
- Fetch and verify device lists for members as needed; cache with version; refetch on a
  server hint or on a decryption failure.
- Rotation triggers, any of:
  - a member leaves, is kicked or banned, or one of their devices is removed;
  - 100 messages sent on the session;
  - 7 days since creation;
  - the user marks a member device as blocked.
- A new member joining does not force rotation: they are sent the current session at the
  current index, so they cannot read earlier messages.
- Kick race: an event sent between the kick and the sender learning of it is readable by
  the kicked member. Document this window; the server's ordered `MembershipChanged` entry
  keeps it short.
- Key request (limited): a device that cannot decrypt may ask the sender's device for the
  session again. The sender answers only if the requester is a current member device and
  only from the index it was originally entitled to. Rate limited.
- Batch the fan-out of key shares using the server's batch to-device frame.

Tests: membership change matrix (join, leave, kick, device add, device remove) checking
exactly who can decrypt which message index; a removed member's device fed all later
ciphertext decrypts none of it.

Acceptance: the matrix passes; key share traffic for a 100-member room is one batched
request per rotation.

### 4.8 Session persistence

Tasks:
- Migrations for the tables in Design.
- `SessionStore` with typed load and save; ratchet state changes and the message they
  belong to are committed in the same vault transaction as the decrypted event (ties
  into the Stage 3 pipeline), so a crash can never leave a ratchet advanced without its
  plaintext stored, or the reverse.
- Outbound side: persist the advanced chain key before the ciphertext leaves the
  process, so a crash cannot cause key reuse.
- Garbage collection: expired skipped keys, old inactive pairwise sessions (keep the 5
  most recent per peer device), superseded inbound group sessions past the local
  retention window.
- Versioned state blobs with an upgrade path.

Tests: kill at every point between ratchet step and commit, for send and receive; after
restart no message is lost, none is decrypted twice with different results, and no
nonce or key is reused.

Acceptance: the kill tests pass under the Stage 3 harness.

### 4.9 Identity verification and trust state

Closes gap G-14.

Tasks:
- Trust on first use: the first identity key seen for a user is recorded. If the server
  later presents a different one, mark the user `changed`, emit a warning event, and
  refuse to send until the frontend acknowledges (policy configurable: warn or block).
- Device changes within a verified identity are trusted automatically if the device list
  is correctly signed, and still surfaced as an informational event.
- Safety numbers: a numeric and QR-encodable fingerprint derived from both users'
  identity keys and ids (iterated hash, 60 decimal digits, as Signal does). Comparing
  them out of band marks the peer `verified`.
- Verified users whose identity key changes always block until re-verified.
- API surface for Stage 5: get safety number, mark verified, list devices with trust
  state, block device.

Tests: all trust state transitions; a simulated malicious server swapping an identity
key or adding an unsigned device is detected.

Acceptance: the malicious-server tests pass; trust state survives restart.

### 4.10 Engine integration

Tasks:
- `RealCryptoProvider` implementing the Stage 3 `CryptoProvider` interface over steps
  4.1 to 4.9.
- Pairwise-mode rooms: encrypt the event once per recipient device (and for the sender's
  other devices, when they exist) and send as a multi-recipient frame.
- Group-mode rooms: the flow in 4.7 then one ciphertext.
- Wire up the undecryptable queue: when a key share arrives, retry the frames waiting on
  that session.
- Publish prekeys at registration and on `PrekeysLow`; rotate the signed prekey on a
  timer.
- Remove the insecure provider from every non-test build; the CMake guard from Stage 3
  becomes a hard error; a start-up self-test runs a handful of known-answer checks and
  refuses to start on failure.
- Re-run the entire Stage 3 integration suite with real crypto.
- An inspection test: after a scripted conversation, scan the server database and the
  captured network traffic for every plaintext string used. None may appear.

Acceptance: milestone M4; the inspection test passes; the Stage 3 suite is green with the
real provider.

### 4.11 Vectors, property tests, fuzzing and the crypto specification

Tasks:
- Finish `docs/crypto/`: `primitives.md`, `identity.md`, `x3dh.md`, `double-ratchet.md`,
  `group-sessions.md`, `key-distribution.md`, `verification.md`, `vault.md`, each with
  exact KDF inputs, layouts and constants, and a section on what the construction does
  not protect against.
- Publish Corded's own test vectors (deterministic keys and randomness in, bytes out) for
  X3DH, Double Ratchet and group messages under `tests/vectors/corded/`, so other
  implementations can check themselves.
- Fuzz targets: pairwise message parser, group message parser, initial message parser,
  session state deserialiser, device list parser, key share parser.
- Constant-time review of all comparisons on secret-dependent data; run `dudect` or
  ctgrind-style checks where practical.
- A written self-review against a checklist: nonce uniqueness argument for every AEAD
  call, domain separation for every KDF and signature, zeroisation of every temporary
  secret, error paths leaving state unchanged.

Acceptance: documentation and code agree (checked by a reviewer walking each document
against the source); fuzz targets run an hour each without findings.

## Test plan

Summarised from the steps: published vectors for primitives; differential tests against
independent implementations for X3DH and Double Ratchet; large randomised simulations for
pairwise and group messaging; negative tests that must fail closed; kill tests for
persistence; fuzzing for every parser; an end-to-end inspection test for plaintext leaks.

## Risks and open questions

- **Implementation error in a protocol we wrote ourselves.** The largest risk in the
  project. Mitigations: follow published specifications closely, differential testing,
  the external audit in Stage 7, and the beta warning staying in the README until then.
  Alternative worth a decision before this stage starts: use an existing audited library
  (vodozemac, the Rust successor to libolm, implements exactly Olm and Megolm) through
  FFI instead of writing the ratchets. That trades a pure C++ build for much lower
  cryptographic risk. Recorded as an open question for the owner.
- **Group forward secrecy** is weaker than pairwise by design. Documented; rotation
  limits bound it.
- **No post-quantum protection.** A recorded-now, decrypt-later adversary with a future
  quantum computer could break X25519. PQXDH-style hybrid key agreement is a known
  upgrade path and is listed as future work.
- **`mlock` limits** on small devices and containers.
- **Open:** whether to ratchet stored inbound group keys forward after a grace period.
- **Open:** default policy on identity key change for unverified users: warn or block.

## Definition of done

- All eleven steps meet their acceptance criteria.
- The insecure test provider cannot be built into a release.
- `docs/crypto/` is complete and matches the code.
- Gaps G-01, G-03, G-11, G-14 and G-16 are marked resolved.
- Milestone M4.
