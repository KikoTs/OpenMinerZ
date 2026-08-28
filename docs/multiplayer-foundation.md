# Multiplayer foundation

This document records the implemented boundary between the OpenMinerZ client,
the source-derived CastleMiner Z protocol, and the Steam/loopback transports.
It is intentionally explicit about what is live and what is not yet claimed.

## Implemented

- `openminerz::net::message_manifest` contains the exact reflected registry
  order from the deployed Classic-v3 (53 messages) and Modern-v4 (55 messages)
  profiles. Each entry carries its original reliability class and echo rule.
- Stock application frames use the original one-byte message ID, payload, and
  XOR8 checksum. Frames outside the 2-4096 byte envelope, unknown IDs, and bad
  checksums fail before gameplay state can be reached.
- `BoundedReader` and `BoundedWriter` provide explicit little-endian primitives
  and .NET `BinaryWriter` 7-bit UTF-8 strings with transactional read failure and
  caller-selected allocation limits.
- `HostSessionSettings` validates the dedicated-server compatibility limits:
  1-16 players, 128-byte lobby identity/message, 256-byte password, supported
  v3/v4 profile, bounded spawn protection, and a matching mod manifest.
- Session discovery properties reproduce the original eight nullable `Int32`
  slots: protocol, join state, mode, difficulty, infinite resources, PvP, and
  two unused null slots.
- `HostAdmissionGate` reserves capacity before handshake, validates platform
  policy, passwords without early-exit comparison, all eight session
  properties, and extension capability, and does not expose gameplay until an
  exact mod hello is accepted. Rejections do not consume player slots.
- `BoundedDatagramQueue` preserves peer IDs, delivery class, channel, and FIFO
  order while enforcing per-packet, packet-count, and aggregate-byte budgets
  before copying input. Native envelope checks require nonzero bounded advertised
  length, successful exact-length reads, and authenticated session membership.
- `LegacySteamWireCodec` reproduces the deployed four-byte Steam envelope and
  Lidgren bit-packed connection request, connected roster, new/drop peer,
  session-property, direct, client-broadcast, and proxy-direct layouts. Golden
  Classic-v3/Modern-v4 bytes and hostile truncation, length, delivery, reserved,
  identity-blob, and property cases are covered.
- `LegacySteamHostSession` implements the source denial order/text, constant-time
  password comparison, restricted-lobby authorization, stock-client mod safety,
  fresh GID allocation, host-first rosters, disconnect cleanup, sender/target
  ownership, packet and typed-payload validation, and authority-before-relay.
  Stock admission preserves the original four-byte little-endian `Int32` status
  value 5 before the connected roster; a one-byte approximation is rejected.
  No received gameplay packet produces a relay until the authority explicitly
  creates validated outbound traffic.
- `SteamBridge32` is a separately compiled Windows-x86 executable that loads the
  installed PE32 `steam_api.dll` dynamically and owns only the exact 2013 Steam
  ABI, lobby metadata/membership, and native P2P calls. The x64 game retains all
  approval, routing, application validation, and authority. The helper publishes
  `Name`, `Protected`, `JoinPolicy`, `Date`, and the present numeric session slots,
  maps stock join policies to native lobby types 2/1/0, polls membership every
  100 ms, accepts P2P only for current members, and preserves the deployed
  reliable/unreliable native-send split.
- `SteamBridgeClient` launches that helper suspended into a kill-on-close job and
  connects it through a GUID-named byte pipe whose DACL grants only the current
  user. Remote pipe clients are rejected; the helper verifies the server PID,
  parent process, and Windows session. Every frame has `OMSB` magic, protocol and
  message types, a 128 KiB payload ceiling, exact length, sequence, and CRC32.
  Typed payloads validate app ID 253430, identities, delivery/channel, reserved
  bytes, strings, eight session slots, and full consumption. Partial frames time
  out and missing/replayed sequences terminate the boundary.
- Source-compatible typed codecs cover every registered message in both
  deployed profiles: bootstrap/presence, movement, chat/voice, inventory,
  pickups/crates, chunks, blocks/doors/spawners, combat/explosives, enemies,
  dragons, time, and Modern custom blocks. Lengths, versions, item
  subtypes/stacks/clips, enum ranges, finite floating-point values, declared
  collection counts, unique world-object locations, and complete payload
  consumption are validated before dispatch. A manifest audit confirms all 55
  unique names have a typed branch; the unsupported fallback remains only as a
  defensive guard against future registry drift.
