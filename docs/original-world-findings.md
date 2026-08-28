# Original world and sky compatibility notes

This document records the clean-room contracts recovered from the locally
installed CastleMiner Z 1.1.3 build. It is a behavior map for the new native
implementation, not copied game source. Proprietary content remains in the
installed game or in a disposable per-user derived-data cache.

## Coordinate and chunk contract

- Terrain chunks cover `16 × 128 × 16` cells.
- The original keeps a 24×24 chunk memory window centered around the eye. Its
  five graphics levels map to a 4–12 chunk draw radius, and movement recenters
  the window instead of exposing a finite edge.
- The active vertical range begins at world Y `-64`, so generator-local Y 66
  is world Y 2.
- Water is a renderer-owned plane at world Y 1.5.
- The world seed initializes the legacy .NET Framework `Random` sequence.
  Perlin and integer noise use 256 generated values repeated into a 1024-entry
  lookup table; this is intentionally not a shuffled permutation.

## Radial world bands

Distance from the origin is folded every 4400 blocks and mirrored in odd
bands. The outward sequence is:

| Radius | Surface pass |
|---:|---|
| 0–200 | Classic |
| 200–300 | Classic → Lagoon |
| 300–900 | Lagoon |
| 900–1000 | Lagoon → Desert |
| 1000–1600 | Desert |
| 1600–1700 | Desert → Mountain |
| 1700–2300 | Mountain |
| 2300–2400 | Mountain → Arctic |
| 2400–3000 | Arctic |
| 3000–3600 | Descent into the hell layer |

After the surface pass, the original ordering is hell ceiling, crash sites,
caves, ores, hell floor, bedrock, origin tower, then trees. Ordering matters:
later passes deliberately replace results from earlier passes.

Implemented native passes now include Classic, Lagoon overhangs, Desert,
Mountain ridges and snow line, Arctic, Descent, caves, coal/copper/iron/gold/
diamond/lava deposits, crash-site space rock and slime, hell ceiling/floor,
bedrock variance, the origin tower, and noise-shaped trees.

## Block and texture contract

The original block IDs are preserved from Empty 0 through Space Rock
Inventory 44, with 45 retained as the uninitialized/count sentinel. This is
the future save and network compatibility boundary.

`Content/Terrain/Textures.xnb` contains three 1024×1024 textures. The first is
an 8×8 diffuse/alpha atlas. Important tile assignments include dirt 0, grass
side 1, grass top 2, sand 3, lantern 4, rock 5, ores 6–10, lava 11, bedrock 12,
snow 13, ice 14, log side/end 15/16, leaves 17, wood 18, and blood stone 19.
The other two sheets are the matching normal/specular and metal/light atlases;
they must be sampled with the same face-local UVs as the diffuse atlas. The
original `BlockVertex` orders its four UV corners independently for ±X, ±Z,
top, and bottom faces. Applying one generic quad order rotates the grass strip
onto the side of ±X walls and likewise misorients every directional texture.

The Windows-only content bridge asks the installed XNA runtime to deserialize
these XNB files and decodes Color/DXT1/DXT3/DXT5 surfaces. The CMake import
target writes isolated Classic and Modern derived data under
`out/build/<preset>/content-packs/<style>/DerivedData`, beside each generation's
complete source `Content` tree. It also derives all six faces of the Clear,
Dawn, Sunset, and Night sky cubes. These ignored build assets are never placed
in the repository source tree.

## Day/night contract

- A full original day lasts 16 real minutes.
- A new sky starts at day fraction 0.41.
- Sky selection uses Night through hour 5, Night→Dawn at 6, Dawn at 7,
  Dawn→Clear at 8, Clear through 17, Clear→Sunset at 18, Sunset at 19,
  Sunset→Night at 20, then Night.
- Fog, ambient, direct, and specular colors transition on the same hourly
  boundaries.
- The HUD clock uses 12-hour time and quantizes minutes downward in 15-minute
  steps.

The playable world defaults to the original 1× cycle. `T` switches paused,
1×, 24×, and 120× for review; left/right step one hour. `Q` drops the selected
item and `E` opens inventory.

## Player and construction contract

- The standing collision box is 0.70 blocks wide and 1.70 blocks tall, with
  the eye point 1.40 blocks above the feet.
- `FPSRig.Speed` initializes to 5, but `Player.UpdateAnimation` overwrites normal
  grounded movement to 4 blocks per second. Jump impulse is 10 and gravity is
  −20 blocks per second squared; at the fixed 60 Hz update this produces a
  2.416667-block apex above takeoff.
- The recovered gameplay camera uses a 73-degree field of view. The native
  settings expose this as a normalized high-resolution vertical-FOV control.
