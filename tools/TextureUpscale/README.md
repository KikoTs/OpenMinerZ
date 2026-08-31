# OpenMinerZ Classic HD pipeline

This tool creates a local `classic-hd` content pack from an already imported,
owned CastleMiner Z Classic pack. It never modifies the source pack and never
writes proprietary or generated assets into the Git source tree.

## One-command setup

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools/TextureUpscale/setup_classic_hd.ps1
```

Defaults:

- source: `out/build/native-release/content-packs/classic`
- destination: `%LOCALAPPDATA%/OpenMinerZ/ContentPacks/classic-hd`
- model: official `realesrgan-x4plus`
- inference: 4x through NCNN/Vulkan on GPU 0, reduced to a 2x final atlas
- final terrain atlases: 2048x2048, with 256x256 independent tile slices

The setup downloads the official portable Real-ESRGAN v0.2.5.0 Windows build
and verifies its SHA-256 before extracting it under the ignored `out/tools`
directory. No CUDA or global Python packages are required.

## Fidelity rules

Each 128x128 diffuse tile is repeated into a 3x3 wrap grid before inference.
Only the centre result is retained, which gives the network the real opposite
edge as context and preserves seamless repetition. The model produces 4x
detail in one unsplit 384x384 inference tile, then Lanczos downsamples it to the
shipping 2x resolution.

`terrain_normal_spec.bmp` and `terrain_metal_light.bmp` are encoded material
data, not photographs. They are resized per tile with deterministic Lanczos
filtering and are never passed through AI. Runtime mipmaps remain isolated per
tile in the D3D11 texture array.

The generated pack contains a manifest with source, model, and result hashes,
plus `classic_hd_comparison.png` for quick visual review.

## Overrides

```powershell
powershell -ExecutionPolicy Bypass -File tools/TextureUpscale/setup_classic_hd.ps1 `
  -SourcePack 'D:\packs\classic' `
  -OutputPack 'D:\packs\classic-hd' `
  -Gpu 0 `
  -KeepWork
```

Rerunning the command refreshes the destination from the source pack and
rebuilds all three atlases. Select `CLASSIC HD` in Graphics > Texture style and
restart OpenMinerZ.
