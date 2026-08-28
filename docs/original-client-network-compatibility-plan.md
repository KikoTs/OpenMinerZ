# Original-client network compatibility plan

Status: protocol, admission, Steam transport, live host/join bootstrap, and the
complete gameplay registry are implemented in the normal build. Gameplay is
also generation-gated behind the accepted `PlayerExists` phase, so disconnect
revokes the old GID before reconnect can announce a fresh one. Private two-machine
role-reversal certification remains the release gate.

The current implementation and its stock-client safety boundary are recorded in
[`multiplayer-foundation.md`](multiplayer-foundation.md).

## Compatibility target

An unmodified CastleMiner Z Steam client and OpenMinerZ must be able to join the same private
session, exchange authoritative world/player state, and complete a small deterministic test
scenario. The certificate requires two separate Steam identities on two machines and both host
roles. Public matchmaking, relay/NAT traversal, moderation, and host migration remain outside
that certificate. Castle Miner Z's dormant voice helper is preserved as a validated/relayed but
application-silent message path because the game never calls `StartVoiceChat`.

This is a clean-room interoperability effort against the user's locally installed client and
readable source. No proprietary assets, service credentials, or original binaries are shipped.

## What the source already establishes

- The Steam game name is `CastleMinerZSteam` and the network version passed by the frontend is
  `3`.
- Session creation/joining uses DNA's `NetworkSession` abstraction. This build contains the
  Lidgren provider plus Steam lobby/host identifiers, so session admission and gameplay packets
  must be treated as two distinct layers.
- A gameplay message is `message-id byte + BinaryWriter payload + checksum byte`.
- Reliability is selected per message (`None`, `Reliable`, `InOrder`, `ReliableInOrder`, or
  `Chat`).
- Message IDs are assigned at runtime from the reflected concrete `Message` type array. They
  must be dumped from the exact original assembly; alphabetical guesses are not acceptable.
- There are 57 game-specific message source files in `CastleMinerZ/Net`, in addition to DNA
  session/control traffic.

## Offline-stability gate

Protocol implementation starts only when these are green:

1. Menu/music/ambience, save/load, movement, mining, inventory/crafting, combat, enemies,
   explosives, and death/respawn have deterministic regression coverage.
2. A 30-minute local survival soak has no crash, stuck input, corrupt save, or unbounded entity
   growth.
3. The D3D11 path has no known depth leaks for terrain, entities, decals, or particles.
4. The remaining offline divergence list is triaged into blocking versus cosmetic items.

## Research and implementation stages

### 1. Produce an exact protocol manifest

Build an opt-in developer harness for the locally installed original client. At process start it
will write, to a user-local artifact directory:

- concrete message type -> runtime byte ID;
- field serialization order and primitive width from every `SendData`/`RecieveData` pair;
- `SendDataOptions`, echo behavior, maximum observed size, and intended sender/recipient;
- session property indexes and meanings;
- join/world-bootstrap message sequence.

The manifest becomes a checked-in, asset-free schema document plus golden byte fixtures. The
normal game must never load or probe this harness.

### 2. Reproduce framing safely

Implement a bounded packet reader/writer in a separate `openminerz_protocol` library:

- explicit little-endian primitives and .NET `BinaryWriter` string encoding;
- checksum parity with `DNA.IO.Checksums`;
- hard packet/message limits before allocation;
- unknown-message rejection without mutating world state;
- golden encode/decode tests for every supported message.

Start with chat/ping and player presence, then movement, block edits, inventory, damage, enemies,
world bootstrap, and finally explosives/dragons. Unsupported messages remain visible in traces
and fail closed.

### 3. Separate transport from game protocol

Define a small transport interface so protocol work can be tested without Steam:

- loopback/in-memory transport for deterministic tests;
- local UDP/Lidgren-compatible transport for packet parity;
- Steam lobby/P2P adapter only after the private LAN handshake is understood.

Preserve each original message's reliability/order class. Do not silently send everything as
reliable; that changes timing and head-of-line behavior.

### 4. Build the two-client lab

The developer controller will expose local IPC commands such as `launch`, `host-private`,
`join-lobby`, `load-world`, `set-position`, `equip`, `act`, `capture`, `dump-state`, and `quit`.
MCP can wrap this IPC later, but the test protocol itself should remain a small documented local
API that also works from CI.

