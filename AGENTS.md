# Working on OpenMinerZ

Read README.md first. Use docs/development.md for the source map and tests,
docs/modding/README.md for add-ons, and docs/releasing.md for distribution.
This file applies to the whole repository.

## Find the implementation

- Public contracts: `include/openminerz/`; implementations: `src/`.
- Runtime integration: `src/app.cpp` and its `app_*.inc` files.
- Luau API: `src/modding/<domain>/api.cpp`; matching service code owns behavior.
- Add-on discovery, permissions and lifecycle: `src/modding/` and
  `src/app_mod_*.inc`. Network authority is in `src/net/`.
- Examples: `examples/mods/`. Copy a complete example before inventing a format.
- SDK: `tools/ModSdk/`; generated API/types: `docs/modding/api-reference.md`
  and `tools/ModSdk/types/`.
- Weapon authoring: `make_tactical_animations.py`, `tactical_authoring.py`,
  `rig_lab.py`, and `rig_lab/` under `tools/ModSdk/`.

## Editing rules

1. Inspect the working tree. Preserve unrelated user changes; do not reset or
   clean the checkout to simplify a task.
2. Implement at the owning service and add the runtime adapter when needed.
   A declaration or a successful SDK call alone does not prove in-game behavior.
3. Look up APIs in the generated reference or registry. Do not invent function
   names, parameter fields, capabilities or event payloads.
4. Keep gameplay authoritative on the server. Client graphics and animation
   changes must not alter aim, damage, inventory, terrain or gameplay timing.
5. Run targeted tests. Use `openminerz_mod_sdk validate --strict`, its headless
   runner and the relevant CTest suite for add-on changes. Visually inspect
   animation/UI work; numerical contact tests alone cannot establish good motion.
6. After API changes, run `openminerz_mod_sdk reference --repo .`, then
   `reference --repo . --check`; include generated type/reference updates.
7. Store temporary scripts, screenshots, logs and reports in `out/`. Update the
   maintained docs instead of adding handoff files, progress diaries or duplicate
   implementation summaries.

## Assets and releases

Import owned game assets with `tools/Import-GameAssets.ps1`. Never copy imports,
reference exports, game/Steam DLLs, avatars, saves or captures into source or
example packs. Authored example assets and licensed recordings are explicitly
listed in `tools/release-assets.json`; retain their credits. Changing this
manifest requires inspecting provenance, not merely accepting a new hash.

Use `py -3 tools/release.py --history` before preparing a release. Build public
packages through `tools/Package-Release.ps1`, not by zipping `out/` or the local
playable directory. Do not publish, tag or rewrite Git history unless requested.
The project license is AGPL-3.0-or-later; third-party notices are separate.