- Construction traces start at the eye, follow the camera forward, and stop at
  5 blocks. Placement uses the empty neighbor immediately before the hit block
  and rejects blocks intersecting the player body.
- Automatic one-block slope/step climbing remains settings-driven, matching
  the original gameplay option.
- Rendered block identity and player collision are separate contracts. Leaves,
  torches, open doors, and lava remain visible and targetable but do not form
  full player-collision cubes. Vanilla marks surface lava as blocking while
  deep lava is pass-through; Rebirth deliberately makes both lava variants
  pass-through.

The original grounds strafe velocity in `FPSCamera.LocalToWorld.Right`; forward
and right therefore remain perpendicular at non-cardinal yaw. Its simulated
step side raises the player at 5 blocks per second and caps the feet at the
block top plus 0.001. The native collision step stays deterministic while its
camera transition recovers at the same 5-unit rate, avoiding an instantaneous
one-block eye snap and preventing a diagonal update from climbing twice.

The native implementation advances this contract at a fixed 60 Hz, resolves
the player AABB against solid voxels, and uses one deterministic grid traversal
for targeting, mining, and placement. `F1` retains the earlier isometric world
inspector as a debugging view.

## Native rendering notes

`RenderChunk.MakeFace` asks `BlockTerrain` for a 3×3 face-light table, averages
four samples into each corner, and packs separate sunlight and torch values in
every terrain vertex. It also packs an eight-neighbour occupancy mask used by
the fancy-lighting shader. The terrain effect receives the current vector to
the sun on every draw; lanterns and lava retain a full-bright material path.

The native mesh now carries equivalent per-corner skylight and occupancy data.
Covered faces darken into caves, cave mouths receive a short propagated-light
falloff, neighbouring blocks produce corner contact shadows, and lantern/lava
faces remain emissive. The moving sun vector shades voxel normals every frame.
Ambient Occlusion toggles the neighbour mask live, while Shadow Quality changes
the directional sunlight contrast instead of remaining a decorative setting.
The D3D11 material pass also samples the original normal/specular and
metal/light atlases. Authored surface normals perturb both diffuse and specular
response; authored gloss/metal pixels receive the recovered `SunSpecular`
color (white by day, warm at dawn/dusk, cool blue at night), and authored light
pixels supplement the full-bright lantern/lava path.

On Windows, the current renderer shares SDL's D3D11 device, draws all six
exposed voxel-face directions into a native-resolution color target with a real
24-bit depth buffer, then GPU-copies that target into an SDL texture for the
high-DPI HUD composition. The renderer preserves and restores SDL's complete
D3D11 pipeline state around this pass. The former painter-sorted projection is
retained only as a portability fallback. The streamed region includes a buffer
beyond the native fog cutoff, including at ultrawide horizontal fields of view.
All six imported faces of each Clear, Dawn, Sunset, and Night cube are
tessellated and perspective-projected around the camera with rotation only;
player translation cannot drag the sky. The original hourly sky transitions
crossfade between whole cubes without stretching side strips or leaving a
top/bottom seam.

The native terrain window now expands from 10×10 to 20×20 chunks with the
32–128 metre Terrain Visibility setting. Crossing a chunk boundary retains the
overlapping chunk objects (including player edits), deterministically builds
only entering strips on a worker, and replaces only the affected chunk-local
CPU/GPU meshes when the job completes. Block edits remesh a bounded 3×3 chunk
neighborhood. Saved block overrides are reapplied if a previously discarded
strip is revisited.

## Terrain fidelity status

Terrain generation is currently a deterministic behavioral reconstruction,
not yet a proven 1:1 reproduction. Every vanilla `CastleMinerZBuilder` pass and
biome class has now been audited against the installed assembly, including the
original block IDs, chunk dimensions, legacy random/noise primitives, radial
biome ordering, vertical bands, transition formulas, and terrain/cave/crash-
site/ore/hell/bedrock/origin/tree pass order. The crash-site slime field's
unusual fixed seed-1 initializer is preserved separately from world-seeded ore
noise. Exact chunk-for-chunk parity still requires a golden-output harness
against the running managed builder, so 1:1 is not claimed yet.

The recovered `WaterPlane` is a 384×384 surface at Y 1.5, drawn after opaque
terrain with depth reads and depth writes disabled. Its original effect and
normal-map source are not distributable standalone assets, so the Rebirth
profile implements the same render ordering with procedural ripples and
sky/fog tinting in the D3D11 pass. It is restricted to the Lagoon frontier;
Classic presentation and the Arctic/Descent bands retain their original dry or
frozen appearance.
