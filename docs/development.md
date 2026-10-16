# Development map

The runtime separates simulation, rendering, network authority and sandboxed
add-ons. Prefer changes in the owning service over adding another special case
to `app.cpp`. [AGENTS.md](../AGENTS.md) gives coding agents the same entry points.

| Area | Contracts / implementation | Useful tests |
| --- | --- | --- |
| World, terrain, player, enemies | `include/openminerz/world`, `src/world` | gameplay, terrain, simulation, dragon, far-terrain suites |
| Inventory and persistence | `inventory.*`, `player_progress.*`, `world_save.*` | gameplay, world-save, player-parity suites |
| Renderer and presentation | `world_preview_renderer.*`, `depth_voxel_renderer.*`, `src/vfx`, HUD/menu/screen files | graphics, HUD, presentation, interface-style suites |
| Networking and authority | `include/openminerz/net`, `src/net`, `src/app_mod_net.inc` | multiplayer, host/join, latency, desync and mod-net suites |
| Add-on services and API | `include/openminerz/modding`, `src/modding` | `test_mod_*.cpp` |
| Runtime adapters | `src/app.cpp`, `src/app_*.inc` | integration tests plus in-game verification |
| SDK and example assets | `tools/ModSdk`, `examples/mods` | SDK validation/runner, mod-examples tests |
| Owned-content decoding | `tools/ContentBridge`, `retail_rig.*`, `xbox_avatar.*` | rig/avatar tests with private imports |
| Release/import boundary | `tools/Import-GameAssets.ps1`, `tools/release.py`, `tools/Package-Release.ps1` | release tests and an isolated import |

## Add or change a Luau API

1. Read the matching domain's `api.cpp`, service implementation and tests.
2. Add the service behavior, capability/side checks, bounds and lifetime handling.
3. Bind the function in the registry with its parameter/event documentation.
4. Wire the real game adapter; headless services alone do not make a feature work
   in the game. Preserve no-add-on behavior.
5. Test positive use, invalid inputs and the wrong VM side. For networking,
   validate server authority, sender identity, ordering and reconnect behavior.
6. Regenerate the reference/types, then run the matching suites:

```powershell
$sdk = '.\out\build\native-release\Release\openminerz_mod_sdk.exe'
& $sdk reference --repo .
& $sdk reference --repo . --check
ctest --preset native-release -R openminerz_mod --output-on-failure
```

The SDK introspects the registered runtime API. Do not hand-edit generated Luau
definitions or JSON to advertise an API that does not exist.
Documentation Luau blocks have side tags and are executed by the mod-examples
test; keep runnable examples in `docs/modding/`.

## Compatibility work

Classic/Modern identifiers, wire contracts and gameplay tables are compatibility
source. The retail probes under `tools/OriginalClientInterop/` and
`tools/OriginalParityProbe/` compare behavior against owned clients. They are
optional and should write evidence below ignored `out/`, never into example packs.
Do not update pinned expectations solely to hide a regression.

For protocol changes, run codec/authority tests and private role-reversal sessions
with consenting testers. Offline tests do not establish real Steam compatibility.
Do not use public players as automated test subjects or attach personal IDs,
worlds, packets or screenshots to public issues.

## Validation and reporting

Use CTest's `-N` to list the suites available in the configured build, then run
tests appropriate to the change. A full release build runs the complete registered
suite. Python authoring tests run with unittest; Rig Lab's state/skin checks use
Node. Visual changes require visual checks in addition to numerical tests.

Explain what changed, how it was checked and what remains untested. Keep local
logs, recordings and scratch reports under `out/`; maintain these docs rather
than accumulating handoff notes. Do not commit a developer's absolute paths,
build output, imported assets or user profile.

Report security issues through the repository's private security advisory form,
not a public issue containing an exploit or player data. A normal bug report
should include revision, reproduction steps, settings and a sanitized log.
