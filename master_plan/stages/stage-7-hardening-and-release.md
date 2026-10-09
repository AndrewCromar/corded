# Stage 7: Hardening and Release

[Overview](../00-overview.md) | [Roadmap](../01-roadmap.md) | Previous: [Stage 6](stage-6-bindings-and-guis.md)

## Goal

Turn a system that works into one that can be trusted and installed. Measure it on the
hardware the blueprint promises, fuzz it continuously, have it reviewed by people who did
not write it, make builds reproducible, package it, document it, and freeze protocol
version 1.

## Scope

In scope: benchmarks, fuzzing infrastructure, internal and external security review,
supply-chain work, packaging for operators and users, documentation, the 1.0 release.

Out of scope: new features. Feature milestones F2 to F7 run alongside this stage but are
tracked in [03-feature-roadmap.md](../03-feature-roadmap.md). Anything that changes the
wire protocol must land before step 7.8.

## Prerequisites

Stage 5 for steps 7.1 to 7.5. Stage 6 for packaging the bindings and the GUI in 7.6.
Steps 7.1 and 7.2 should begin as soon as Stage 5 closes; step 7.4 has long lead times and
should be scheduled early. Blueprint source: roadmap Phase 5, third bullet.

## Steps

### 7.1 Benchmarks and performance budgets

Tasks:
- A benchmark suite in `tests/bench/` (Google Benchmark or Catch2 benchmarks) covering:
  primitive operations, X3DH, Double Ratchet encrypt and decrypt, group encrypt and
  decrypt, key share fan-out for rooms of 10, 100 and 1,000 devices, vault writes, event
  pipeline throughput, timeline queries, cold start and unlock time.
- End-to-end measurements with the load generator from Stage 2 plus real engines:
  message latency, sync time after N missed events, protocol overhead in bytes per
  message for each crypto mode.
- Reference hardware, with results recorded for each: Raspberry Pi 4 (4 GB, SD card),
  Raspberry Pi 5, a 1 vCPU / 512 MB VPS, a mid-range Android phone, a current laptop.
- Budgets, to be confirmed or revised from first measurements:

| Metric | Budget |
|---|---|
| `cordedd` idle memory, 100 connections (Pi 4) | under 30 MB |
| `cordedd` sustained sends, 50-member room (Pi 4) | 500 events/s, p99 under 50 ms |
| Engine unlock time (Pi 4, Argon2id included) | under 2 s |
| Engine memory, 20 rooms, 100,000 stored events | under 60 MB |
| Text message end to end, same region | under 150 ms p95 |
| Wire overhead per group text message | under 400 bytes before padding |
| Sync of 10,000 missed events (laptop) | under 5 s |
| Timeline page query (50 events with aggregates) | under 10 ms |

- A performance regression job: benchmarks run nightly on a fixed runner, results
  stored, a regression over 10 percent opens an issue.
- Profile and fix the worst offenders. Likely candidates: JSON at the ABI boundary,
  SQLite transaction batching, key share fan-out.
