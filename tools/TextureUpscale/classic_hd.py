"""Build a faithful, locally generated Classic HD OpenMinerZ content pack."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from PIL import Image, ImageDraw


ATLAS_COLUMNS = 8
ATLAS_ROWS = 8
AI_SCALE = 4
FINAL_SCALE = 2
DIFFUSE_NAME = "terrain_diffuse.bmp"
DATA_ATLAS_NAMES = ("terrain_normal_spec.bmp", "terrain_metal_light.bmp")


@dataclass(frozen=True)
class AtlasLayout:
    """Validated dimensions for one square 8x8 terrain atlas."""

    atlas_size: int
    tile_size: int
    final_atlas_size: int
    final_tile_size: int


def parse_args(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments for local pack generation."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pack", type=Path, required=True)
    parser.add_argument("--output-pack", type=Path, required=True)
    parser.add_argument("--upscaler", type=Path, required=True)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--keep-work", action="store_true")
    return parser.parse_args(arguments)


def sha256(path: Path) -> str:
    """Return the lowercase SHA-256 digest of a file."""

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_atlas(image: Image.Image, name: str) -> AtlasLayout:
    """Validate the source atlas geometry and return its tile layout."""

    width, height = image.size
    if width != height or width == 0 or width % ATLAS_COLUMNS != 0:
        raise ValueError(f"{name} must be a non-empty square 8x8 atlas; got {width}x{height}")
    tile_size = width // ATLAS_COLUMNS
    return AtlasLayout(
        atlas_size=width,
        tile_size=tile_size,
        final_atlas_size=width * FINAL_SCALE,
        final_tile_size=tile_size * FINAL_SCALE,
    )


def tile_box(index: int, tile_size: int) -> tuple[int, int, int, int]:
    """Return the pixel bounds for one row-major atlas tile."""

    if not 0 <= index < ATLAS_COLUMNS * ATLAS_ROWS:
        raise ValueError(f"tile index must be in [0, 63], got {index}")
    x = (index % ATLAS_COLUMNS) * tile_size
    y = (index // ATLAS_COLUMNS) * tile_size
    return x, y, x + tile_size, y + tile_size


def wrap_tile(tile: Image.Image) -> Image.Image:
    """Repeat a tile into a 3x3 canvas so opposite edges become real context."""

    width, height = tile.size
    wrapped = Image.new(tile.mode, (width * 3, height * 3))
    for row in range(3):
        for column in range(3):
            wrapped.paste(tile, (column * width, row * height))
    return wrapped


def is_empty_tile(tile: Image.Image) -> bool:
    """Return true when an atlas cell has no RGB information to restore."""

    return tile.convert("RGB").getbbox() is None


def prepare_diffuse_inputs(
    source: Image.Image,
    layout: AtlasLayout,
    input_directory: Path,
) -> tuple[list[int], dict[int, Image.Image]]:
    """Write seamless AI inputs and retain untouched empty tiles."""

    input_directory.mkdir(parents=True, exist_ok=True)
    active_indices: list[int] = []
    empty_tiles: dict[int, Image.Image] = {}
    for index in range(ATLAS_COLUMNS * ATLAS_ROWS):
        tile = source.crop(tile_box(index, layout.tile_size)).convert("RGBA")
        if is_empty_tile(tile):
            empty_tiles[index] = tile
            continue
        active_indices.append(index)
        wrap_tile(tile).save(input_directory / f"tile_{index:02d}.png")
    if not active_indices:
        raise ValueError("the diffuse atlas contains no populated tiles")
    return active_indices, empty_tiles


def run_upscaler(
    executable: Path,
    input_directory: Path,
    output_directory: Path,
    gpu: int,
    inference_tile_size: int,
) -> None:
    """Run the pinned portable Real-ESRGAN Vulkan executable."""

    if not executable.is_file():
        raise FileNotFoundError(f"Real-ESRGAN executable is missing: {executable}")
    output_directory.mkdir(parents=True, exist_ok=True)
    command = [
        str(executable),
        "-i",
        str(input_directory),
        "-o",
        str(output_directory),
        "-n",
        "realesrgan-x4plus",
        "-s",
        str(AI_SCALE),
        "-t",
        str(inference_tile_size),
        "-g",
        str(gpu),
        "-j",
        "2:4:2",
        "-f",
        "png",
        "-v",
    ]
    completed = subprocess.run(
        command,
        cwd=executable.parent,
        check=False,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"Real-ESRGAN failed with exit code {completed.returncode}")


def central_ai_tile(image: Image.Image, source_tile_size: int) -> Image.Image:
    """Crop the central wrapped tile and downsample it to the shipping 2x size."""

    ai_tile_size = source_tile_size * AI_SCALE
    expected_size = ai_tile_size * 3
    if image.size != (expected_size, expected_size):
        raise ValueError(
            f"unexpected Real-ESRGAN output {image.size}; expected "
            f"{expected_size}x{expected_size}"
        )
    central = image.crop(
        (ai_tile_size, ai_tile_size, ai_tile_size * 2, ai_tile_size * 2)
    )
    final_size = source_tile_size * FINAL_SCALE
    return central.resize((final_size, final_size), Image.Resampling.LANCZOS)


def rebuild_diffuse_atlas(
    source: Image.Image,
    layout: AtlasLayout,
    active_indices: Iterable[int],
    empty_tiles: dict[int, Image.Image],
    output_directory: Path,
) -> Image.Image:
    """Assemble the AI-restored and deterministic empty cells into one atlas."""

    atlas = Image.new("RGBA", (layout.final_atlas_size, layout.final_atlas_size))
    for index in active_indices:
        result_path = output_directory / f"tile_{index:02d}.png"
        if not result_path.is_file():
            raise FileNotFoundError(f"Real-ESRGAN did not create {result_path.name}")
        with Image.open(result_path) as result:
            tile = central_ai_tile(result.convert("RGBA"), layout.tile_size)
        x = (index % ATLAS_COLUMNS) * layout.final_tile_size
        y = (index // ATLAS_COLUMNS) * layout.final_tile_size
        atlas.paste(tile, (x, y))
    for index, source_tile in empty_tiles.items():
        tile = source_tile.resize(
            (layout.final_tile_size, layout.final_tile_size), Image.Resampling.NEAREST
        )
        x = (index % ATLAS_COLUMNS) * layout.final_tile_size
        y = (index // ATLAS_COLUMNS) * layout.final_tile_size
        atlas.paste(tile, (x, y))
    return atlas


def resize_data_atlas(source: Image.Image, layout: AtlasLayout) -> Image.Image:
    """Resize encoded material data independently per tile without AI or bleed."""

    source_rgba = source.convert("RGBA")
    atlas = Image.new("RGBA", (layout.final_atlas_size, layout.final_atlas_size))
    for index in range(ATLAS_COLUMNS * ATLAS_ROWS):
        tile = source_rgba.crop(tile_box(index, layout.tile_size)).resize(
            (layout.final_tile_size, layout.final_tile_size), Image.Resampling.LANCZOS
        )
        x = (index % ATLAS_COLUMNS) * layout.final_tile_size
        y = (index // ATLAS_COLUMNS) * layout.final_tile_size
        atlas.paste(tile, (x, y))
    return atlas


def comparison_sheet(
    source: Image.Image, restored: Image.Image, layout: AtlasLayout
) -> Image.Image:
    """Create a labelled nearest-original versus Classic HD review sheet."""

    populated_indices = [
        index
        for index in range(ATLAS_COLUMNS * ATLAS_ROWS)
        if not is_empty_tile(source.crop(tile_box(index, layout.tile_size)))
    ]
    cell = layout.final_tile_size
    label_height = 28
    sheet = Image.new(
        "RGB", (cell * 2, (cell + label_height) * len(populated_indices)), "#101416"
    )
    draw = ImageDraw.Draw(sheet)
    source_rgba = source.convert("RGBA")
    for row, index in enumerate(populated_indices):
        source_tile = source_rgba.crop(tile_box(index, layout.tile_size)).resize(
            (cell, cell), Image.Resampling.NEAREST
        )
        restored_tile = restored.crop(tile_box(index, cell))
        y = row * (cell + label_height)
        sheet.paste(source_tile.convert("RGB"), (0, y))
        sheet.paste(restored_tile.convert("RGB"), (cell, y))
        draw.text((6, y + cell + 6), f"tile {index:02d} ORIGINAL", fill="#d5dadd")
        draw.text((cell + 6, y + cell + 6), f"tile {index:02d} CLASSIC HD", fill="#ff4057")
    return sheet


def save_bmp(image: Image.Image, destination: Path) -> None:
    """Save and verify an alpha-preserving BITMAPV4 BGRA surface for SDL."""

    destination.parent.mkdir(parents=True, exist_ok=True)
    rgba = image.convert("RGBA")
    width, height = rgba.size
    pixel_bytes = width * height * 4
    dib_size = 108
    pixel_offset = 14 + dib_size
    file_size = pixel_offset + pixel_bytes
    file_header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset)
    info_header = struct.pack(
        "<IiiHHIIiiII",
        dib_size,
        width,
        height,
        1,
        32,
        3,
        pixel_bytes,
        2835,
        2835,
        0,
        0,
    )
    masks_and_colour_space = struct.pack(
        "<IIIII36sIII",
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        0xFF000000,
        0x73524742,
        bytes(36),
        0,
        0,
        0,
    )
    raw_bgra = rgba.tobytes("raw", "BGRA")
    row_bytes = width * 4
    with destination.open("wb") as stream:
        stream.write(file_header)
        stream.write(info_header)
        stream.write(masks_and_colour_space)
        for row in range(height - 1, -1, -1):
            start = row * row_bytes
            stream.write(raw_bgra[start : start + row_bytes])
    with Image.open(destination) as check:
        if check.size != image.size or check.convert("RGBA").getchannel("A").getextrema() != (
            rgba.getchannel("A").getextrema()
        ):
            raise RuntimeError(f"saved atlas has the wrong dimensions: {destination}")


def validate_pack_paths(source_pack: Path, output_pack: Path) -> None:
    """Reject dangerous source/output relationships before copying content."""

    source = source_pack.resolve()
    output = output_pack.resolve()
    if source == output:
        raise ValueError("the Classic HD output pack must not overwrite the source pack")
    if source in output.parents or output in source.parents:
        raise ValueError("source and output packs must not contain one another")
    required = [DIFFUSE_NAME, *DATA_ATLAS_NAMES]
    for name in required:
        path = source / "DerivedData" / name
        if not path.is_file():
            raise FileNotFoundError(f"required Classic atlas is missing: {path}")
    if not (source / "Content" / "Sounds.xgs").is_file():
        raise FileNotFoundError(f"Classic content pack is incomplete: {source}")


def build_pack(args: argparse.Namespace) -> Path:
    """Generate and install one complete Classic HD content pack."""

    source_pack = args.source_pack.resolve()
    output_pack = args.output_pack.resolve()
    executable = args.upscaler.resolve()
    validate_pack_paths(source_pack, output_pack)

    work_root = output_pack.parent / ".classic-hd-work"
    input_directory = work_root / "inputs"
    ai_output_directory = work_root / "ai-output"
    generated_directory = work_root / "generated"
    if work_root.exists():
        shutil.rmtree(work_root)
    input_directory.mkdir(parents=True)
    ai_output_directory.mkdir(parents=True)
    generated_directory.mkdir(parents=True)

    source_paths = {
        name: source_pack / "DerivedData" / name
        for name in (DIFFUSE_NAME, *DATA_ATLAS_NAMES)
    }
    try:
        with Image.open(source_paths[DIFFUSE_NAME]) as diffuse_source_file:
            diffuse_source = diffuse_source_file.convert("RGBA")
        layout = validate_atlas(diffuse_source, DIFFUSE_NAME)
        active_indices, empty_tiles = prepare_diffuse_inputs(
            diffuse_source, layout, input_directory
        )
        print(f"Restoring {len(active_indices)} populated diffuse tiles on GPU {args.gpu}...")
        run_upscaler(
            executable,
            input_directory,
            ai_output_directory,
            args.gpu,
            layout.tile_size * 3,
        )
        restored_diffuse = rebuild_diffuse_atlas(
            diffuse_source,
            layout,
            active_indices,
            empty_tiles,
            ai_output_directory,
        )
        save_bmp(restored_diffuse, generated_directory / DIFFUSE_NAME)

        for name in DATA_ATLAS_NAMES:
            with Image.open(source_paths[name]) as data_source_file:
                data_source = data_source_file.convert("RGBA")
            data_layout = validate_atlas(data_source, name)
            if data_layout != layout:
                raise ValueError(f"{name} dimensions do not match {DIFFUSE_NAME}")
            save_bmp(resize_data_atlas(data_source, layout), generated_directory / name)

        comparison = comparison_sheet(diffuse_source, restored_diffuse, layout)
        comparison.save(generated_directory / "classic_hd_comparison.png")

        print(f"Refreshing the complete local pack at {output_pack}...")
        shutil.copytree(source_pack, output_pack, dirs_exist_ok=True)
        output_derived = output_pack / "DerivedData"
        for name in (DIFFUSE_NAME, *DATA_ATLAS_NAMES, "classic_hd_comparison.png"):
            shutil.copy2(generated_directory / name, output_derived / name)

        model_root = executable.parent / "models"
        manifest = {
            "format": 1,
            "generator": "OpenMinerZ Classic HD",
            "model": "realesrgan-x4plus",
            "ai_scale": AI_SCALE,
            "final_scale": FINAL_SCALE,
            "source_atlas_size": layout.atlas_size,
            "output_atlas_size": layout.final_atlas_size,
            "source_hashes": {name: sha256(path) for name, path in source_paths.items()},
            "model_hashes": {
                "realesrgan-x4plus.bin": sha256(model_root / "realesrgan-x4plus.bin"),
                "realesrgan-x4plus.param": sha256(model_root / "realesrgan-x4plus.param"),
            },
            "output_hashes": {
                name: sha256(output_derived / name)
                for name in (DIFFUSE_NAME, *DATA_ATLAS_NAMES)
            },
            "populated_tiles": active_indices,
        }
        manifest_path = output_derived / "classic_hd_manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return manifest_path
    finally:
        if not args.keep_work and work_root.exists():
            shutil.rmtree(work_root)


def main(arguments: Sequence[str] | None = None) -> int:
    """Generate the pack and print its manifest location."""

    try:
        args = parse_args(arguments)
        manifest = build_pack(args)
    except (FileNotFoundError, OSError, RuntimeError, ValueError) as error:
        print(f"classic-hd error: {error}", file=sys.stderr)
        return 1
    print(f"Classic HD manifest: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
