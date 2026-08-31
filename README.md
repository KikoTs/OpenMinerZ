# OpenMinerZ

OpenMinerZ is a native C++20 compatibility-focused reimplementation of the
CastleMiner Z runtime. It combines a deterministic voxel simulation, a D3D11
renderer, SDL3 input/windowing, original-style and modern presentation modes,
local world persistence, and Steam-backed multiplayer interoperability.

> **Repository status:** private, pre-release, and under active development.
> Expect protocol, save, renderer, and tooling changes until the compatibility
> matrix is complete.

## What is here

- A playable native client with local worlds, inventory, combat, enemies,
  environmental simulation, and Classic/Rebirth presentation profiles.
- Classic-v3 and Modern-v4 session discovery, admission, replication, and
  server-authoritative gameplay paths.
- A separately built 32-bit Steam bridge for the legacy Steam ABI.
- Deterministic content import tools for a developer's legally owned Classic
  and Modern installations.
- Headless/unit coverage for gameplay, saves, menus, networking, codecs,
  admission, replication, and protocol-trace validation.

The repository intentionally contains **source code only**. It does not contain
CastleMiner Z binaries, Steam runtime DLLs, XNB content, audio banks, textures,
models, avatars, saves, screenshots, packet captures, or imported derivatives.

## Requirements

The actively supported development environment is Windows x64:

- Visual Studio 2022 or newer with **Desktop development with C++**
- CMake 3.25+
- Ninja
- Git
- A vcpkg checkout with `VCPKG_ROOT` set
- .NET SDK 8+ for the audio/content helper projects
- A legally owned CastleMiner Z installation for a runnable content build

The engine code is structured for other platforms, but the D3D11 renderer,
legacy Steam bridge, and original XACT audio path currently make Windows the
complete target.

## Quick start: source and tests

From a Visual Studio developer PowerShell:

```powershell
git clone https://github.com/KikoTs/OpenMinerZ.git
Set-Location OpenMinerZ
$env:VCPKG_ROOT = 'C:\src\vcpkg'

cmake --preset native-dev
cmake --build --preset native-dev --target openminerz_tests
ctest --preset native-dev
```

The checked-in presets do not import any proprietary content. This allows the
core and test targets to be configured from a clean clone without embedding a
developer's local game paths in Git.

## Configure a runnable local build

1. Copy the example user preset. The destination is ignored by Git.

   ```powershell
   Copy-Item CMakeUserPresets.example.json CMakeUserPresets.json
   ```

2. Edit `CMakeUserPresets.json` and set:

   - `OPENMINERZ_CLASSIC_GAME_DIR` to the root of the owned Classic build.
   - `OPENMINERZ_MODERN_GAME_DIR` to the root of the owned Modern/Steam build.
   - `OPENMINERZ_STEAM_API_DLL` to that installation's 32-bit
     `steam_api.dll`.

3. Configure, build, and test with the local preset:

   ```powershell
   cmake --preset native-release-owned
   cmake --build --preset native-release-owned
   ctest --preset native-release-owned
   ```

Imported content is generated under
`out/build/native-release-owned/content-packs/` and remains ignored. The build
stages `steam_api.dll` beside the executable; never copy that DLL into the
source tree or commit it.

### Classic HD texture pack

After importing the owned Classic content, generate the local AI-restored pack
with:

```powershell
powershell -ExecutionPolicy Bypass -File tools/TextureUpscale/setup_classic_hd.ps1
```

The script downloads the pinned official Real-ESRGAN Vulkan release, prepares a
private Python environment, upscales every repeating diffuse tile independently,
and installs `classic-hd` under `%LOCALAPPDATA%/OpenMinerZ/ContentPacks`. Generated
textures, model weights, and owned source content remain outside Git. Select
`CLASSIC HD` under Graphics > Texture style and restart the game. See
`tools/TextureUpscale/README.md` for the reproducible pipeline and overrides.

The generated game executable is:

```text
out/build/native-release-owned/Release/openminerz.exe
```

Steam initialization happens at process launch. For a local diagnostic only,
`OPENMINERZ_STEAM_API` may point to an owned `steam_api.dll` when it is not
already staged beside the executable.

## Common commands

