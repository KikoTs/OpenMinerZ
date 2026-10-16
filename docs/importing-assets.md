# Import your game assets

OpenMinerZ imports the art and sound from your own installed CastleMiner Z copy.
The original installation is read only. Imported derivatives are private local
files, not redistributable content in this repository.

## Normal setup

From a source checkout:

```powershell
.\import-assets.bat -ListInstallations
.\import-assets.bat -GameDirectory 'D:\SteamLibrary\steamapps\common\CastleMiner Z'
```

An asset-free binary package has **Import Game Assets.cmd** with the same
arguments. With no path, a single detected Steam installation is selected.
If detection is ambiguous, pass the path explicitly. Detection follows Steam's
registered libraries rather than requiring a particular drive letter.

The importer identifies Classic or Modern from the content layout. Use a
separate run for each owned generation. `-Style Classic` or `-Style Modern`
checks your expectation and refuses a mismatched installation.
At startup, the game uses an available complete pack if the saved texture style
is missing. An explicit `--texture-style` argument still selects that exact style.

The default destinations are:

| Location | Contents |
| --- | --- |
| `%LOCALAPPDATA%\OpenMinerZ\ContentPacks\classic` | Imported Classic art/fonts/animation and four audio banks |
| `%LOCALAPPDATA%\OpenMinerZ\ContentPacks\modern` | Imported Modern content |
| `%LOCALAPPDATA%\OpenMinerZ\Runtime\steam_api.dll` | Your installation's Steam runtime, when present |

The game looks in the local runtime directory when starting its Steam helper.
No original executables, XNB tree or unrelated game files are copied into the
public package. Only the local import contains the required proprietary data.

## What happens on an update

The importer checks the installation and destination, builds or locates the
ContentBridge helper, and generates a fresh staging pack. It verifies required
outputs before replacing an existing pack. A failure rolls back the replacement.
The previous successful pack remains in a `.previous-*` sibling folder; keep it
until you have checked the new import. Existing world saves are not touched.

Each import contains a private marker and a SHA-256 manifest. Neither belongs in
Git or release archives. Import destinations inside a Git checkout must be below
`out/`; destinations inside the original game folder and paths through junctions
or symbolic links are refused.
Content-pack and runtime destinations must be separate, non-overlapping folders.

```powershell
# Validate paths and the required source files without writing anything.
powershell -NoProfile -File tools/Import-GameAssets.ps1 -ValidateOnly `
  -GameDirectory 'D:\Games\CastleMiner Z'

# An isolated local test pack; does not replace the default imported packs.
powershell -NoProfile -File tools/Import-GameAssets.ps1 `
  -GameDirectory 'D:\Games\CastleMiner Z' `
  -ContentPackRoot "$PWD\out\test-content" `
  -RuntimeDirectory "$PWD\out\test-runtime"
$env:OPENMINERZ_CONTENT_PACKS = "$PWD\out\test-content"
$env:OPENMINERZ_STEAM_API = "$PWD\out\test-runtime\steam_api.dll"
```

`-ValidateOnly` checks source-file presence and path safety; it does not decode
every asset or prove that the graphics/XNA prerequisites work.

## Other local assets

Xbox avatar exports and Rig Lab reference exports are private too. The repository
includes their decoders and tools, not avatar meshes or textures.
`tools/TextureUpscale/setup_classic_hd.ps1` can produce a local Classic HD pack;
it uses owned Classic textures and downloads its upscaler. Its generated textures
and weights remain private under the tool's ignored work folder/local content
directory. It is optional and not part of public packaging.

## Troubleshooting

Verify missing source files through Steam. For XNA or audio failures, run the
original game's prerequisite installers and launch the original once. Close
OpenMinerZ if a pack or runtime file is locked, rerun the importer, then restart.
Use `-GameDirectory` for a non-Steam or separately stored owned build.

Do not share a content-pack folder to solve another player's setup problem; have
them run the importer against their own installation.
