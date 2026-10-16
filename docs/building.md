# Build and run

The complete game currently targets Windows x64. It uses C++20, SDL3, D3D11,
FreeType and Luau; the legacy Steam and XNA helpers are x86 processes.

## Prerequisites

Install Visual Studio 2022 with Desktop development with C++, a Windows SDK,
CMake 3.25+, Ninja, Git, and vcpkg. Open a **Developer PowerShell for VS 2022**.
Set `VCPKG_ROOT` to your vcpkg checkout. `vcpkg.json` pins the dependency baseline.

The helpers additionally need a .NET SDK that can build `net48`, the .NET
Framework 4.8 targeting pack, and XNA Framework 4.0. XNA is supplied by the owned
game's prerequisites; it is not bundled by OpenMinerZ. The content importer
uses a local graphics device, so perform imports on a Windows desktop with a
working graphics driver, not a headless build worker.

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
cmake --preset native-release
cmake --build --preset native-release --parallel 4
ctest --preset native-release
```

The executable is `out/build/native-release/Release/openminerz.exe`.
`play.bat` launches that build and forwards command-line arguments.

| Preset | Configuration | Purpose |
| --- | --- | --- |
| `native-dev` | RelWithDebInfo | Debuggable development |
| `native-release` | Release | Normal local development/play |
| `native-public` | Release | Separate asset-free public build |

All three disable automatic owned-content import. Tests that need imported
content or original clients are optional; passing the asset-free suite does not
prove live multiplayer or animation quality.

## Import and play

```powershell
.\import-assets.bat -ListInstallations
.\import-assets.bat -GameDirectory 'D:\SteamLibrary\steamapps\common\CastleMiner Z'
.\play.bat
```

See [importing-assets.md](importing-assets.md) for separate Classic/Modern packs,
failure recovery and private import paths.

For automatic local imports during a developer build, copy
`CMakeUserPresets.example.json` to ignored `CMakeUserPresets.json`, set the paths
for the game generations you own, and clear the path for any you do not own.
The `*-owned` presets write private packs below their build directory. At least
one generation must be configured. These are not public-release presets.

## Useful targets

```powershell
cmake --build --preset native-release --target openminerz_mod_sdk
& .\out\build\native-release\Release\openminerz_mod_sdk.exe --help
ctest --preset native-release -R openminerz_mod --output-on-failure
```

Python tools need Python 3.10+; Rig Lab and tactical authoring use NumPy and
Pillow. Other procedural example assets also use fontTools. Install these in a
virtual environment under ignored `out/` if you do not have them available.
Use `node tools/ModSdk/rig_lab/test-editor-state.mjs` for editor history tests.

Original-client probes are opt-in via `OPENMINERZ_BUILD_INTEROP_LAB=ON` and owned
game paths. Keep their generated facts and captures under `out/`. See
[development.md](development.md) before changing a compatibility contract.

## Build failures

- **Compiler/Ninja not found:** use the VS developer shell and check installed workloads.
- **vcpkg toolchain missing:** set `VCPKG_ROOT` before configuring.
- **XNA references missing:** install the owned game's XNA prerequisites; they
  are required for the importer/audio bridge, not copied into public packages.
- **Original assets missing at launch:** run the importer, then restart.
- **A stale local executable is locked:** close your game before replacing it.

Do not commit build caches, local presets, downloaded dependencies or imports.