```powershell
# Fast development build
cmake --build --preset native-dev --parallel

# Release build with locally owned content
cmake --build --preset native-release-owned --parallel

# All registered tests, with failures printed
ctest --preset native-release-owned

# Run just the C++ suite directly
& .\out\build\native-release-owned\Release\openminerz_tests.exe

# Apply the repository's C++ style
clang-format -i (rg --files include src tests -g '*.cpp' -g '*.hpp')
```

The legacy `play.bat` helper launches the canonical
`out/build/native-release/Release` build used by the primary workstation. Other
developers should launch the executable from their selected preset directory.

## Repository layout

| Path | Purpose |
| --- | --- |
| `include/openminerz/` | Public engine, gameplay, save, and network contracts |
| `src/` | Runtime, renderer, simulation, UI, and protocol implementations |
| `tests/` | Deterministic C++ test suite |
| `tools/SteamBridge32/` | Legacy x86 Steam transport helper |
| `tools/AudioBridge/` | Local original-bank audio helper |
| `tools/ContentBridge/` | Owned-content importer and derived-data generator |
| `tools/OriginalClientInterop/` | Isolated compatibility and trace-audit lab |
| `docs/` | Protocol, UI, world-generation, and parity findings |
| `out/` | Local builds, dependencies, imports, captures, and test output; ignored |

## Runtime boundaries

```text
platform input -----> fixed-step simulation -----> render snapshot
network packets ----> validation/authority ------> save + replication
                                                   |
                                                   +--> D3D11 renderer
                                                   +--> audio bridge
```

- The simulation owns gameplay truth; render state is derived from it.
- Network payloads are bounded and validated before application.
- Host-authoritative state wins for world mutation, combat, inventory, death,
  respawn, pickups, and late-join bootstrap.
- Classic and Rebirth presentation must not change gameplay outcomes.
- Imported content generations are isolated and selected at process start.

## Multiplayer development

The client discovers both Classic-v3 and Modern-v4 Steam lobbies. Admission,
player presence, terrain authority, bootstrap ordering, and ownership checks
are deliberately fail-closed. When changing protocol code:

1. Add or update a codec/validation test.
2. Verify the loopback session.
3. Run the trace audit.
4. Test private original/OpenMinerZ role reversal when the change touches live
   interoperability.
5. Remove or anonymize Steam IDs, lobby IDs, usernames, IP addresses, and save
   metadata before attaching evidence.

The isolated interoperability tooling is opt-in:

```powershell
cmake -S . -B out/build/interop-lab `
  -DOPENMINERZ_BUILD_INTEROP_LAB=ON `
  -DOPENMINERZ_BUILD_TESTS=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

Never point automation at public players or servers. Use owned clients,
consenting testers, and private sessions.

## Controls

- `WASD` / mouse: move and look
- `Space`: jump or swim upward
- `Left Shift` / `Left Ctrl`: swim downward
- `E`: inventory
- `Q`: drop the selected item
- Mouse wheel or `1`-`8`: hotbar selection
- Left/right mouse: use/place and mine/aim according to the held item
- `R`: reload
- `Tab`: player roster in multiplayer
- `Esc`: pause/back

Keyboard and mouse own input until a controller's Start/Menu button claims
gamepad control. Any keyboard or mouse activity hands control back.

## Documentation

- [`docs/multiplayer-foundation.md`](docs/multiplayer-foundation.md) describes
  the multiplayer architecture and trust boundaries.
- [`docs/original-client-network-compatibility-plan.md`](docs/original-client-network-compatibility-plan.md)
  tracks the isolated interoperability strategy.
- [`docs/original-ui-findings.md`](docs/original-ui-findings.md) records the
  original menu/UI behavior used for parity work.
- [`docs/original-world-findings.md`](docs/original-world-findings.md) records
  world-generation and rendering evidence.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) contains the developer workflow and pull
  request checklist.
- [`SECURITY.md`](SECURITY.md) covers private vulnerability reporting.

## Legal and redistribution boundary

OpenMinerZ is an independent compatibility project and is not affiliated with
or endorsed by the CastleMiner Z rights holders, Microsoft, Xbox, Valve, or
Steam.

Do not commit or redistribute proprietary game binaries, Microsoft/Xbox avatar
assets, Steam DLLs, original or derived content, audio banks, private protocol
captures, or another player's data. Every developer is responsible for using
their own lawfully obtained files and complying with applicable licenses and
local law.

No open-source license is currently granted for this private repository. All
rights in the original OpenMinerZ source remain with their respective authors;
third-party dependencies retain their own licenses.
