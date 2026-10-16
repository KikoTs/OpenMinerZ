# Player animation and Rig Lab

Animation packs are client add-ons. They change rendered motion and sound, not
reload duration, damage, aim or hit detection. Start with `animation_pack` for a
small example or `tactical_animations` for weapon parts and hand-contact editing.

## Inspect and export a rig

Build the SDK and import your owned game assets first:

```powershell
$sdk = '.\out\build\native-release\Release\openminerz_mod_sdk.exe'
& $sdk export-rig --out out/authoring/rig.glb --modern
& $sdk check-anim out/authoring/my-animation.glb --clips --strict
```

Use `--classic` for a Classic rig. `--reference-clips all` adds original reference
motion for local comparison; those clips have `REF_` names and must never ship.
The export, meshes, skins and rest-pose data are private authoring references.

Author rotations on the named bones; the runtime uses its imported rig. The
authoring convention is +Y up, character forward +Z, left hand on +X, one world
unit per block. Preserve bone names and parent relationships. Export only your
authored tracks to the pack's `assets/animations/` folder, not the reference
mesh, skin, imported offsets or `REF_` clips. The SDK rejects reference clips.

`anim` registers replacement clips. `gunfx` defines moving weapon parts, timed
cues and presentation motion. Use the exact contracts in the
[API reference](api-reference.md) and the shipped
[`tactical.luau`](../../examples/mods/tactical_animations/client/tactical.luau).
Weapon part selectors reference the locally imported model; they do not include
the original gun geometry in the add-on.

## Start Rig Lab

```powershell
.\rig-lab.bat
```

The launcher prepares local reference assets and opens `http://127.0.0.1:8731/`.
It needs Python 3.10+, NumPy, Pillow, the built SDK and your imported game files.
Three.js is bundled; the editor does not need a CDN. Close its terminal to stop
the server. Restart the server after changing its Python source.

## Fit a hand

1. Choose the weapon, animation and skin. Check both the stock character and
   Xbox avatar if your pack is intended for both.
2. Choose **This weapon's holding poses** to adjust the support grip through
   idle, movement, aiming and matching reload entry/return contacts. Magazine,
   pouch and mechanical contacts remain separately authored.
3. Drag the 3D move/rotate gizmo or type the position/rotation values. The arm
   updates live before saving. **Fingers & thumb** controls curl, handle shape,
   spread and thumb opposition.
4. Use **This animation key** for a particular reload contact. Select a timeline
   diamond, adjust its time/position, or add a key at the cursor. Retiming moves
   the key rather than leaving its old copy behind.
5. Use **Weapon & right hand** for weapon motion keys and **Handle inside right
   hand** to fit a hilt/handle across that weapon's clips. Knife clips select
   this mode because their mask does not animate the left hand.
6. Check first person and external views. **Focus hand** zooms the selected hand;
   orbit/zoom to inspect the grip. Hide the inspector for more viewport space.

Position fields display millimetres and rotations display degrees. The timeline
uses source-clip seconds; reload playback scales to the weapon's actual duration.
Loop at 0.1x/0.25x/0.5x or step frames to check contact throughout the motion.

Shortcuts: W move, E rotate, F focus, Space play/pause, Left/Right step,
Ctrl+Z/Ctrl+Y undo/redo, Ctrl+S save. Snap uses 1 mm / 5 degrees.

## Undo, reset and recover

A drag is one undo step. Reset a key to remove that override; reset an animation
to remove its local edits while retaining linked grip changes. Reset a weapon
to clear its linked and clip edits, or reset the whole project to supplied poses.
Resets can be undone. Saved versions can be restored as a draft before saving.

The browser offers recovery after an unsaved refresh. It does not silently apply
a stale draft over a newer disk save. Durable history and exported rig data stay
under ignored `out/rig-lab/`. Editable project data is
`tools/ModSdk/tactical_authoring.json`; back it up before broad experiments.

**Save project** saves the authoring profile. **Build & install** saves,
generates, validates and installs the pack in the source example and the build
used by `play.bat`; a failed export keeps the previous installed pack. Restart
the game to load the newly built animation files.

## Validate before sharing

```powershell
& $sdk validate --strict examples/mods/tactical_animations
py -3 -m unittest discover -s tools/ModSdk -p 'test_*.py'
node tools/ModSdk/rig_lab/test-editor-state.mjs
node tools/ModSdk/rig_lab/test-rig.mjs out/rig-lab/scene/rig.json
```

The editor uses the generator's solve and the game's skinning data. Tests check
reach, wrist correction, interpolation and carried-part continuity. A zero palm
gap does not prove a natural-looking grip: inspect fingers, magazine seating,
sights and transitions, and test ordinary playback in game. Editor lighting and
ground-prop behavior are not the full game's renderer/physics.

After modifying authored media, review its provenance before updating
`tools/release-assets.json`. Keep `AUDIO_CREDITS.txt` and `AUDIO_SOURCES.json` with
Tactical Animations; its recorded foley retains the named creators' licenses.
