"""Draws the terrain textures of the Wilderness example (examples/mods/wilderness) from code.

WORLD part of the Wilderness pack: the blocks of its mountain range (water,
spruce log and needles, snowy needles, needle litter, gravel and slate), made
the way make_example_assets.py makes the other examples' art and saved with
its helper. Nothing comes from CastleMiner Z. The tiles are 128 texels square
(the retail terrain's own tile size) and wrap seamlessly; the colours sit in
the muted range of the retail rock, snow and grass they stand next to.
Deterministic: running it again rewrites the same bytes.

    py -3 tools/ModSdk/make_wilderness_terrain.py [--root examples/mods]

Needs Python 3.10+ and Pillow.
"""

from __future__ import annotations

import argparse
import math
import random
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_example_assets import clamp, save_png  # noqa: E402  (the shared helpers)

SIZE = 128

# ---------------------------------------------------------------------------
# tileable noise
# ---------------------------------------------------------------------------


class ValueNoise:
    """Smooth value noise that wraps every `period` lattice cells."""

    def __init__(self, seed: int, period_x: int, period_y: int | None = None) -> None:
        rng = random.Random(seed)
        self.px = period_x
        self.py = period_y if period_y is not None else period_x
        self.grid = [[rng.random() for _ in range(self.px)] for _ in range(self.py)]

    def at(self, u: float, v: float) -> float:
        """u, v in tile units (0..1 wraps); returns 0..1."""
        x = u * self.px
        y = v * self.py
        x0 = math.floor(x)
        y0 = math.floor(y)
        tx = x - x0
        ty = y - y0
        tx = tx * tx * (3 - 2 * tx)
        ty = ty * ty * (3 - 2 * ty)
        g = self.grid
        a = g[y0 % self.py][x0 % self.px]
        b = g[y0 % self.py][(x0 + 1) % self.px]
        c = g[(y0 + 1) % self.py][x0 % self.px]
        d = g[(y0 + 1) % self.py][(x0 + 1) % self.px]
        top = a + (b - a) * tx
        bottom = c + (d - c) * tx
        return top + (bottom - top) * ty


class Fbm:
    """Octaves of ValueNoise, each twice as fine; -1..1 roughly, wraps."""

    def __init__(self, seed: int, base: int, octaves: int, stretch: tuple[int, int] = (1, 1),
                 gain: float = 0.5) -> None:
        self.layers = []
        amplitude = 1.0
        for octave in range(octaves):
            period = base << octave
            self.layers.append((ValueNoise(seed * 31 + octave, period * stretch[0], period * stretch[1]),
                                amplitude))
            amplitude *= gain
        self.norm = sum(a for _, a in self.layers)

    def at(self, u: float, v: float) -> float:
        total = sum(layer.at(u, v) * a for layer, a in self.layers)
        return (total / self.norm) * 2.0 - 1.0


class Cells:
    """Worley noise on a wrapping grid of `count` x `count` jittered points."""

    def __init__(self, seed: int, count: int) -> None:
        rng = random.Random(seed)
        self.count = count
        self.points = [[(rng.random(), rng.random(), rng.random()) for _ in range(count)] for _ in range(count)]

    def at(self, u: float, v: float) -> tuple[float, float, float]:
        """(nearest distance, second distance, the nearest cell's own 0..1 value), distances in cells."""
        x = u * self.count
        y = v * self.count
        cx = math.floor(x)
        cy = math.floor(y)
        first = second = 9.0
        value = 0.0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                gx = cx + dx
                gy = cy + dy
                jx, jy, cell = self.points[gy % self.count][gx % self.count]
                distance = math.hypot(gx + jx - x, gy + jy - y)
                if distance < first:
                    second = first
                    first = distance
                    value = cell
                elif distance < second:
                    second = distance
        return first, second, value


def paint(function) -> Image.Image:
    """An RGBA tile from function(u, v) -> (r, g, b[, a]) in 0..255 floats."""
    image = Image.new("RGBA", (SIZE, SIZE))
    pixels = image.load()
    for y in range(SIZE):
        for x in range(SIZE):
            colour = function((x + 0.5) / SIZE, (y + 0.5) / SIZE)
            alpha = colour[3] if len(colour) > 3 else 255
            pixels[x, y] = (clamp(colour[0]), clamp(colour[1]), clamp(colour[2]), clamp(alpha))
    return image


def mix(a, b, t: float):
    t = max(0.0, min(1.0, t))
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def scale(colour, factor: float):
    return tuple(c * factor for c in colour)


# ---------------------------------------------------------------------------
# the tiles
# ---------------------------------------------------------------------------


