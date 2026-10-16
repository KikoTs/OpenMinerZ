"""Draws the art of the example mods in examples/mods/ from code.

Every texture, icon, model, sound and font the examples ship is made here, so
nothing in them comes from CastleMiner Z or anyone else, and anyone can remake
or change them. The output is deterministic: running it again rewrites the
same bytes.

    py -3 tools/ModSdk/make_example_assets.py [--root examples/mods]

Needs Python 3.10+, Pillow and fontTools (pip install pillow fonttools).
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import random
import struct
import wave
from pathlib import Path

from PIL import Image, ImageDraw

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def save_png(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format="PNG", optimize=True)


def clamp(value: float, low: float = 0.0, high: float = 255.0) -> int:
    return int(max(low, min(high, round(value))))


def noise_tile(size: int, base: tuple[int, int, int], spread: int, seed: int) -> Image.Image:
    rng = random.Random(seed)
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            d = rng.randint(-spread, spread)
            pixels[x, y] = (clamp(base[0] + d), clamp(base[1] + d), clamp(base[2] + d), 255)
    return image


def write_wav(path: Path, samples: list[float], rate: int = 22050) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(b"".join(struct.pack("<h", clamp(s * 32767.0, -32768, 32767)) for s in samples))


# ---------------------------------------------------------------------------
# textures and icons
# ---------------------------------------------------------------------------


def water_tile() -> Image.Image:
    size = 32
    image = noise_tile(size, (38, 92, 168), 6, 11)
    draw = ImageDraw.Draw(image)
    for row in range(4, size, 8):
        for x in range(size):
            y = row + round(1.5 * math.sin((x + row) * 0.45))
            draw.point((x, y % size), fill=(120, 176, 230, 255))
    return image


def brick_tile(seed: int, base: tuple[int, int, int], moss: bool) -> Image.Image:
    size = 32
    rng = random.Random(seed)
    image = noise_tile(size, base, 7, seed)
    draw = ImageDraw.Draw(image)
    mortar = (52, 48, 58, 255)
    for row in range(0, size, 8):
        draw.line([(0, row), (size - 1, row)], fill=mortar)
        offset = 0 if (row // 8) % 2 == 0 else 8
        for x in range(offset, size, 16):
            draw.line([(x, row), (x, row + 7)], fill=mortar)
    if moss:
        pixels = image.load()
        for _ in range(140):
            x, y = rng.randrange(size), rng.randrange(size)
            for dx, dy in ((0, 0), (1, 0), (0, 1)):
                px, py = (x + dx) % size, (y + dy) % size
                pixels[px, py] = (62 + rng.randint(-8, 8), 112 + rng.randint(-10, 10), 48, 255)
    return image


def icon_canvas() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    image = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    return image, ImageDraw.Draw(image)


def icon_canteen(full: bool) -> Image.Image:
    image, draw = icon_canvas()
    draw.ellipse((12, 18, 52, 58), fill=(96, 112, 84, 255), outline=(40, 46, 36, 255), width=3)
    if full:
        draw.ellipse((19, 30, 45, 52), fill=(64, 140, 220, 255))
        draw.ellipse((24, 33, 32, 39), fill=(170, 210, 245, 255))
    else:
        draw.ellipse((19, 30, 45, 52), fill=(70, 80, 62, 255))
    draw.rectangle((27, 6, 37, 20), fill=(140, 96, 56, 255), outline=(70, 48, 28, 255), width=2)
    draw.arc((8, 10, 56, 40), 200, 340, fill=(60, 44, 30, 255), width=3)
    return image


def icon_ration() -> Image.Image:
    image, draw = icon_canvas()
    draw.rounded_rectangle((8, 20, 56, 44), radius=6, fill=(196, 168, 112, 255), outline=(98, 80, 48, 255), width=3)
    draw.rectangle((24, 20, 32, 44), fill=(170, 60, 50, 255))
    for x in (14, 40, 48):
        draw.line([(x, 26), (x + 3, 38)], fill=(150, 120, 74, 255), width=2)
    return image


def icon_berries() -> Image.Image:
    image, draw = icon_canvas()
    draw.line([(32, 8), (32, 22)], fill=(70, 110, 50, 255), width=3)
    draw.polygon([(32, 14), (46, 8), (40, 20)], fill=(84, 150, 60, 255))
    for cx, cy in ((22, 34), (40, 34), (31, 44), (20, 50), (42, 50), (31, 28)):
        draw.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=(146, 34, 88, 255), outline=(80, 14, 46, 255), width=2)
        draw.ellipse((cx - 4, cy - 5, cx - 1, cy - 2), fill=(230, 150, 190, 255))
    return image


def icon_meat(cooked: bool) -> Image.Image:
    image, draw = icon_canvas()
    flesh = (126, 70, 40, 255) if cooked else (196, 58, 64, 255)
    edge = (70, 36, 20, 255) if cooked else (110, 24, 30, 255)
    draw.ellipse((8, 14, 50, 50), fill=flesh, outline=edge, width=3)
    draw.arc((14, 20, 44, 44), 150, 300, fill=(236, 220, 200, 255), width=4)
    draw.rectangle((46, 36, 58, 44), fill=(232, 224, 206, 255), outline=(150, 140, 120, 255), width=2)
    if cooked:
        for x in (18, 26, 34):
            draw.line([(x, 24), (x + 8, 40)], fill=(70, 34, 18, 255), width=2)
    return image


def icon_charm() -> Image.Image:
    image, draw = icon_canvas()
    draw.line([(32, 4), (20, 22)], fill=(150, 130, 90, 255), width=2)
    draw.line([(32, 4), (44, 22)], fill=(150, 130, 90, 255), width=2)
    draw.polygon([(32, 20), (50, 38), (32, 60), (14, 38)], fill=(80, 200, 180, 255), outline=(30, 90, 80, 255))
    draw.polygon([(32, 26), (42, 38), (32, 50)], fill=(170, 240, 225, 255))
    return image


def grass_top() -> Image.Image:
    rng = random.Random(21)
    size = 32
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            pick = rng.random()
            if pick < 0.12:
                color = (186, 120, 46)
            elif pick < 0.2:
                color = (150, 96, 38)
            else:
                color = (128 + rng.randint(-10, 10), 132 + rng.randint(-10, 10), 52 + rng.randint(-6, 6))
            pixels[x, y] = (*color, 255)
    return image


def grass_side() -> Image.Image:
    rng = random.Random(22)
    size = 32
    image = noise_tile(size, (116, 82, 54), 9, 23)
    pixels = image.load()
    for x in range(size):
        depth = 6 + rng.randint(0, 4)
        for y in range(depth):
            pixels[x, y] = (128 + rng.randint(-10, 10), 132 + rng.randint(-10, 10), 52, 255)
    return image


def warm_lut() -> Image.Image:
    n = 16
    image = Image.new("RGB", (n * n, n))
    pixels = image.load()
    for b in range(n):
        for g in range(n):
            for r in range(n):
                rf, gf, bf = r / (n - 1), g / (n - 1), b / (n - 1)
                # Warm the mids, lift the shadows a touch, keep white white.
                rf2 = rf + 0.06 * math.sin(math.pi * rf)
                gf2 = gf + 0.02 * math.sin(math.pi * gf)
                bf2 = bf * 0.9 + 0.03
                pixels[b * n + r, g] = (clamp(rf2 * 255), clamp(gf2 * 255), clamp(bf2 * 255))
    return image


# ---------------------------------------------------------------------------
# UI theme
# ---------------------------------------------------------------------------


def nine_slice(size: int, border: int, fill, frame, corner) -> Image.Image:
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((0, 0, size - 1, size - 1), radius=border, fill=frame)
    draw.rounded_rectangle((border // 2, border // 2, size - 1 - border // 2, size - 1 - border // 2),
                           radius=max(1, border // 2), fill=fill)
    for x, y in ((1, 1), (size - 3, 1), (1, size - 3), (size - 3, size - 3)):
        draw.rectangle((x, y, x + 1, y + 1), fill=corner)
    return image


def cursor_image() -> Image.Image:
    image = Image.new("RGBA", (24, 24), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.polygon([(1, 1), (1, 19), (6, 14), (10, 22), (13, 21), (9, 13), (16, 13)], fill=(250, 214, 120, 255),
                 outline=(40, 24, 12, 255))
    return image


# A 5x7 pixel face drawn for these examples: '#' is ink. Lowercase letters
# sit on the same baseline and use the lower five rows.
GLYPHS = {
    " ": ["....."] * 7,
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    '"': [".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."],
    "#": [".#.#.", "#####", ".#.#.", ".#.#.", "#####", ".#.#.", "....."],
    "$": ["..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."],
    "%": ["##..#", "##.#.", "..#..", ".#...", "#..##", "...##", "....."],
    "&": [".##..", "#..#.", ".##..", ".#..#", "#.##.", "#..#.", ".##.#"],
    "'": ["..#..", "..#..", ".....", ".....", ".....", ".....", "....."],
    "(": ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."],
    ")": [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."],
    "*": [".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."],
    "+": [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."],
    ",": [".....", ".....", ".....", ".....", "..#..", "..#..", ".#..."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    ".": [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    "/": ["....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "..##.", ".#...", "#....", "#####"],
    "3": ["####.", "....#", "....#", ".###.", "....#", "....#", "####."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": [".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."],
    ":": [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
    ";": [".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..."],
    "<": ["...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."],
    "=": [".....", ".....", "#####", ".....", "#####", ".....", "....."],
    ">": [".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "@": [".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####"],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "[": [".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."],
    "\\": ["#....", ".#...", ".#...", "..#..", "...#.", "...#.", "....#"],
    "]": [".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."],
    "^": ["..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."],
    "_": [".....", ".....", ".....", ".....", ".....", ".....", "#####"],
    "`": [".#...", "..#..", ".....", ".....", ".....", ".....", "....."],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "b": ["#....", "#....", "####.", "#...#", "#...#", "#...#", "####."],
    "c": [".....", ".....", ".###.", "#....", "#....", "#...#", ".###."],
    "d": ["....#", "....#", ".####", "#...#", "#...#", "#...#", ".####"],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".###."],
    "f": ["..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#..."],
    "g": [".....", ".####", "#...#", "#...#", ".####", "....#", ".###."],
    "h": ["#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#"],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "j": ["...#.", ".....", "..##.", "...#.", "...#.", "#..#.", ".##.."],
    "k": ["#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."],
    "l": [".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "m": [".....", ".....", "##.#.", "#.#.#", "#.#.#", "#.#.#", "#...#"],
    "n": [".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#"],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "p": [".....", "####.", "#...#", "#...#", "####.", "#....", "#...."],
    "q": [".....", ".####", "#...#", "#...#", ".####", "....#", "....#"],
    "r": [".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    "t": [".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##."],
    "u": [".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#"],
    "v": [".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "w": [".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."],
    "x": [".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"],
    "y": [".....", "#...#", "#...#", "#...#", ".####", "....#", ".###."],
    "z": [".....", ".....", "#####", "...#.", "..#..", ".#...", "#####"],
    "{": ["...##", "..#..", "..#..", ".#...", "..#..", "..#..", "...##"],
    "|": ["..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "}": ["##...", "..#..", "..#..", "...#.", "..#..", "..#..", "##..."],
    "~": [".....", ".....", ".#...", "#.#.#", "...#.", ".....", "....."],
}


def pixel_font(path: Path, family: str) -> None:
    """A TrueType font whose outlines are the 5x7 pixels above."""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    units_per_em = 1000
    pixel = 100  # 7 rows: caps 700 units tall
    descent = 200
    advance = 6 * pixel
    names = {".notdef": ".notdef"}
    cmap = {}
    for char in GLYPHS:
        name = "space" if char == " " else f"uni{ord(char):04X}"
        names[char] = name
        cmap[ord(char)] = name
    order = [".notdef"] + [names[c] for c in GLYPHS]

    # Proportional: each glyph starts at its first inked column and advances
    # by its inked width plus one pixel of spacing (a space is three pixels).
    def columns(rows):
        inked = [col for row in rows for col, cell in enumerate(row) if cell == "#"]
        return (min(inked), max(inked)) if inked else (0, 1)

    def outline(rows, drop=0):
        pen = TTGlyphPen(None)
        first = columns(rows)[0]
        for row_index, row in enumerate(rows):
            for col, cell in enumerate(row):
                if cell != "#":
                    continue
                x0 = (col - first + 0.5) * pixel
                y1 = (7 - row_index - drop) * pixel
                y0 = y1 - pixel
                pen.moveTo((x0, y0))
                pen.lineTo((x0, y1))
                pen.lineTo((x0 + pixel, y1))
                pen.lineTo((x0 + pixel, y0))
                pen.closePath()
        return pen.glyph()

    glyphs = {".notdef": outline(["#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#####"])}
    for char, rows in GLYPHS.items():
        # Lowercase descenders (g j p q y) hang two rows below the baseline.
        glyphs[names[char]] = outline(rows, 2 if char in "gjpqy" else 0)
    builder = FontBuilder(units_per_em, isTTF=True)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap(cmap)
    builder.setupGlyf(glyphs)
    metrics = {".notdef": (advance, 0)}
    for char, rows in GLYPHS.items():
        first, last = columns(rows)
        metrics[names[char]] = (3 * pixel if char == " " else (last - first + 2) * pixel, pixel // 2)
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=800, descent=-descent)
    builder.setupNameTable({"familyName": family, "styleName": "Regular",
                            "copyright": "Drawn for the OpenMinerZ example mods; free to use."})
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-descent, usWinAscent=800, usWinDescent=descent)
    builder.setupPost()
    # Fixed dates, so the file is the same every run (2026-01-01, in the
    # format's seconds since 1904).
    builder.updateHead(created=3849984000, modified=3849984000)
    builder.font.recalcTimestamp = False
    path.parent.mkdir(parents=True, exist_ok=True)
    builder.save(str(path))


# ---------------------------------------------------------------------------
# sounds
# ---------------------------------------------------------------------------


def click_sound() -> list[float]:
    rate = 22050
    rng = random.Random(3)
    out = []
    for i in range(int(rate * 0.05)):
        t = i / rate
        env = math.exp(-t * 90)
        out.append(0.5 * env * (math.sin(2 * math.pi * 1800 * t) * 0.6 + rng.uniform(-1, 1) * 0.4))
    return out


def chime_sound() -> list[float]:
    rate = 22050
    out = []
    for i in range(int(rate * 0.45)):
        t = i / rate
        tone = 660 if t < 0.12 else 990
        env = math.exp(-((t if t < 0.12 else t - 0.12)) * 9)
        out.append(0.35 * env * math.sin(2 * math.pi * tone * t))
    return out


def gulp_sound() -> list[float]:
    rate = 22050
    out = []
    for i in range(int(rate * 0.32)):
        t = i / rate
        pitch = 300 - 500 * t
        env = math.sin(math.pi * min(1.0, t / 0.32)) ** 2
        out.append(0.5 * env * math.sin(2 * math.pi * pitch * t))
    return out


def drone_sound(rate: int) -> list[float]:
    seconds = 4.0
    out = []
    for i in range(int(rate * seconds)):
        t = i / rate
        # Every partial and modulation completes whole cycles in 4 s, so the
        # sound loops without a click.
        lfo = 0.6 + 0.4 * math.sin(2 * math.pi * t / seconds)
        value = (math.sin(2 * math.pi * 55 * t) * 0.5 + math.sin(2 * math.pi * 82.5 * t) * 0.3 +
                 math.sin(2 * math.pi * 110 * t + math.sin(2 * math.pi * t / 2)) * 0.2)
        out.append(0.3 * lfo * value)
    return out


# ---------------------------------------------------------------------------
# Frontier (mod-world): block textures, the ember icon, the ashlands grade and
# three music loops
# ---------------------------------------------------------------------------


def bark_tile() -> Image.Image:
    size = 32
    rng = random.Random(51)
    image = noise_tile(size, (122, 58, 40), 8, 52)
    draw = ImageDraw.Draw(image)
    # Deep vertical furrows.
    for x in range(0, size, 4):
        offset = rng.randint(0, 3)
        for y in range(size):
            wobble = round(math.sin((y + offset * 5) * 0.4))
            draw.point(((x + wobble) % size, y), fill=(74, 32, 22, 255))
    return image


def rings_tile() -> Image.Image:
    size = 32
    image = noise_tile(size, (176, 112, 74), 5, 53)
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - 15.5, y - 15.5)
            if int(d) % 3 == 0:
                r, g, b, a = pixels[x, y]
                pixels[x, y] = (r - 40, g - 30, b - 22, a)
            if d > 14.5:
                pixels[x, y] = (96, 44, 30, 255)
    return image


def needles_tile() -> Image.Image:
    size = 32
    rng = random.Random(54)
    image = noise_tile(size, (30, 70, 42), 8, 66)
    draw = ImageDraw.Draw(image)
    for _ in range(140):
        x, y = rng.randrange(size), rng.randrange(size)
        length = rng.randint(2, 4)
        shade = rng.randint(-14, 14)
        color = (44 + shade, 100 + shade, 58 + shade, 255)
        draw.line([(x, y), (x + length, y + rng.choice((-1, 1)))], fill=color)
    return image


def salt_crust_tile() -> Image.Image:
    size = 32
    image = noise_tile(size, (232, 228, 218), 6, 55)
    draw = ImageDraw.Draw(image)
    rng = random.Random(56)
    # Hairline cracks.
    for _ in range(5):
        x, y = rng.randrange(size), rng.randrange(size)
        for _ in range(10):
            nx, ny = x + rng.randint(-3, 3), y + rng.randint(-3, 3)
            draw.line([(x, y), (nx, ny)], fill=(196, 188, 174, 255))
            x, y = nx % size, ny % size
    return image


def rock_salt_tile() -> Image.Image:
    size = 32
    rng = random.Random(57)
    image = noise_tile(size, (214, 176, 170), 10, 58)
    draw = ImageDraw.Draw(image)
    for _ in range(12):
        x, y = rng.randrange(size), rng.randrange(size)
        s = rng.randint(2, 4)
        draw.rectangle([x, y, x + s, y + s], fill=(240, 214, 208, 255))
    return image


def ash_tile() -> Image.Image:
    size = 32
    rng = random.Random(59)
    image = noise_tile(size, (88, 84, 82), 9, 60)
    pixels = image.load()
    for _ in range(18):
        x, y = rng.randrange(size), rng.randrange(size)
        pixels[x, y] = (200, 90, 40, 255) if rng.random() < 0.3 else (140, 136, 132, 255)
    return image


def cinder_tile() -> Image.Image:
    size = 32
    rng = random.Random(61)
    image = noise_tile(size, (66, 46, 44), 10, 62)
    draw = ImageDraw.Draw(image)
    for _ in range(26):
        x, y = rng.randrange(size), rng.randrange(size)
        draw.ellipse([x, y, x + 2, y + 2], fill=(34, 24, 24, 255))
    return image


def basalt_side_tile() -> Image.Image:
    size = 32
    image = noise_tile(size, (52, 54, 60), 6, 63)
    draw = ImageDraw.Draw(image)
    for x in (0, 10, 21, 31):
        draw.line([(x, 0), (x, size)], fill=(30, 32, 36, 255))
    return image


def basalt_top_tile() -> Image.Image:
    size = 32
    image = noise_tile(size, (60, 62, 68), 5, 64)
    draw = ImageDraw.Draw(image)
    # A hexagonal column end.
    points = [(16 + 13 * math.cos(math.pi / 3 * k), 16 + 13 * math.sin(math.pi / 3 * k)) for k in range(6)]
    draw.polygon(points, outline=(30, 32, 36, 255))
    return image


def ember_ore_tile() -> Image.Image:
    size = 32
    rng = random.Random(65)
    image = cinder_tile()
    draw = ImageDraw.Draw(image)
    for _ in range(7):
        x, y = rng.randrange(2, size - 4), rng.randrange(2, size - 4)
        draw.polygon([(x, y + 2), (x + 2, y), (x + 4, y + 2), (x + 2, y + 4)], fill=(255, 140, 40, 255))
        draw.point((x + 2, y + 2), fill=(255, 230, 150, 255))
    return image


def icon_ember() -> Image.Image:
    image, draw = icon_canvas()
    draw.polygon([(32, 6), (48, 30), (40, 58), (22, 56), (16, 28)], fill=(220, 90, 30, 255),
                 outline=(110, 40, 16, 255))
    draw.polygon([(32, 16), (40, 32), (34, 48), (26, 46), (24, 30)], fill=(255, 190, 80, 255))
    draw.polygon([(31, 24), (34, 32), (30, 40), (28, 32)], fill=(255, 246, 200, 255))
    return image


def ashlands_lut() -> Image.Image:
    n = 16
    image = Image.new("RGB", (n * n, n))
    pixels = image.load()
    for b in range(n):
        for g in range(n):
            for r in range(n):
                rf, gf, bf = r / (n - 1), g / (n - 1), b / (n - 1)
                luma = 0.3 * rf + 0.59 * gf + 0.11 * bf
                # Ember shadows, smoky mids, near-grey highlights.
                rf2 = luma + (rf - luma) * 0.7 + 0.08 * (1 - luma)
                gf2 = luma + (gf - luma) * 0.7 + 0.01
                bf2 = luma + (bf - luma) * 0.6 - 0.04 * (1 - luma)
                pixels[b * n + r, g] = (clamp(rf2 * 255), clamp(gf2 * 255), clamp(bf2 * 255))
    return image


def music_loop(rate: int, seconds: float, notes: list[float], partials: list[float], pulse: float,
               brightness: float) -> list[float]:
    """An ambient pad: each note swells once per loop, and every frequency
    completes whole cycles in the loop, so it repeats without a click."""
    out = []
    total = int(rate * seconds)
    step = seconds / len(notes)
    for i in range(total):
        t = i / rate
        value = 0.0
        for index, note in enumerate(notes):
            frequency = round(note * seconds) / seconds
            phase = (t - index * step) / seconds
            envelope = 0.5 - 0.5 * math.cos(2 * math.pi * phase)
            for k, weight in enumerate(partials):
                value += envelope * weight * (brightness ** k) * math.sin(2 * math.pi * frequency * (k + 1) * t)
        tremolo = 1.0 - pulse * (0.5 - 0.5 * math.cos(2 * math.pi * 8 * t / seconds))
        out.append(0.16 * tremolo * value)
    return out


# ---------------------------------------------------------------------------
# models: boxes on a small skeleton of nodes, with idle/walk/run/attack/hurt
# ---------------------------------------------------------------------------


class Box:
    def __init__(self, name, parent, pivot, center, size, color):
        self.name, self.parent, self.pivot = name, parent, pivot
        self.center, self.size, self.color = center, size, color


def box_mesh(center, size):
    cx, cy, cz = center
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    faces = [
        ((1, 0, 0), [(hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz), (hx, -hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz), (-hx, -hy, -hz)]),
        ((0, 1, 0), [(-hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, hz), (-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz)]),
        ((0, 0, 1), [(hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz), (-hx, -hy, hz)]),
        ((0, 0, -1), [(-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz), (hx, -hy, -hz)]),
    ]
    positions, normals, indices = [], [], []
    for normal, corners in faces:
        base = len(positions)
        for x, y, z in corners:
            positions.append((cx + x, cy + y, cz + z))
            normals.append(normal)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, indices


def quat_x(degrees):
    r = math.radians(degrees) / 2
    return (math.sin(r), 0.0, 0.0, math.cos(r))


def quat_y(degrees):
    r = math.radians(degrees) / 2
    return (0.0, math.sin(r), 0.0, math.cos(r))


def write_model(path: Path, boxes: list[Box], clips: dict[str, tuple[float, dict[str, list]]]) -> None:
    """clips: name -> (duration, {node: [(time, quaternion) ...]})"""
    buffer = bytearray()
    accessors, views, meshes, nodes, materials = [], [], [], [], []

    def add_view(data: bytes, target=None):
        while len(buffer) % 4:
            buffer.append(0)
        view = {"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)}
        if target:
            view["target"] = target
        buffer.extend(data)
        views.append(view)
        return len(views) - 1

    def add_accessor(values, kind, component, count, extra=None):
        accessor = {"bufferView": values, "componentType": component, "count": count, "type": kind}
        if extra:
            accessor.update(extra)
        accessors.append(accessor)
        return len(accessors) - 1

    names = [b.name for b in boxes]
    for box in boxes:
        materials.append({"name": box.name, "pbrMetallicRoughness": {
            "baseColorFactor": [c / 255 for c in box.color] + [1.0], "metallicFactor": 0.0, "roughnessFactor": 0.9}})
        local_center = tuple(box.center[i] - box.pivot[i] for i in range(3))
        positions, normals, indices = box_mesh(local_center, box.size)
        p = add_view(b"".join(struct.pack("<3f", *v) for v in positions), 34962)
        n = add_view(b"".join(struct.pack("<3f", *v) for v in normals), 34962)
        i = add_view(b"".join(struct.pack("<H", v) for v in indices), 34963)
        lo = [min(v[k] for v in positions) for k in range(3)]
        hi = [max(v[k] for v in positions) for k in range(3)]
        pa = add_accessor(p, "VEC3", 5126, len(positions), {"min": lo, "max": hi})
        na = add_accessor(n, "VEC3", 5126, len(normals))
        ia = add_accessor(i, "SCALAR", 5123, len(indices))
        meshes.append({"name": box.name, "primitives": [
            {"attributes": {"POSITION": pa, "NORMAL": na}, "indices": ia, "material": len(materials) - 1}]})
    for index, box in enumerate(boxes):
        parent_pivot = boxes[names.index(box.parent)].pivot if box.parent else (0.0, 0.0, 0.0)
        node = {"name": box.name, "mesh": index,
                "translation": [box.pivot[k] - parent_pivot[k] for k in range(3)]}
        children = [j for j, other in enumerate(boxes) if other.parent == box.name]
        if children:
            node["children"] = children
        nodes.append(node)
    animations = []
    for clip, (duration, tracks) in clips.items():
        samplers, channels = [], []
        for node_name, keys in tracks.items():
            times = add_view(b"".join(struct.pack("<f", t) for t, _ in keys))
            values = add_view(b"".join(struct.pack("<4f", *q) for _, q in keys))
            ta = add_accessor(times, "SCALAR", 5126, len(keys), {"min": [keys[0][0]], "max": [keys[-1][0]]})
            va = add_accessor(values, "VEC4", 5126, len(keys))
            samplers.append({"input": ta, "output": va, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": names.index(node_name), "path": "rotation"}})
        animations.append({"name": clip, "samplers": samplers, "channels": channels})
    roots = [i for i, b in enumerate(boxes) if not b.parent]
    document = {
        "asset": {"version": "2.0", "generator": "OpenMinerZ make_example_assets.py"},
        "scene": 0, "scenes": [{"nodes": roots}], "nodes": nodes, "meshes": meshes, "materials": materials,
        "accessors": accessors, "bufferViews": views, "animations": animations,
        "buffers": [{"byteLength": len(buffer),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")


def swing(degrees: float, period: float, phase: float = 0.0, axis=quat_x, steps: int = 8):
    return [(period * k / steps, axis(degrees * math.sin(2 * math.pi * (k / steps + phase)))) for k in range(steps + 1)]


def quadruped(path: Path, sizes, palette, attack=False):
    """A four-legged animal facing +Z: body, head and four legs."""
    bw, bh, bl = sizes["body"]
    leg_h = sizes["leg"]
    body_y = leg_h + bh / 2
    boxes = [Box("body", None, (0, body_y, 0), (0, body_y, 0), (bw, bh, bl), palette["body"])]
    hx, hy, hz = sizes["head"]
    neck = (0, body_y + bh * 0.3, bl / 2)
    boxes.append(Box("head", "body", neck, (0, neck[1] + hy * 0.35, bl / 2 + hz * 0.45), (hx, hy, hz), palette["head"]))
    if "ears" in sizes:
        ex, ey, ez = sizes["ears"]
        for side in (-1, 1):
            boxes.append(Box(f"ear_{'l' if side < 0 else 'r'}", "head", neck,
                             (side * hx * 0.3, neck[1] + hy * 0.85 + ey / 2, bl / 2 + hz * 0.3), (ex, ey, ez),
                             palette["head"]))
    leg_w = sizes["leg_w"]
    for name, sx, sz in (("leg_fl", -1, 1), ("leg_fr", 1, 1), ("leg_bl", -1, -1), ("leg_br", 1, -1)):
        hip = (sx * (bw / 2 - leg_w / 2), leg_h, sz * (bl / 2 - leg_w))
        boxes.append(Box(name, "body", hip, (hip[0], leg_h / 2, hip[2]), (leg_w, leg_h, leg_w), palette["legs"]))
    if "tail" in sizes:
        tx, ty, tz = sizes["tail"]
        root = (0, body_y + bh * 0.25, -bl / 2)
        boxes.append(Box("tail", "body", root, (0, root[1], -bl / 2 - tz / 2), (tx, ty, tz), palette["head"]))
    walk = {"leg_fl": swing(28, 0.8), "leg_br": swing(28, 0.8), "leg_fr": swing(28, 0.8, 0.5),
            "leg_bl": swing(28, 0.8, 0.5), "head": swing(4, 0.8, 0.25)}
    run = {name: swing(45, 0.45, phase) for name, phase in (("leg_fl", 0), ("leg_fr", 0.15), ("leg_bl", 0.5),
                                                              ("leg_br", 0.65))}
    run["head"] = swing(8, 0.45, 0.25)
    idle = {"head": swing(6, 2.4, 0.0, quat_x, 6)}
    if "tail" in sizes:
        idle["tail"] = swing(20, 1.2, 0.0, quat_y, 6)
    hurt = {"body": [(0.0, quat_x(0)), (0.1, quat_x(-12)), (0.3, quat_x(0))]}
    clips = {"idle": (2.4, idle), "walk": (0.8, walk), "run": (0.45, run), "hurt": (0.3, hurt)}
    if attack:
        clips["attack"] = (0.6, {"head": [(0.0, quat_x(0)), (0.2, quat_x(25)), (0.35, quat_x(-10)), (0.6, quat_x(0))]})
    write_model(path, boxes, clips)


def biped(path: Path, palette, height=1.8, attack=False, hunch=0.0):
    """A person-shaped figure facing +Z: body, head, arms, legs."""
    leg_h = height * 0.45
    body_h = height * 0.33
    head = height * 0.2
    boxes = [
        Box("body", None, (0, leg_h, 0), (0, leg_h + body_h / 2, 0), (0.5, body_h, 0.28), palette["body"]),
        Box("head", "body", (0, leg_h + body_h, 0), (0, leg_h + body_h + head / 2, 0.02), (head, head, head),
            palette["head"]),
        Box("arm_l", "body", (-0.33, leg_h + body_h * 0.92, 0), (-0.33, leg_h + body_h * 0.45, 0),
            (0.16, body_h * 0.95, 0.16), palette["arms"]),
        Box("arm_r", "body", (0.33, leg_h + body_h * 0.92, 0), (0.33, leg_h + body_h * 0.45, 0),
            (0.16, body_h * 0.95, 0.16), palette["arms"]),
        Box("leg_l", "body", (-0.13, leg_h, 0), (-0.13, leg_h / 2, 0), (0.2, leg_h, 0.2), palette["legs"]),
        Box("leg_r", "body", (0.13, leg_h, 0), (0.13, leg_h / 2, 0), (0.2, leg_h, 0.2), palette["legs"]),
    ]
    if "hood" in palette:
        boxes.append(Box("hood", "head", (0, leg_h + body_h, 0), (0, leg_h + body_h + head * 0.62, -0.04),
                         (head * 1.15, head * 0.5, head * 1.15), palette["hood"]))
    lean = [(0.0, quat_x(hunch)), (1.0, quat_x(hunch))]
    walk = {"leg_l": swing(30, 1.0), "leg_r": swing(30, 1.0, 0.5), "arm_l": swing(25, 1.0, 0.5),
            "arm_r": swing(25, 1.0), "body": lean}
    run = {"leg_l": swing(50, 0.6), "leg_r": swing(50, 0.6, 0.5), "arm_l": swing(45, 0.6, 0.5),
           "arm_r": swing(45, 0.6), "body": lean}
    idle = {"head": swing(5, 3.0, 0.0, quat_y, 6), "arm_l": swing(3, 3.0), "arm_r": swing(3, 3.0, 0.5),
            "body": lean}
    clips = {"idle": (3.0, idle), "walk": (1.0, walk), "run": (0.6, run),
             "hurt": (0.3, {"body": [(0.0, quat_x(hunch)), (0.1, quat_x(hunch - 15)), (0.3, quat_x(hunch))]})}
    if attack:
        clips["attack"] = (0.7, {"arm_l": [(0.0, quat_x(0)), (0.3, quat_x(-110)), (0.5, quat_x(20)), (0.7, quat_x(0))],
                                 "arm_r": [(0.0, quat_x(0)), (0.3, quat_x(-110)), (0.5, quat_x(20)), (0.7, quat_x(0))]})
    write_model(path, boxes, clips)


# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2] / "examples" / "mods"))
    root = Path(parser.parse_args().root)

    survival = root / "survival" / "assets"
    save_png(water_tile(), survival / "textures" / "fresh_water.png")
    save_png(icon_canteen(False), survival / "textures" / "icons" / "canteen.png")
    save_png(icon_canteen(True), survival / "textures" / "icons" / "canteen_full.png")
    save_png(icon_ration(), survival / "textures" / "icons" / "ration.png")
    save_png(icon_berries(), survival / "textures" / "icons" / "berries.png")
    write_wav(survival / "sounds" / "gulp.wav", gulp_sound())

    dungeon = root / "story_dungeon" / "assets"
    save_png(brick_tile(31, (92, 86, 104), False), dungeon / "textures" / "crypt_brick.png")
    save_png(brick_tile(32, (86, 90, 92), True), dungeon / "textures" / "mossy_brick.png")
    save_png(icon_charm(), dungeon / "textures" / "icons" / "hermit_charm.png")
    biped(dungeon / "models" / "hermit.gltf",
          {"body": (92, 70, 120), "head": (214, 176, 140), "arms": (92, 70, 120), "legs": (60, 50, 70),
           "hood": (70, 52, 96)}, hunch=8.0)
    biped(dungeon / "models" / "ghoul.gltf",
          {"body": (70, 92, 74), "head": (150, 176, 140), "arms": (120, 150, 112), "legs": (54, 60, 50)},
          height=1.9, attack=True, hunch=18.0)
    write_wav(dungeon / "sounds" / "crypt_drone.wav", drone_sound(16000), rate=16000)

    wildlife = root / "wildlife" / "assets"
    quadruped(wildlife / "models" / "deer.gltf",
              {"body": (0.45, 0.5, 1.1), "leg": 0.75, "leg_w": 0.13, "head": (0.28, 0.32, 0.42),
               "ears": (0.08, 0.16, 0.05), "tail": (0.1, 0.12, 0.12)},
              {"body": (150, 104, 66), "head": (162, 116, 76), "legs": (120, 84, 54)})
    quadruped(wildlife / "models" / "rabbit.gltf",
              {"body": (0.26, 0.24, 0.42), "leg": 0.12, "leg_w": 0.08, "head": (0.2, 0.2, 0.2),
               "ears": (0.05, 0.2, 0.04), "tail": (0.1, 0.1, 0.08)},
              {"body": (180, 170, 156), "head": (190, 180, 166), "legs": (160, 150, 136)})
    quadruped(wildlife / "models" / "boar.gltf",
              {"body": (0.6, 0.55, 1.0), "leg": 0.32, "leg_w": 0.16, "head": (0.4, 0.4, 0.45),
               "ears": (0.1, 0.12, 0.05), "tail": (0.05, 0.05, 0.16)},
              {"body": (86, 62, 48), "head": (96, 70, 54), "legs": (64, 48, 38)}, attack=True)
    save_png(icon_meat(False), wildlife / "textures" / "icons" / "raw_venison.png")
    save_png(icon_meat(True), wildlife / "textures" / "icons" / "cooked_venison.png")

    theme = root / "ui_theme" / "assets"
    save_png(nine_slice(32, 8, (34, 28, 40, 236), (120, 86, 48, 255), (240, 200, 110, 255)),
             theme / "ui" / "panel.png")
    save_png(nine_slice(24, 6, (54, 46, 62, 240), (96, 74, 52, 255), (200, 170, 110, 255)),
             theme / "ui" / "button.png")
    save_png(nine_slice(24, 6, (96, 62, 30, 250), (240, 180, 80, 255), (255, 236, 170, 255)),
             theme / "ui" / "button_focus.png")
    save_png(nine_slice(24, 4, (20, 18, 26, 220), (90, 74, 58, 255), (150, 120, 80, 255)),
             theme / "ui" / "slot.png")
    save_png(cursor_image(), theme / "ui" / "cursor.png")
    pixel_font(theme / "fonts" / "EmberPixel.ttf", "Ember Pixel")
    write_wav(theme / "ui" / "click.wav", click_sound())
    write_wav(theme / "ui" / "popup.wav", chime_sound())

    lighting = root / "lighting_pack" / "assets"
    save_png(grass_top(), lighting / "textures" / "blocks" / "2.png")
    save_png(grass_side(), lighting / "textures" / "blocks" / "1.png")
    save_png(warm_lut(), lighting / "luts" / "warm_evening.png")
    frontier = root / "frontier" / "assets"
    save_png(bark_tile(), frontier / "textures" / "redwood_bark.png")
    save_png(rings_tile(), frontier / "textures" / "redwood_rings.png")
    save_png(needles_tile(), frontier / "textures" / "redwood_needles.png")
    save_png(salt_crust_tile(), frontier / "textures" / "salt_crust.png")
    save_png(rock_salt_tile(), frontier / "textures" / "rock_salt.png")
    save_png(ash_tile(), frontier / "textures" / "ash.png")
    save_png(cinder_tile(), frontier / "textures" / "cinder_stone.png")
    save_png(basalt_side_tile(), frontier / "textures" / "basalt_side.png")
    save_png(basalt_top_tile(), frontier / "textures" / "basalt_top.png")
    save_png(ember_ore_tile(), frontier / "textures" / "ember_ore.png")
    save_png(icon_ember(), frontier / "textures" / "icons" / "ember.png")
    save_png(ashlands_lut(), frontier / "luts" / "ashlands.png")
    # A calm major pad under the redwoods, a high shimmer on the salt and a
    # low minor drone in the ashlands.
    write_wav(frontier / "music" / "redwood.wav",
              music_loop(11025, 8.0, [196.0, 246.94, 293.66, 392.0], [1.0, 0.35, 0.12], 0.15, 0.8), rate=11025)
    write_wav(frontier / "music" / "salt_flats.wav",
              music_loop(11025, 8.0, [523.25, 659.25, 783.99, 987.77], [1.0, 0.2], 0.35, 0.6), rate=11025)
    write_wav(frontier / "music" / "ashlands.wav",
              music_loop(11025, 8.0, [73.42, 87.31, 110.0, 130.81], [1.0, 0.5, 0.3, 0.15], 0.25, 0.9), rate=11025)
    print(f"example assets written under {root}")


if __name__ == "__main__":
    main()