The safe controller and read-only state slice is implemented in the optional Windows lab. The
native supervisor launches either the untouched staged executable directly or the staged x86
managed state host under Steam's normal identity, publishes a current-user/SYSTEM-only local
named-pipe endpoint, and supports bounded `status`, `capture`, `dump-state`, and graceful `quit`
commands through `Interop-Control.ps1`. The managed host invokes the untouched original assembly
in-process and reflectively snapshots the original runtime without injection or binary changes.
Native-window, managed-state, and combined forwarding fixtures exercise the full control path
without Steam. Scripted gameplay-control commands remain a later convenience, not a prerequisite
for manual role-reversal barriers.

Each scenario records:

- timestamped packet directions and decoded message names (implemented on the
  OpenMinerZ host/client boundary through the opt-in, payload-free
  `--network-trace <path>` JSONL recorder);
- authoritative snapshots from both clients (OpenMinerZ uses `--state-dump <path>` and the
  original uses `dump-state`; both include transform/health, a semantic inventory hash, entity
  counts, and an exact fixed 33x128x33 block-region fingerprint at the same center);
- screenshots at synchronization barriers;
- world hash, player transform/health/inventory, and block-edit journal.

The recorder and state dump deliberately exclude raw packet bytes and all
persistent Steam, lobby, gamer, password, endpoint, and free-form diagnostic
data. The original adapter deliberately emits `null` for the global edit hash/count because the
complete persistent delta journal is not atomically observable. The comparator fails closed on
that absence unless the private test explicitly supplies `-AllowRegionOnlyEdits`; the exact
fixed region then remains the edit evidence for the bounded scenario.

`tools/OriginalClientInterop/Compare-State.ps1` consumes two version-1 state
barriers and emits a versioned pass/fail certificate. World/edit/entity fields
and death state compare exactly; matching GID transforms use explicit bounded
position/angle tolerances, and a non-host/client role pair fails closed. Its certificate records
whether global edit evidence was present or the explicit region-only exception was used.

`tools/OriginalClientInterop/Audit-Trace.ps1` consumes the latest session in an
append-only trace, rejects extra/leaky fields and malformed or non-monotonic
records, and proves the role-specific admission -> bootstrap -> presence ->
inventory -> delta-list -> gameplay ordering. Its gameplay profile requires an
accepted player update in both directions; safety runs may opt into expected
rejections but can never certify a transport failure.

The controller uses a disposable original-client copy or injected launch configuration only in
the developer lab. It never becomes a runtime dependency or user-facing brand/path.

### 5. Compatibility milestones

1. Discover/join a private original-hosted session and exchange chat.
2. Spawn both players and keep transforms synchronized for five minutes.
3. Replicate one block place/dig in each direction.
4. Synchronize inventory pickup/drop and damage/death/respawn.
5. Join an original world bootstrap and verify a fixed region hash.
6. Exercise zombies, guns, rockets, TNT/C4, and a dragon with state hashes.
7. Run a 30-minute two-client soak with packet loss/reordering injection.
8. Reverse roles: OpenMinerZ host, unmodified original client joins.

## Capture and validation rules

- Prefer source-derived schemas and runtime message-table dumps over blind packet inference.
- Use loopback/private sessions only until parsers survive fuzzed lengths, IDs, and ordering.
- Redact Steam IDs, lobby IDs, gamer tags, IPs, passwords, and tokens from committed artifacts.
- Never automate sign-in, bypass service controls, or expose a public compatibility server during
  research.
- A milestone is complete only when both semantic state comparison and wire-level fixtures pass;
  a successful join screen alone is not compatibility.

## Known risks

- Reflected message ordering can differ across assembly builds; the exact installed assembly is
  authoritative.
- Steam lobby admission/authentication may be inseparable from the original service adapter.
  If so, original-host compatibility may require Steam P2P rather than raw UDP.
- Host authority is distributed across many game messages rather than one snapshot protocol;
  implementing only movement/block edits would create a client that joins but corrupts state.
- The original parser has legacy trust assumptions. OpenMinerZ must preserve wire behavior while
  enforcing modern bounds and validating sender authority.