def water() -> Image.Image:
    # Deep, slightly green-blue; soft swells and bright ripple crests.
    swell = Fbm(901, 2, 4)
    ripple = Fbm(902, 4, 3, stretch=(2, 1))
    deep = (30, 70, 112)
    light = (70, 128, 168)

    def colour(u, v):
        s = swell.at(u, v)
        r = ripple.at(u, v)
        crest = max(0.0, 1.0 - abs(r) * 6.0) ** 3
        base = mix(deep, light, 0.5 + 0.45 * s)
        base = mix(base, (176, 214, 232), crest * 0.55)
        return (*base, 196)

    return paint(colour)


def spruce_bark() -> Image.Image:
    # Grey-brown plates split by deep vertical furrows.
    furrows = Fbm(911, 10, 3, stretch=(1, 1))
    plates = Cells(912, 7)
    grain = Fbm(913, 8, 3)
    base = (74, 56, 44)

    def colour(u, v):
        f = furrows.at(u, v * 0.25)
        first, second, cell = plates.at(u * 1.6 % 1.0, v * 0.6 % 1.0)
        edge = max(0.0, 1.0 - (second - first) * 5.0)
        g = grain.at(u, v)
        shade = 1.0 + 0.18 * g + 0.1 * (cell - 0.5)
        tone = scale(base, shade)
        tone = mix(tone, (30, 21, 17), max(0.0, -f - 0.05) * 3.2)
        tone = mix(tone, (40, 29, 23), edge * 0.6)
        return mix(tone, (110, 98, 88), max(0.0, f - 0.2) * 1.4)

    return paint(colour)


def spruce_rings() -> Image.Image:
    grain = Fbm(921, 4, 3)
    cracks = Fbm(922, 3, 2)

    def colour(u, v):
        dx = u - 0.5
        dy = v - 0.5
        r = math.hypot(dx, dy) * 2.0 + grain.at(u, v) * 0.03
        if r > 0.86:
            # The bark around the cut.
            return scale((70, 52, 40), 0.85 + 0.25 * grain.at(u * 3 % 1.0, v * 3 % 1.0))
        ring = 0.5 + 0.5 * math.cos(r * 52.0)
        tone = mix((168, 128, 86), (124, 86, 54), ring ** 2)
        if r > 0.8:
            tone = mix(tone, (96, 66, 44), (r - 0.8) / 0.06)
        crack = abs(cracks.at(u, v))
        tone = mix(tone, (82, 58, 38), max(0.0, 0.05 - crack) * 12 * (1 if r < 0.75 else 0))
        return tone

    return paint(colour)


def needles(seed: int, snowy_top: bool = False, snow_side: bool = False) -> Image.Image:
    # Short dark needles in criss-crossing sprays with gaps (the leaves pass
    # cuts the gaps out).
    rng = random.Random(seed)
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    pixels = image.load()
    clumps = Fbm(seed + 1, 8, 2)
    shadow = Fbm(seed + 2, 6, 2)

    def put(x, y, colour):
        pixels[x % SIZE, y % SIZE] = colour

    # Under-layer: dark mass where clumps are dense.
    for y in range(SIZE):
        for x in range(SIZE):
            c = clumps.at(x / SIZE, y / SIZE)
            if c > -0.42:
                d = 0.75 + 0.35 * shadow.at(x / SIZE, y / SIZE)
                pixels[x, y] = (clamp(22 * d), clamp(40 * d), clamp(30 * d), 255)
    # Needles.
    for _ in range(2600):
        x = rng.randrange(SIZE)
        y = rng.randrange(SIZE)
        if clumps.at(x / SIZE, y / SIZE) < -0.46:
            continue
        angle = rng.choice((0.6, 2.5)) + rng.uniform(-0.3, 0.3)
        length = rng.randint(3, 6)
        tone = rng.uniform(0.75, 1.3)
        colour = (clamp(40 * tone), clamp(72 * tone), clamp(50 * tone), 255)
        for step in range(length):
            put(round(x + math.cos(angle) * step), round(y + math.sin(angle) * step), colour)
    if snowy_top or snow_side:
        snow = Fbm(seed + 3, 4, 3)
        for y in range(SIZE):
            for x in range(SIZE):
                s = snow.at(x / SIZE, y / SIZE)
                cover = s + (0.35 if snowy_top else 0.55 - 1.4 * y / SIZE)
                if cover > 0.1 and pixels[x, y][3] > 0 or (snowy_top and cover > 0.3):
                    bright = 0.9 + 0.1 * s
                    pixels[x, y] = (clamp(222 * bright), clamp(228 * bright), clamp(234 * bright), 255)
    return image