- `LoopbackSession` pairs host and peer endpoints through the real admission,
  extension, stock-framing, typed-payload, channel, and delivery gates. It keeps
  gameplay closed until admission completes, applies the original dynamic
  reliable/unreliable rule for `PlayerUpdate`, preserves packet metadata in
  both directions, and purges queued packets on disconnect/reconnect boundaries.
- `AuthoritativeBootstrapHost` adapts a deterministic C++ `WorldSaveData`
  snapshot into the original request sequence: WorldInfo, PlayerExists echo,
  inventory retrieval, modified-chunk list, and priority-preserving chunk
  replies. Peer phases reset on reconnect and out-of-order requests produce no
  response. Oversized crate dictionaries use the dedicated server's fallback:
  bounded WorldInfo without crates followed by reliable-ordered typed
  `ItemCrate` reconstruction. Relayed echoes retain the originating peer ID.
  The client reproduces `DNA.Net.Message.DoSend` local echo for its broadcast
  `PlayerExistsMessage`, because Steam correctly excludes the sender from the
  host's broadcast relay, and uses the original ten-byte default avatar blob.
- `LegacySteamRuntimeHost` now keeps ordinary application traffic closed until
  that exact peer/GID generation has completed its accepted `PlayerExists`
  announcement. Disconnect revokes readiness, reconnect requires a fresh
  source-ordered announcement, and a delayed packet carrying the retired GID
  cannot become the new player's state. The OpenMinerZ client likewise binds a
  remote `PlayerExists` gamer tag to the already-admitted Steam roster identity
  before accepting player updates or auxiliary player state.
- Host Online now opens a thirteen-row preflight screen for server name,
  protocol, player limit, visibility, password, join policy, PvP, anti-cheat,
  safe join, spawn protection, and mod playlist before continuing into the
  original game-mode/difficulty/world flow. Name and password fields use a
  focused text editor; passwords are never echoed in menu text or logs.
- Join Online now opens a responsive friend/public/LAN/direct browser and a
  separate session-details surface. Discovery records validate the original
  eight-property contract, profile, capacity, platform policy, password state,
  extension capability, and exact ordered mod manifest before enabling
  admission. Filters/search use stable indices; duplicate/zero lobby IDs,
  hostile direct endpoints, full sessions, malformed advertisements, and mod
  mismatches fail closed. The selected record produces the same
  `PeerAdmissionRequest` consumed by `HostAdmissionGate`; passwords are masked
  and never logged. The live Steam discovery/join path refreshes lobby metadata,
  runs that same preflight again immediately before joining, then installs the
  authoritative bootstrap snapshot into the running world.
- Host and client runtime pumps apply the complete deployed message registry to
  player, terrain, inventory, pickup, crate, combat, enemy, dragon, environment,
  and Modern custom-block state before any relay. Castle Miner Z itself never
  calls `DNAGame.StartVoiceChat`, so its `_voiceChat` field remains null: voice
  retains exact framing, validation, `Chat` delivery, echo, and relay behavior,
  but application playback/capture deliberately remains silent for 1:1 parity.
- The opt-in `--network-trace <path>` certification recorder sits at those
  validated runtime boundaries. It appends JSONL with wall/monotonic timestamps,
  role, direction, result, protocol, decoded message name, ephemeral GID route,
  delivery, channel, and packet length. It never writes payload bytes, Steam or
  lobby IDs, gamer tags, passwords, endpoints, or free-form diagnostics;
  rejection reasons are represented only by a one-way correlation fingerprint.
- The companion `--state-dump <path>` writes one redacted versioned JSON state
  at shutdown. It includes role/phase/profile, player transform/health, semantic
  inventory and edit hashes, entity counts, and a 33x128x33 block-region
  fingerprint. `--state-region X,Z` selects the identical fixed world-space
  center for both roles and refuses a partial/unloaded volume; omission keeps a
  player-centered diagnostic mode. Item labels and persistent platform/user
  identities are hashed or structurally absent. The snapshot also lists local
  and replicated transforms/death state by ephemeral GID so the included
  `Compare-State.ps1` barrier checker can correlate both views.
- The local `addons` catalog loads strict descriptors, rejects links and
  case-colliding paths, hashes complete directory trees with bounded SHA-256,
  and derives exact ordered Classic/Modern admission manifests at runtime.

## Mod negotiation and stock-client safety

Mods have three scopes:

- `client`: texture/UI/client presentation content; omitted from admission;
- `server`: entity/behavior/content that changes replicated simulation;
- `core`: a runtime/profile replacement that must match before gameplay.

