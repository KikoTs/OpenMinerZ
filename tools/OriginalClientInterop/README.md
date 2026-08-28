# Original-client interoperability lab

This optional Windows-only lab starts an explicitly staged, untouched original Castle Miner Z
assembly with that client directory as its working directory and supplies the normal Steam app
identity. It does not bypass Steam authentication and is excluded from ordinary OpenMinerZ
builds. When the managed state host is staged, the supervisor launches that x86 host and the host
invokes `CastleMinerZ.exe` in-process; without it, the supervisor retains its direct-executable
fallback.

The supervisor starts the managed host without selector arguments because the original reads the
process command line independently of its reflected `Main(string[])` parameter. `--game` remains
available only for the deterministic fixture and other non-original test assemblies.

Enable `OPENMINERZ_BUILD_INTEROP_LAB` only for the future protocol-compatibility lab. The target
is `openminerz_interop_launcher`, and its output is kept under `out/interop-lab`.

The supervisor stays alive with the game and publishes
`OpenMinerZInterop.endpoint.json`. Its named pipe rejects remote clients and its ACL grants
access only to the launching Windows user and SYSTEM. Commands and replies are bounded and
versioned; screenshot names cannot escape the staged `InteropCaptures` directory, and state
names cannot escape `InteropStates`.

Screenshot capture tries `PrintWindow`, then the window DC. A desktop fallback is allowed only
after Windows confirms the target became the foreground window. Uniform client frames are
rejected, preventing a blank D3D surface or an occluding app from producing a false pass.

```powershell
pwsh ./Interop-Control.ps1 -Endpoint '<staged-dir>/OpenMinerZInterop.endpoint.json' `
  -Command status
pwsh ./Interop-Control.ps1 -Endpoint '<staged-dir>/OpenMinerZInterop.endpoint.json' `
  -Command capture -CaptureName original-barrier.bmp
pwsh ./Interop-Control.ps1 -Endpoint '<staged-dir>/OpenMinerZInterop.endpoint.json' `
  -Command dump-state -CenterX 0 -CenterZ 0 -StateName original-barrier.json
pwsh ./Interop-Control.ps1 -Endpoint '<staged-dir>/OpenMinerZInterop.endpoint.json' `
  -Command quit
```

`quit` posts the original window's normal close request; the supervisor never forcibly
terminates the real game. Deterministic fixtures prove launch, status, window capture, state
capture/forwarding, and graceful shutdown without initializing Steam.

The opt-in build also produces `out/interop-managed/Host/OpenMinerZ.OriginalStateHost.exe`.
It is PE32/x86 and loads only a same-directory executable selected with `--game`. Its own named
pipe is ACL-limited to the current user and SYSTEM and uses `PIPE_REJECT_REMOTE_CLIENTS`. The only
semantic request is a bounded fixed-region state dump. The adapter reads the exact original
Modern block packing (`type = (raw & 0x7ffff000) >> 12`), inventory/tray layout, player roster,
health/death, enemy/pickup counts, and dragon state. It never serializes gamer tags, Steam IDs,
endpoints, item names, or waypoint labels.

`Test-ContractProbe.ps1` loads but never invokes the owned Classic and Modern assemblies. It pins
their SHA-256/version/profile, required reflection members, and Modern block constants 69/70, so
a fixture cannot accidentally certify the wrong runtime namespace or layout.

For the OpenMinerZ half of a private compatibility run, launch the normal game
with `--network-trace <path-to-session.jsonl>`. The file is append-only and
contains timestamped decoded message metadata and lifecycle outcomes only. It
never contains packet payloads, Steam/lobby IDs, gamer tags, passwords, or
endpoints. The original-client launcher remains a process/window supervisor only; it does not
inject into or modify the staged client.

Add `--state-dump <path-to-barrier.json> --state-region <X,Z>` to the OpenMinerZ
process to write its final comparable state: player transform/health, semantic
inventory/edit hashes, entity counts, and an exact 33x128x33 block-region
fingerprint at the same world-space center on both machines. An explicit region
fails closed unless the whole volume is resident; omitting `--state-region` is
reserved for convenient player-centered diagnostics. This is the
The original side emits the same v1 document through the managed host. Original global edit-
journal evidence is deliberately `null`: the loaded 384x128x384 terrain is observable, but all
persistent chunk deltas are not yet available as one atomic collection. Never treat this as an
empty edit list. Use the exact shared fixed region to cover scenario edits.

Audit the latest appended trace session before comparing state:

```powershell
pwsh ./Audit-Trace.ps1 -Path openminerz-client.jsonl -Profile Gameplay `
  -OutFile client-trace-certificate.json
```

The auditor accepts only the payload-free v1 field set, validates bounded values,
contiguous sequence numbers and monotonic clocks, and proves the ordered admission,
world bootstrap, presence, inventory, delta-list, and bidirectional player-update
lifecycle. Any failure record fails; rejection records fail unless explicitly
allowed for a hostile-input safety run.

Once both sides have emitted the same `openminerz.interop-state.v1` schema, run:

```powershell
pwsh ./Compare-State.ps1 -Left host.json -Right client.json `
  -AllowRegionOnlyEdits -OutFile certificate.json
```

The comparator requires ready host/client phases, an exact 33x33 horizontal region,
exact protocol/world/entity state, identical fixed-region bounds and hash,
and the same GID roster. Replicated
player transforms use bounded position and angle tolerances; death state remains
exact, and health is compared when both sides possess it. It exits nonzero and
emits stable mismatch codes when the barrier diverges.

Without `-AllowRegionOnlyEdits`, either side lacking a global edit hash fails closed with
`GlobalEditEvidenceUnavailable`. Certificates explicitly record whether global edit evidence was
available and whether the region-only exception was enabled.