def needle_litter() -> Image.Image:
    # Rust-brown fallen needles over dark soil.
    rng = random.Random(941)
    soil = Fbm(942, 4, 4)
    image = paint(lambda u, v: scale((58, 42, 30), 0.8 + 0.3 * soil.at(u, v)))
    pixels = image.load()
    for _ in range(3200):
        x = rng.randrange(SIZE)
        y = rng.randrange(SIZE)
        angle = rng.uniform(0, math.pi)
        length = rng.randint(3, 7)
        tone = rng.uniform(0.7, 1.25)
        base = rng.choice(((128, 82, 44), (112, 74, 42), (96, 70, 46), (70, 84, 50)))
        colour = (clamp(base[0] * tone), clamp(base[1] * tone), clamp(base[2] * tone), 255)
        for step in range(length):
            pixels[round(x + math.cos(angle) * step) % SIZE, round(y + math.sin(angle) * step) % SIZE] = colour
    return image


def forest_dirt(seed: int = 951) -> Image.Image:
    cracks = Cells(seed, 9)
    grain = Fbm(seed + 1, 8, 3)

    def colour(u, v):
        first, second, cell = cracks.at(u, v)
        edge = max(0.0, 1.0 - (second - first) * 7.0)
        tone = scale((78, 58, 40), 0.85 + 0.2 * grain.at(u, v) + 0.12 * (cell - 0.5))
        return mix(tone, (40, 29, 21), edge * 0.7)

    return paint(colour)


def needle_litter_side() -> Image.Image:
    # Soil, with the litter hanging over the top edge.
    side = forest_dirt(961)
    top = needle_litter()
    fringe = Fbm(962, 8, 2)
    pixels = side.load()
    litter = top.load()
    for x in range(SIZE):
        depth = 14 + round(6 * fringe.at(x / SIZE, 0.5))
        for y in range(depth):
            pixels[x, y] = litter[x, y]
        pixels[x, depth] = (30, 22, 16, 255)
    return side


def gravel() -> Image.Image:
    # Rounded pebbles of a few greys and browns with dark gaps.
    pebbles = Cells(971, 11)
    small = Cells(972, 23)
    grain = Fbm(973, 8, 2)
    tones = ((112, 108, 102), (92, 88, 84), (132, 126, 118), (98, 86, 74), (78, 78, 80), (120, 112, 100))

    def colour(u, v):
        first, second, cell = pebbles.at(u, v)
        sf, ss, sc = small.at(u, v)
        if second - first < 0.12:
            return scale((52, 50, 48), 0.9 + 0.2 * grain.at(u, v))
        tone = tones[int(cell * len(tones)) % len(tones)]
        # Lit from the top-left, rounded.
        light = 1.06 - first * 0.45
        tone = scale(tone, light + 0.08 * grain.at(u, v))
        if ss - sf < 0.1:
            tone = scale(tone, 0.82)
        return tone

    return paint(colour)


def slate() -> Image.Image:
    # Dark blue-grey stone split along fine horizontal cleavage.
    layers = Fbm(981, 2, 4, stretch=(1, 6))
    grain = Fbm(982, 8, 3)
    joints = Cells(983, 5)

    def colour(u, v):
        l = layers.at(u, v)
        g = grain.at(u, v)
        first, second, _ = joints.at(u, v * 0.5 % 1.0)
        joint = max(0.0, 1.0 - (second - first) * 20.0) * (0.4 + 0.6 * max(0.0, g))
        tone = scale((60, 64, 72), 0.92 + 0.22 * l + 0.1 * g)
        band = 0.5 + 0.5 * math.sin((v * 9 + l * 0.6) * math.tau)
        tone = mix(tone, (82, 86, 94), max(0.0, band - 0.85) * 3)
        return mix(tone, (32, 34, 40), joint * 0.6)

    return paint(colour)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2] / "examples" / "mods"))
    root = Path(parser.parse_args().root)
    out = root / "wilderness" / "assets" / "textures" / "terrain"
    save_png(water(), out / "water.png")
    save_png(spruce_bark(), out / "spruce_bark.png")
    save_png(spruce_rings(), out / "spruce_rings.png")
    save_png(needles(931), out / "spruce_needles.png")
    save_png(needles(935, snowy_top=True), out / "snowy_needles_top.png")
    save_png(needles(935, snow_side=True), out / "snowy_needles_side.png")
    save_png(needle_litter(), out / "needle_litter.png")
    save_png(needle_litter_side(), out / "needle_litter_side.png")
    save_png(forest_dirt(), out / "forest_dirt.png")
    save_png(gravel(), out / "gravel.png")
    save_png(slate(), out / "slate.png")
    print(f"wilderness terrain textures written under {out}")


if __name__ == "__main__":
    main()