Synchronized server/core entries are compared by exact list position, scope,
ID, version, and 32-byte content digest. Missing, extra, reordered, wrong-scope,
wrong-version, and wrong-content manifests produce distinct fail-closed results.
The private control packet is capped at 4096 bytes, carries monotonically
declared list indexes, .NET-style bounded strings, a nonce, and CRC32 corruption
detection.

The extension frame is not a Castle Miner Z gameplay message. A transport must
route it on the OpenMinerZ control channel only after discovery metadata has
advertised extension support. An unmodified original server therefore never
receives an unknown OpenMinerZ frame. An unmodified original client remains
eligible for a host with no synchronized mods; a modded host rejects a
capability-silent client at session admission before any gameplay packet is
forwarded.

Packet bounds, checksums, message IDs, sender membership, and ownership checks
are always-on safety invariants. The host UI's anti-cheat toggle controls later
gameplay policy only; it never disables parsing or admission safety.

## Evidence

- Release build: zero compiler warnings.
- Deterministic suite: 67/67 tests, including exact manifest boundaries,
  stock framing golden bytes, oversized/corrupt rejection, malformed 7-bit
  lengths, extension round-trip/corruption, ordered mod admission, stock-client
  fallback, session-property layout, slot/password/property admission, gameplay
  gating, bounded queue pressure, native envelope rejection, original-layout
  typed payload round trips, owned Classic/Modern WorldInfo and PINV golden
  binaries, hostile typed values, strict late-join ordering, saved-world delta
  grouping, large-crate replay, paired host/peer delivery, echoed sender
  identity, dynamic movement reliability, disconnect queue purging, complete
  combat/pickup/enemy/dragon/voice schema coverage, discovery contract
  validation, direct endpoint bounds, browser filtering, and browser-to-host
  admission handoff. This also includes exact legacy Steam approval/routing
  fixtures, stock host admission/GID/reconnect/authority behavior, typed
  pre-presence rejection, retired-GID rejection after reconnect, fresh-GID
  resumption, bridge CRC and typed-payload corruption/bounds, restricted-policy
  fail-closed behavior, timestamped decoded JSONL trace persistence, semantic
  inventory/edit/fixed-region state fingerprints, explicit payload/identity/
  secret non-disclosure, and a
  real offline x64/x86 certificate that creates the secured pipe, launches the
  supervised x86 PE, exchanges sequenced hello/heartbeat/stop frames, and exits
  without initializing Steam or creating a lobby.
- Visual checks:
  `artifacts/validation/host-setup.bmp` at 1920x1080 and
  `artifacts/validation/host-setup-minimum.bmp` at 960x540, plus
  `artifacts/validation/join-browser.bmp`,
  `artifacts/validation/join-browser-minimum.bmp`, and
  `artifacts/validation/join-details-minimum.bmp`.
- Diagnostic launch routes: `openminerz --host-setup --screenshot <path>`,
  `openminerz --diagnostic-join-browser --screenshot <path>`, and
  `openminerz --diagnostic-join-details --screenshot <path>`, plus
  `openminerz --diagnostic-online-loading --screenshot <path>`. The user-facing
  `--join-browser` and `--join-details` switches always query live Steam lobbies.
  Add `--network-trace <jsonl-path>` to a host, join, or Steam probe invocation
  to append the redacted certification stream; add `--state-dump <json-path>`
  plus `--state-region X,Z` to write the final semantic comparison barrier.
  `Audit-Trace.ps1` validates the latest appended session's exact field schema,
  sequencing, monotonic clocks, admission, ordered bootstrap, and bidirectional
  gameplay traffic. The lab comparator emits a versioned certificate and fails
  nonzero on readiness, region extent, role, world, roster, transform, or
  replicated-state divergence.

## Not yet certified

- the full private two-machine matrix with an unmodified original client and
  OpenMinerZ exchanging chat, movement, bidirectional edits, pickups,
  inventory, damage/death/respawn, enemies, explosives, dragon state, and a
  fixed-region late-join hash;
- the required role reversal (original host/OpenMinerZ client, then OpenMinerZ
  host/original client) and a 30-minute hostile-packet/loss/reordering soak.

The owned PE32 `steam_api.dll` is copied beside every Windows Release build and
has also been staged in the current deployment directory. The remote test node
must be reachable and its exact install directory must be resolved before that
binary or the build is uploaded; deployment never guesses a server path.

No public compatibility or stability claim should be made until those gates
are complete and the original/OpenMinerZ role-reversal playtests pass.
