# Original frontend findings

This note records behavior observed from the legally installed CastleMiner Z
copy. It is implementation evidence, not copied game source.

## Baseline build

- Executable: `CastleMinerZ.exe` 1.1.3.0 installed refresh build
- SHA-256: `057C2FC9756ADA75506D52733AAB23881B6E154A93BA8111624CB8B9D36F9BF6`
- DNA.Common SHA-256:
  `7FA8F56DDF185E30CA60583D5D3F3715899EE0D4EAF94EFAE4DD7744E1A70FE0`
- Managed source evidence was inspected in the existing ModWorkbench vanilla
  and refresh decompilations. No source or proprietary asset was copied into
  OpenMinerZ.

## Recovered menu contract

The original main menu exposes Host Online, Join Online, Play Offline,
Purchase (trial only), Awards, Options, and Exit. Its layout is tied to fixed
pixel offsets: a 512-pixel logo at y=25, the menu begins at y=175, and its
draw area consumes half the title-safe width minus 125 pixels. The player name
is drawn at the absolute origin. This explains the weak high-resolution and
non-16:9 behavior visible in the comparison capture.

Options is a second fixed two-column screen. It routes to Controls, destructive
storage deletion with confirmation, Settings, Release Notes, and Back.

The original Settings screen exposes nine controls: brightness, controller
sensitivity, music volume, gameplay FOV, a five-step aggregate graphics
setting, invert Y, auto-climb, fullscreen, and hide UI. Values are applied on
every update and saved when the screen pops. Gameplay and first-person
viewmodel FOV are derived separately, which is retained and made explicit in
OpenMinerZ.

## Rebirth decisions

- Preserve recognizable top-level routes while giving each a dedicated,
  animated, descriptive control surface.
- Replace absolute XNA display-mode coordinates with a 1920×1080 design canvas,
  safe area, UI scale, and real drawable-density font rasterization.
- Keep gameplay vertical FOV and viewmodel FOV independent.
- Expand settings into Display, Graphics, Gameplay, Audio, Controls, and
  Accessibility tabs with immediate preview and persistence.
- Route unavailable gameplay/network systems through typed frontend commands
  and visible status messages so controls never fail silently.
- Use original assets only through a future runtime importer pointed at a
  legally installed copy.
