"""Unit tests for deterministic Classic HD atlas preparation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from PIL import Image

import classic_hd


class ClassicHdTests(unittest.TestCase):
    """Validate geometry and edge-preserving atlas operations."""

    def test_wrap_tile_repeats_opposite_edges(self) -> None:
        tile = Image.new("RGBA", (2, 2))
        tile.putdata(
            [
                (255, 0, 0, 255),
                (0, 255, 0, 255),
                (0, 0, 255, 255),
                (255, 255, 255, 255),
            ]
        )
        wrapped = classic_hd.wrap_tile(tile)
        self.assertEqual(wrapped.size, (6, 6))
        self.assertEqual(wrapped.getpixel((1, 2)), tile.getpixel((1, 0)))
        self.assertEqual(wrapped.getpixel((4, 5)), tile.getpixel((0, 1)))

    def test_data_atlas_resizes_tiles_without_cross_bleed(self) -> None:
        atlas = Image.new("RGBA", (16, 16), (0, 0, 0, 255))
        atlas.paste((255, 0, 0, 255), (0, 0, 2, 2))
        layout = classic_hd.validate_atlas(atlas, "fixture")
        resized = classic_hd.resize_data_atlas(atlas, layout)
        self.assertEqual(resized.size, (32, 32))
        self.assertEqual(resized.getpixel((1, 1))[0], 255)
        self.assertEqual(resized.getpixel((5, 1))[0], 0)

    def test_save_bmp_round_trips_sized_surface(self) -> None:
        image = Image.new("RGBA", (32, 32), (20, 40, 60, 128))
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "atlas.bmp"
            classic_hd.save_bmp(image, destination)
            with Image.open(destination) as loaded:
                self.assertEqual(loaded.size, image.size)
                self.assertEqual(loaded.convert("RGBA").getpixel((0, 0))[3], 128)

    def test_source_and_output_must_be_separate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ValueError):
                classic_hd.validate_pack_paths(root, root)


if __name__ == "__main__":
    unittest.main()
