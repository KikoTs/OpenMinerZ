# OpenMinerZ

[![Source and asset audit](https://github.com/KikoTs/OpenMinerZ/actions/workflows/source-audit.yml/badge.svg?branch=main)](https://github.com/KikoTs/OpenMinerZ/actions/workflows/source-audit.yml)
[![License: AGPL v3+](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D4.svg)](docs/building.md)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?logo=cplusplus&logoColor=white)](docs/building.md)
[![Add-ons: Luau](https://img.shields.io/badge/add--ons-Luau-00A2FF.svg?logo=luau&logoColor=white)](docs/modding/README.md)
[![Status: development preview](https://img.shields.io/badge/status-development%20preview-orange.svg)](docs/releasing.md)

A native C++20 reimplementation of CastleMiner Z for Windows, with a D3D11
renderer, local worlds, Classic and Modern gameplay profiles, Steam multiplayer,
and a sandboxed Luau add-on system.

**Development preview:** expect bugs, especially in visual animation and live
multiplayer interoperability. Back up important worlds before testing updates.
This is an independent project, not an official CastleMiner Z release.

## Play

You need Windows x64, a DirectX 11-capable GPU, and your own CastleMiner Z
installation. OpenMinerZ does not distribute the original game's models,
textures, fonts, audio banks, reference animations, avatars or Steam DLL.

For an asset-free binary package produced from this repository:

1. Extract the entire package to a writable folder.
2. Install and run your own CastleMiner Z copy once so its prerequisites are set up.
3. Run **Import Game Assets.cmd**. It detects a Steam installation, or accepts
   `-GameDirectory "D:\SteamLibrary\steamapps\common\CastleMiner Z"`.
4. Run **Play OpenMinerZ.cmd**. Keep Steam signed in for online play.

Imported files go to `%LOCALAPPDATA%\OpenMinerZ\ContentPacks\` and
`%LOCALAPPDATA%\OpenMinerZ\Runtime\`. They stay separate from public packages.
Import Classic and Modern installations separately if you use both profiles.
See [asset setup and troubleshooting](docs/importing-assets.md).

This README describes packages you can build; it does not imply that a published
binary release is already available.

## Build from source

Use a Visual Studio 2022 developer PowerShell with the C++ desktop workload,
CMake 3.25+, Ninja, Git and a vcpkg checkout. Install the .NET SDK, .NET Framework
4.8 targeting pack and XNA 4.0 for the Windows audio/import helpers.

```powershell
git clone https://github.com/KikoTs/OpenMinerZ.git
Set-Location OpenMinerZ
$env:VCPKG_ROOT = 'C:\src\vcpkg'
cmake --preset native-release
cmake --build --preset native-release --parallel 4
ctest --preset native-release
.\import-assets.bat
.\play.bat
```

The checked-in presets do not import owned content. The importer is a separate
local step. The [build guide](docs/building.md) covers dependencies, other presets,
optional compatibility probes and working without game assets.

## Add-ons

Open **Add-Ons** in the main menu. Bundled examples start off; enable one before
loading a world. **Open Mods Folder** opens
`%LOCALAPPDATA%\OpenMinerZ\Mods\`; install an add-on as a folder containing
`openminerz.mod`. A user-installed copy takes precedence over a bundled example
with the same ID.

- **Client add-ons** change presentation: UI, graphics, sounds and animations.
- **Server add-ons** change gameplay. A host using them requires OpenMinerZ
  clients with matching server content. Downloads require the joining player's
  approval. Client presentation packs do not grant gameplay authority.
- World-generator add-ons remain dependencies of worlds they create.

Start with the [add-on guide](docs/modding/README.md), then use the
[SDK workflow](docs/modding/authoring.md), [tested recipes](docs/modding/recipes.md)
and [generated API reference](docs/modding/api-reference.md).

| Example | What it demonstrates |
| --- | --- |
| `survival` | Hunger, thirst, food and HUD stats |
| `wildlife` | Passive animals and drops |
| `story_dungeon` | A structure, NPC dialogue and a quest |
| `frontier` | Custom biomes and world generation |
| `bestiary` | Creature brains, attacks and a boss |
| `wilderness` | Terrain, liquids, survival rules, animals and custom items |
| `ui_theme` | Menus, HUD widgets and theme assets |
| `lighting_pack` | Lights, grading and texture overrides |
| `animation_pack` | Authored player animation replacements |
| `tactical_animations` | Weapon motion, moving parts, hand contacts and recorded foley |

For hand placement and weapon animation editing, run `rig-lab.bat` after building
the SDK and importing your assets. [The animation guide](docs/modding/animation.md)
explains live editing, linked grips, undo, resets and installation.

## Controls and local files

| Input | Action |
| --- | --- |
| WASD / mouse | Move / look |
| Space | Jump / swim up |
| Shift / Ctrl | Swim down |
| E | Inventory |
| Q | Drop selected item |
| Mouse wheel / 1–8 | Hotbar |
| Left / right mouse | Use, mine, place or aim, depending on the held item |
| R | Reload |
| Tab / hold V | Multiplayer roster / voice chat |
| Esc | Pause / back |
| F2 / F3 | Toggle HUD / diagnostics |

Worlds, settings and add-on choices live under `%APPDATA%\OpenMinerZ\`.
Imported content and installed add-ons live under `%LOCALAPPDATA%\OpenMinerZ\`.
Never include either user-data directory in a release or bug-report attachment.

## Contribute and release

- [Development map and tests](docs/development.md)
- [Instructions for coding agents](AGENTS.md)
- [Public-release workflow](docs/releasing.md)
- [Report a bug](https://github.com/KikoTs/OpenMinerZ/issues)
- [Report a security issue privately](https://github.com/KikoTs/OpenMinerZ/security/advisories/new)

The release audit checks both tracked and untracked publishable files. It rejects
private imports, unapproved media, credential patterns and broken documentation
links. Public packaging uses an explicit runtime file list, never a whole build
directory. Keep tests and source tools; put captures, experiments and local build
output under ignored `out/`.

## License

Copyright (c) 2026 Kiril Tsanov and contributors.

OpenMinerZ is free software under the **GNU Affero General Public License v3.0
or later (AGPL-3.0-or-later)**. See [LICENSE](LICENSE) and [NOTICE.txt](NOTICE.txt).
Third-party code and recordings retain their own licenses; see
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). This license does not cover
the original game or assets imported from it.