- Vault maintenance: local retention settings, incremental `VACUUM`, and their effect on
  the numbers above (carried over from Stage 3's open risk).

Acceptance: `docs/operator/performance.md` and `docs/developer/performance.md` contain
real numbers for every reference device; every budget is met or has an accepted,
documented exception.

### 7.2 Continuous fuzzing

Tasks:
- Inventory every fuzz target written in Stages 2 to 5 and fill gaps. Every parser of
  untrusted bytes must have one: frames, each frame handler, pairwise and group
  messages, the initial X3DH message, session state, device lists, key shares, event
  content, the vault header, `corded_command`, invite links, configuration files.
- Structure-aware fuzzing for FlatBuffers frames (a custom mutator that keeps buffers
  verifiable so fuzzing reaches the handlers).
- Stateful fuzzing: a protocol-level fuzzer that plays sequences of valid and mutated
  frames against a live server and a live engine, looking for invariant violations
  (crash, hang, memory growth, duplicate `seq`, cross-room leakage).
- A differential fuzzer for the ratchets against the reference implementations from
  Stage 4.
- Infrastructure: apply to OSS-Fuzz once public interest justifies it; until then, a
  nightly ClusterFuzzLite job on GitHub Actions with a persisted corpus.
- Corpus management: seed from test vectors, minimise weekly, commit regressions as
  unit tests.
- Coverage report for the fuzz corpus, with a target for parser code.

Acceptance: every target has run for at least 24 cumulative CPU-hours with no open
findings; fuzz coverage of parser code exceeds 85 percent of lines.

### 7.3 Internal security review

Tasks:
- Revisit the Stage 1 threat model against what was actually built. Update it and list
  every deviation.
- Checklist review of the whole codebase, written up in `docs/security/internal-review.md`:
  - Every AEAD call: key and nonce uniqueness argument.
  - Every signature and KDF: domain separation.
  - Every secret: where it is created, stored, copied and zeroed.
  - Every server handler: authorisation check present and tested.
  - Every error path: no state change, no secret in the message.
  - Every log statement: nothing sensitive.
  - Every place the server's word is trusted by a client.
- Metadata audit: capture a realistic session's traffic and server database, and write
  down exactly what an operator can infer. Compare with the claims in the README and
  threat model, and correct the documents.
- Dependency review: versions, known vulnerabilities, how each is built (flags,
  disabled features), update policy.
- Static analysis sweep: clang-tidy at full strength, CodeQL, a second analyser
  (for example the Clang static analyzer or PVS-Studio's free tier for open source).
- Abuse scenarios walked through: spam invites, prekey draining, storage exhaustion,
  oversized rooms, slow-loris connections, malicious room admin, malicious server.
- Fix findings; record accepted risks in the decisions file.
- Revisit decision D-07 (TLS versus Noise) and the crypto-library question raised in
  Stage 4 with real experience in hand.

Acceptance: the review document exists with every item dispositioned; no known high
severity issue is open.

### 7.4 External audit

Tasks:
- Prepare an audit package: threat model, protocol and crypto specifications, the
  internal review, build instructions, test and fuzz setup, a list of areas of concern.
- Define scope in priority order: (1) the cryptographic protocol design and its
  implementation in `core/src/crypto/`; (2) the vault and key handling; (3) server
  authentication and authorisation; (4) the C ABI boundary.
- Funding. An audit of this scope typically costs tens of thousands of dollars. Options:
  the Open Technology Fund's Security Lab, NLnet / NGI grants, the Sovereign Tech
  Agency, OSTIF-coordinated audits, or community funding. Applications take months, so
  begin when Stage 4 is complete, not when Stage 7 starts.
- If a paid audit is not obtainable in time: commission a smaller design-only review of
  the specifications, run a public review period with a call for comments posted to the
  relevant communities, and **keep the "unaudited" warning in the README**. The 1.0
  label then needs an explicit decision by the owner on whether to ship without a full
  audit.
- Triage and fix findings; publish the report and the response.
- A lightweight, ongoing vulnerability disclosure process: `SECURITY.md` (exists),
  response targets, a security advisory workflow, a hall of thanks.

Acceptance: an audit or design review report is published along with fixes for every
high and critical finding; the README's security statement matches reality.

### 7.5 Reproducible builds, signing and SBOM

Tasks:
- Reproducible builds: pinned compiler and vcpkg baseline in a container image,
  `SOURCE_DATE_EPOCH`, path prefix mapping, deterministic archives. A CI job builds
  twice on different runners and compares hashes for `cordedd` and `libcorded`.
- Document how a third party reproduces a release binary.
- Signing: release artefacts signed with Sigstore (cosign, keyless) and checksums
  published; SLSA build provenance generated by the release workflow.
- SBOM in SPDX or CycloneDX for each artefact, generated from the vcpkg manifest.
- Licence compliance: collect third-party licence texts into `THIRD-PARTY-NOTICES`
  shipped with every binary; check compatibility with Apache-2.0.
- Dependency update policy: monthly baseline bump, immediate bumps for security fixes in
  OpenSSL, libsodium and SQLite, with a documented point-release process.
- Protected release process: tags signed, releases cut only from `main` by workflow,
  required reviews on workflow changes.

Acceptance: two independent builds of a tag are bit-identical on Linux for the server
and the library; a user can verify a download with one documented command.

### 7.6 Release packaging

Tasks:
- Server:
  - Static binaries for Linux x86_64, ARM64 and ARMv7.
  - `.deb` and `.rpm` with the systemd unit and a dedicated user.
  - OCI image (scratch based, multi-architecture) and a compose file.
  - An install script that only downloads, verifies and installs; nothing clever.
  - AUR package; a NixOS module if there is demand.
- Core library: tarballs and zips per platform; Debian `-dev` style split; Homebrew
  formula; vcpkg port so other C++ projects can depend on it.
- TUI: same channels as the library; a single-file static build for Linux.
- GUI: AppImage and Flatpak for Linux, a signed and notarised macOS bundle, a signed
  Windows installer, an Android APK for direct download. Store submissions are out of
  scope for 1.0.
- Bindings: already published in Stage 6; align versions with the 1.0 core.
- Upgrade path: server database and client vault migrations tested from every released
  version; a documented downgrade policy (not supported; back up first).
- Release checklist and a fully automated release workflow driven by a tag.

Acceptance: on a fresh Raspberry Pi OS install, a server is running and reachable in
under five minutes following the quick-start; each client package installs and starts on
a clean system.

### 7.7 Operator and user documentation

Tasks:
- A documentation site (static, built from `docs/`, published to GitHub Pages).
- **Operator guide.** Quick start; configuration reference; TLS with Let's Encrypt and
  with a self-signed certificate; running behind a reverse proxy or on a home connection;
  invites and user management; backup and restore; upgrades; monitoring; sizing guidance
  from the benchmarks; hardening checklist.
- **What your server can see.** A plain-language page for operators and their users,
  taken from the metadata audit.
- **User guide.** Installing a client; creating an account; what the passphrase protects
  and that it cannot be recovered (gap G-19); joining a server; verifying contacts and
  what a key-change warning means; what deletion and disappearing messages do and do not
  guarantee.
- **Developer documentation.** Architecture overview; protocol and crypto
  specifications; ABI reference; binding guide; how to add a feature; contribution guide.
- **Security page.** Threat model summary, audit reports, known limitations, how to
  report issues.
- An FAQ covering the honest comparisons people will ask for (Signal, Matrix, others).
- Review all documentation for claims the software does not meet.

Acceptance: a person with general Linux skills but no knowledge of Corded sets up a
server and two clients from the documentation alone, and can explain what the server
operator can see.

### 7.8 Protocol freeze and 1.0

Tasks:
- Protocol review: go through every frame and event type and remove or mark experimental
  anything not ready to be supported long term.
- Freeze protocol version 1: tag the specification, commit the final schemas as the
  compatibility baseline, and turn on the rule that version 1 clients and servers must
  interoperate indefinitely.
- Compatibility tests: the 1.0 client against the 1.0 server becomes a permanent CI
  fixture that all later versions are tested against.
- Publish Corded's test vectors and the specification as a versioned document so
  independent implementations are possible.
- Define the support policy: which versions get security fixes and for how long.
- Release candidates: at least two, each used for real by a small group for a couple of
  weeks, with a tracked bug bar (no known data-loss, security or crash-on-start bugs).
- Replace the README's pre-alpha warning with an accurate status and security statement.
- Write the 1.0 release notes, including known limitations.
- Plan what follows: remaining feature milestones, MLS, sealed sender, post-quantum key
  agreement, mobile push, federation (a non-goal for 1.0, to be reconsidered).

Acceptance: milestone M7. Version 1.0.0 is tagged, signed, reproducible, packaged and
documented, with an audit or review report published.

## Test plan

This stage is largely testing. Beyond the steps above:

- Long-running soak of a real deployment: one server on a Raspberry Pi, several clients
  of different kinds, in daily use for at least a month before 1.0.
- Upgrade tests across every pre-release version still in use.
- Disaster tests: restore a server from backup; recover a client after disk-full,
  power loss during write, and clock reset.
- A final run of every suite (unit, integration, conformance in all languages, fuzz
  regression, benchmarks) on the release commit, recorded in the release notes.

## Risks and open questions

- **Audit funding and scheduling** is the critical path and partly outside the project's
  control. Start applications early; have the fallback in 7.4 ready.
- **Findings that require protocol changes.** Possible, which is why the freeze in 7.8
  comes after the audit, not before.
- **Reproducibility on macOS and Windows** is harder than on Linux. Linux
  reproducibility is the requirement; the others are best effort and documented.
- **Scope creep.** Feature milestones compete for attention with hardening. The rule:
  nothing that changes the wire protocol merges after the first release candidate.
- **Open:** whether 1.0 may ship with a design review instead of a full audit.
- **Open:** whether multi-device (F7) must be in 1.0. Recommended yes, because adding it
  later affects user expectations more than code.

## Definition of done

- All eight steps meet their acceptance criteria.
- Gap G-19 is documented for users; decisions revisited in 7.3 are updated.
- Protocol version 1 is frozen and published.
- Milestone M7.
