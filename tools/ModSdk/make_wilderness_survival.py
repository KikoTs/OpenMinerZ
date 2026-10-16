"""Draws the survival half of the Wilderness example (examples/mods/wilderness).

The farm animals (cow, pig, chicken, sheep) as low-poly glTF figures with
idle/walk/run/hurt/death clips, the icons of the food, drink and container
items, the hunger and thirst HUD icons, and the sounds (eating, drinking,
filling, milking, the animals' calls). Made the way make_example_assets.py
makes the other examples' art, with its helpers; nothing comes from
CastleMiner Z. Deterministic: running it again rewrites the same bytes.

    py -3 tools/ModSdk/make_wilderness_survival.py [--root examples/mods]

Needs Python 3.10+ and Pillow.
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import math
import random
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_example_assets import (  # noqa: E402  (the shared helpers)
    clamp,
    gulp_sound,
    icon_berries,
    icon_canvas,
    quat_x,
    quat_y,
    save_png,
    write_wav,
)


def quat_z(degrees):
    r = math.radians(degrees) / 2
    return (0.0, 0.0, math.sin(r), math.cos(r))


def quat_mul(a, b):
    """a then... the Hamilton product a*b (b applied first)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


IDENTITY = (0.0, 0.0, 0.0, 1.0)

# ---------------------------------------------------------------------------
# models: boxes on a node skeleton, textured or plain, with rotation and
# translation clips (translations are offsets from the rest pose)
# ---------------------------------------------------------------------------


@dataclass
class Material:
    color: tuple[int, int, int]
    texture: Image.Image | None = None
    texels: float = 48.0  # texture pixels per block along a face
    # (low, high): each face takes a patch of the texture whose share of dark
    # pixels falls in this range, so no face of a spotted hide is all spot.
    dark_share: tuple[float, float] | None = None


def dark_fraction(texture: Image.Image, box) -> float:
    region = texture.crop(tuple(int(round(v)) for v in box)).convert("L")
    values = list(region.getdata())
    return sum(1 for v in values if v < 110) / max(1, len(values))


@dataclass
class Part:
    name: str
    parent: str | None
    pivot: tuple[float, float, float]
    center: tuple[float, float, float] | None = None  # None: a joint without a box
    size: tuple[float, float, float] | None = None
    material: str | None = None
    rest: tuple[float, float, float, float] = IDENTITY  # a fixed turn about the pivot


# The four axes a face's texture runs along: (u axis, v axis) per normal axis.
FACE_AXES = {0: (2, 1), 1: (0, 2), 2: (0, 1)}


def textured_box(center, size, material: Material | None, rng: random.Random):
    """Positions, normals, uvs and indices of an axis-aligned box. A textured
    material maps each face to its own patch of the texture, sized by the
    face, so a spotted hide is spotted alike on every face."""
    cx, cy, cz = center
    half = (size[0] / 2, size[1] / 2, size[2] / 2)
    hx, hy, hz = half
    faces = [
        ((1, 0, 0), [(hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz), (hx, -hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz), (-hx, -hy, -hz)]),
        ((0, 1, 0), [(-hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, hz), (-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz)]),
        ((0, 0, 1), [(hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz), (-hx, -hy, hz)]),
        ((0, 0, -1), [(-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz), (hx, -hy, -hz)]),
    ]
    positions, normals, uvs, indices = [], [], [], []
    texture = material.texture if material is not None else None
    for normal, corners in faces:
        axis = next(k for k in range(3) if normal[k] != 0)
        ua, va = FACE_AXES[axis]
        rect = (0.0, 0.0, 1.0, 1.0)
        if texture is not None:
            tw, th = texture.size
            pw = min(tw, max(2.0, size[ua] * material.texels))
            ph = min(th, max(2.0, size[va] * material.texels))
            x0 = rng.uniform(0, tw - pw)
            y0 = rng.uniform(0, th - ph)
            if material.dark_share is not None:
                low, high = material.dark_share
                best = None
                for _ in range(40):
                    share = dark_fraction(texture, (x0, y0, x0 + pw, y0 + ph))
                    miss = max(0.0, low - share, share - high)
                    if best is None or miss < best[0]:
                        best = (miss, x0, y0)
                    if miss == 0.0:
                        break
                    x0, y0 = rng.uniform(0, tw - pw), rng.uniform(0, th - ph)
                _, x0, y0 = best
            rect = (x0 / tw, y0 / th, (x0 + pw) / tw, (y0 + ph) / th)
        base = len(positions)
        for corner in corners:
            positions.append((cx + corner[0], cy + corner[1], cz + corner[2]))
            normals.append(normal)
            u = (corner[ua] + half[ua]) / (2 * half[ua]) if half[ua] > 0 else 0.0
            v = 1.0 - (corner[va] + half[va]) / (2 * half[va]) if half[va] > 0 else 0.0
            if va != 1:  # a top or bottom face: v runs along z
                v = 1.0 - v
            uvs.append((rect[0] + u * (rect[2] - rect[0]), rect[1] + v * (rect[3] - rect[1])))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def png_bytes(image: Image.Image) -> bytes:
    out = io.BytesIO()
    image.save(out, format="PNG", optimize=True)
    return out.getvalue()


def write_figure(path: Path, parts: list[Part], materials: dict[str, Material], clips: dict, seed: int = 1) -> None:
    """clips: name -> (duration, {node: {"r": [(t, quat)], "t": [(t, (dx, dy, dz))]}})"""
    rng = random.Random(seed)
    buffer = bytearray()
    accessors, views, meshes, nodes = [], [], [], []

    def add_view(data: bytes, target=None):
        while len(buffer) % 4:
            buffer.append(0)
        view = {"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)}
        if target:
            view["target"] = target
        buffer.extend(data)
        views.append(view)
        return len(views) - 1

    def add_accessor(view, kind, component, count, extra=None):
        accessor = {"bufferView": view, "componentType": component, "count": count, "type": kind}
        if extra:
            accessor.update(extra)
        accessors.append(accessor)
        return len(accessors) - 1

    material_names = sorted(materials)
    gltf_materials, images, textures = [], [], []
    for name in material_names:
        material = materials[name]
        pbr = {"metallicFactor": 0.0, "roughnessFactor": 0.92}
        if material.texture is not None:
            view = add_view(png_bytes(material.texture))
            images.append({"bufferView": view, "mimeType": "image/png"})
            textures.append({"source": len(images) - 1, "sampler": 0})
            pbr["baseColorTexture"] = {"index": len(textures) - 1}
            pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
        else:
            pbr["baseColorFactor"] = [c / 255 for c in material.color] + [1.0]
        gltf_materials.append({"name": name, "pbrMetallicRoughness": pbr})

    names = [p.name for p in parts]
    by_name = {p.name: p for p in parts}
    for part in parts:
        node = {"name": part.name}
        parent_pivot = by_name[part.parent].pivot if part.parent else (0.0, 0.0, 0.0)
        node["translation"] = [round(part.pivot[k] - parent_pivot[k], 6) for k in range(3)]
        if part.rest != IDENTITY:
            node["rotation"] = [round(c, 6) for c in part.rest]
        if part.center is not None:
            material = materials[part.material]
            local = tuple(part.center[k] - part.pivot[k] for k in range(3))
            positions, normals, uvs, indices = textured_box(local, part.size, material, rng)
            p = add_view(b"".join(struct.pack("<3f", *v) for v in positions), 34962)
            n = add_view(b"".join(struct.pack("<3f", *v) for v in normals), 34962)
            lo = [min(v[k] for v in positions) for k in range(3)]
            hi = [max(v[k] for v in positions) for k in range(3)]
            attributes = {"POSITION": add_accessor(p, "VEC3", 5126, len(positions), {"min": lo, "max": hi}),
                          "NORMAL": add_accessor(n, "VEC3", 5126, len(normals))}
            if material.texture is not None:
                t = add_view(b"".join(struct.pack("<2f", *v) for v in uvs), 34962)
                attributes["TEXCOORD_0"] = add_accessor(t, "VEC2", 5126, len(uvs))
            i = add_view(b"".join(struct.pack("<H", v) for v in indices), 34963)
            meshes.append({"name": part.name, "primitives": [
                {"attributes": attributes, "indices": add_accessor(i, "SCALAR", 5123, len(indices)),
                 "material": material_names.index(part.material)}]})
            node["mesh"] = len(meshes) - 1
        children = [j for j, other in enumerate(parts) if other.parent == part.name]
        if children:
            node["children"] = children
        nodes.append(node)

    animations = []
    for clip, (duration, tracks) in clips.items():
        samplers, channels = [], []
        for node_name, channels_of in tracks.items():
            part = by_name[node_name]
            for channel, frames in channels_of.items():
                if channel == "r":
                    values = [quat_mul(q, part.rest) for _, q in frames]
                    packed = b"".join(struct.pack("<4f", *q) for q in values)
                    kind, target = "VEC4", "rotation"
                else:
                    rest = nodes[names.index(node_name)]["translation"]
                    values = [tuple(rest[k] + offset[k] for k in range(3)) for _, offset in frames]
                    packed = b"".join(struct.pack("<3f", *v) for v in values)
                    kind, target = "VEC3", "translation"
                times = add_view(b"".join(struct.pack("<f", t) for t, _ in frames))
                ta = add_accessor(times, "SCALAR", 5126, len(frames), {"min": [frames[0][0]], "max": [frames[-1][0]]})
                va = add_accessor(add_view(packed), kind, 5126, len(frames))
                samplers.append({"input": ta, "output": va, "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1,
                                 "target": {"node": names.index(node_name), "path": target}})
        # Every clip runs its full length, even when its tracks end early.
        animations.append({"name": clip, "samplers": samplers, "channels": channels})
        del duration
    roots = [i for i, p in enumerate(parts) if not p.parent]
    document = {
        "asset": {"version": "2.0", "generator": "OpenMinerZ make_wilderness_survival.py"},
        "scene": 0, "scenes": [{"nodes": roots}], "nodes": nodes, "meshes": meshes, "materials": gltf_materials,
        "accessors": accessors, "bufferViews": views, "animations": animations,
        "buffers": [{"byteLength": len(buffer),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    if images:
        document["images"] = images
        document["textures"] = textures
        # Nearest filtering keeps the hides crisp, like the game's blocks.
        document["samplers"] = [{"magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071}]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# clip helpers
# ---------------------------------------------------------------------------


def wave(degrees, period, phase=0.0, axis=quat_x, steps=8):
    """A rotation swinging +-degrees, looping every period."""
    return [(period * k / steps, axis(degrees * math.sin(2 * math.pi * (k / steps + phase)))) for k in range(steps + 1)]


def bob(amount, period, phase=0.0, steps=8):
    """An up-and-down offset, looping every period."""
    return [(period * k / steps, (0.0, amount * math.sin(2 * math.pi * (k / steps + phase)), 0.0))
            for k in range(steps + 1)]


def keys(axis, pairs):
    """[(time, degrees)] -> rotation keys about one axis."""
    return [(t, axis(d)) for t, d in pairs]


def flaps(degrees, duration, cycles, axis=quat_z, steps_per_cycle=4):
    """A wing beat: out to `degrees` and back, `cycles` times in `duration`."""
    steps = cycles * steps_per_cycle
    return [(duration * k / steps, axis(degrees * (0.5 - 0.5 * math.cos(2 * math.pi * cycles * k / steps))))
            for k in range(steps + 1)]


def gait(legs, period, swing_deg, pattern):
    """Leg tracks: pattern maps a leg name to its phase (0..1) in the cycle."""
    return {leg: {"r": wave(swing_deg, period, pattern[leg])} for leg in legs}


def merge(*track_sets):
    out: dict = {}
    for tracks in track_sets:
        for node, channels in tracks.items():
            out.setdefault(node, {}).update(channels)
    return out


def death_clip(width, side_lift, duration=0.9, legs=(), leg_kick=25.0):
    """Topples onto its left side: the root turns 90 degrees about the feet
    and rises by half the body's width, so the body lies on the ground."""
    root = {"r": keys(quat_z, [(0.0, 0), (duration * 0.55, 70), (duration * 0.8, 92), (duration, 90)]),
            "t": [(0.0, (0, 0, 0)), (duration * 0.55, (width * 0.3, side_lift * 0.7, 0)),
                  (duration, (width * 0.5, side_lift, 0))]}
    tracks = {"root": root}
    for index, leg in enumerate(legs):
        sign = 1 if index % 2 == 0 else -1
        tracks[leg] = {"r": keys(quat_x, [(0.0, 0), (duration * 0.6, sign * leg_kick), (duration, sign * 8)])}
    return tracks


# ---------------------------------------------------------------------------
# hides
# ---------------------------------------------------------------------------


def blob_hide(size, base, patch, blobs, seed, speckle=8):
    """A light hide with big dark patches (a Holstein cow's coat)."""
    rng = random.Random(seed)
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    centers = [(rng.uniform(0, size), rng.uniform(0, size), rng.uniform(size * 0.08, size * 0.17),
                rng.uniform(0.6, 1.4)) for _ in range(blobs)]
    for y in range(size):
        for x in range(size):
            field = 0.0
            for cx, cy, r, stretch in centers:
                dx = (x - cx) / stretch
                dy = (y - cy) * stretch
                field += r * r / (dx * dx + dy * dy + 1.0)
            wobble = 0.12 * math.sin(x * 0.7) * math.cos(y * 0.6)
            dark = field + wobble > 1.0
            tone = patch if dark else base
            d = rng.randint(-speckle, speckle)
            pixels[x, y] = (clamp(tone[0] + d), clamp(tone[1] + d), clamp(tone[2] + d), 255)
    return image


def fleece(size, base, seed):
    """Sheep's wool: soft round curls, lighter on top."""
    rng = random.Random(seed)
    image = Image.new("RGBA", (size, size), (*base, 255))
    draw = ImageDraw.Draw(image)
    for _ in range(size * size // 10):
        x, y = rng.uniform(-3, size + 3), rng.uniform(-3, size + 3)
        r = rng.uniform(2.0, 4.2)
        shade = rng.randint(-26, 14)
        draw.ellipse((x - r, y - r, x + r, y + r),
                     fill=(clamp(base[0] + shade), clamp(base[1] + shade), clamp(base[2] + shade - 4), 255))
        draw.arc((x - r, y - r, x + r, y + r), 200, 320, fill=(clamp(base[0] + 22), clamp(base[1] + 22),
                                                                clamp(base[2] + 18), 255))
    return image


def mottle(size, base, spread, seed, spots=None):
    """A plain skin with a little colour noise and a few soft blotches."""
    rng = random.Random(seed)
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            d = rng.randint(-spread, spread)
            pixels[x, y] = (clamp(base[0] + d), clamp(base[1] + d), clamp(base[2] + d), 255)
    if spots:
        draw = ImageDraw.Draw(image)
        color, count = spots
        for _ in range(count):
            x, y, r = rng.uniform(0, size), rng.uniform(0, size), rng.uniform(1.5, 3.5)
            draw.ellipse((x - r, y - r, x + r, y + r), fill=(*color, 255))
    return image


def feathers(size, base, seed):
    """White plumage: rows of overlapping feather tips."""
    rng = random.Random(seed)
    image = Image.new("RGBA", (size, size), (*base, 255))
    draw = ImageDraw.Draw(image)
    for row in range(0, size + 6, 5):
        for col in range(-4, size + 6, 6):
            x = col + (3 if (row // 5) % 2 else 0) + rng.uniform(-0.8, 0.8)
            shade = rng.randint(-20, 6)
            draw.arc((x - 3, row - 4, x + 3, row + 2), 20, 160,
                     fill=(clamp(base[0] + shade - 14), clamp(base[1] + shade - 14), clamp(base[2] + shade - 8), 255))
    return image


# ---------------------------------------------------------------------------
# the animals (facing +Z, one unit per block, feet at the origin)
# ---------------------------------------------------------------------------

LEGS = ("leg_fl", "leg_fr", "leg_bl", "leg_br")


def legs_of(hip_y, x, z_front, z_back, width, material, hoof=None, top_extra=0.02):
    parts = []
    for name, sx, zz in (("leg_fl", -1, z_front), ("leg_fr", 1, z_front), ("leg_bl", -1, z_back),
                         ("leg_br", 1, z_back)):
        hip = (sx * x, hip_y, zz)
        parts.append(Part(name, "body", hip, (sx * x, (hip_y + top_extra) / 2, zz), (width, hip_y + top_extra, width),
                          material))
        if hoof is not None:
            hoof_material, hoof_h = hoof
            parts.append(Part(name + "_hoof", name, hip, (sx * x, hoof_h / 2, zz),
                              (width + 0.02, hoof_h, width + 0.02), hoof_material))
    return parts


def cow(path: Path) -> None:
    """A black-and-white dairy cow: spotted hide, pink muzzle and udder,
    cream horns, a tufted tail."""
    hide = blob_hide(128, (238, 234, 226), (34, 31, 32), 6, seed=71)
    materials = {
        "hide": Material((238, 234, 226), hide, texels=46, dark_share=(0.2, 0.45)),
        "leg_hide": Material((238, 234, 226), hide, texels=46, dark_share=(0.0, 0.5)),
        "white": Material((236, 232, 224)),
        "black": Material((38, 35, 36)),
        "muzzle": Material((232, 168, 160)),
        "nostril": Material((120, 58, 60)),
        "udder": Material((240, 166, 172)),
        "horn": Material((232, 222, 194)),
        "hoof": Material((58, 48, 44)),
        "eye": Material((22, 18, 18)),
        "glint": Material((250, 250, 250)),
    }
    leg_h = 0.64
    body_y = 0.96
    parts = [
        Part("root", None, (0, 0, 0)),
        Part("body", "root", (0, body_y, 0), (0, body_y, 0), (0.84, 0.74, 1.46), "hide"),
        Part("udder", "body", (0, body_y, 0), (0, 0.56, -0.34), (0.34, 0.12, 0.36), "udder"),
    ]
    for tx in (-0.07, 0.07):
        for tz in (-0.26, -0.42):
            parts.append(Part(f"teat_{len(parts)}", "body", (0, body_y, 0), (tx, 0.48, tz), (0.05, 0.06, 0.05),
                              "udder"))
    parts += legs_of(leg_h, 0.25, 0.52, -0.5, 0.2, "leg_hide", hoof=("hoof", 0.1))
    neck = (0, 1.2, 0.66)
    parts += [
        Part("head", "body", neck, (0, 1.24, 0.97), (0.46, 0.44, 0.44), "white"),
        Part("neck", "head", neck, (0, 1.13, 0.68), (0.4, 0.42, 0.4), "hide"),
        Part("patch", "head", neck, (-0.232, 1.29, 0.98), (0.012, 0.3, 0.3), "black"),
        Part("poll", "head", neck, (0, 1.463, 0.9), (0.3, 0.012, 0.22), "black"),
        Part("muzzle", "head", neck, (0, 1.11, 1.24), (0.38, 0.22, 0.14), "muzzle"),
        Part("nostril_l", "head", neck, (-0.08, 1.14, 1.312), (0.06, 0.05, 0.012), "nostril"),
        Part("nostril_r", "head", neck, (0.08, 1.14, 1.312), (0.06, 0.05, 0.012), "nostril"),
        Part("eye_l", "head", neck, (-0.238, 1.32, 1.06), (0.012, 0.075, 0.075), "glint"),
        Part("pupil_l", "head", neck, (-0.243, 1.315, 1.075), (0.012, 0.055, 0.05), "eye"),
        Part("eye_r", "head", neck, (0.238, 1.32, 1.06), (0.012, 0.075, 0.075), "glint"),
        Part("pupil_r", "head", neck, (0.243, 1.315, 1.075), (0.012, 0.055, 0.05), "eye"),
        Part("ear_l", "head", (-0.23, 1.38, 0.88), (-0.31, 1.37, 0.88), (0.18, 0.07, 0.13), "black",
             rest=quat_z(12)),
        Part("ear_r", "head", (0.23, 1.38, 0.88), (0.31, 1.37, 0.88), (0.18, 0.07, 0.13), "black",
             rest=quat_z(-12)),
        Part("horn_l", "head", neck, (-0.17, 1.49, 0.9), (0.15, 0.07, 0.08), "horn"),
        Part("horn_l_tip", "head", neck, (-0.255, 1.55, 0.9), (0.065, 0.12, 0.065), "horn"),
        Part("horn_r", "head", neck, (0.17, 1.49, 0.9), (0.15, 0.07, 0.08), "horn"),
        Part("horn_r_tip", "head", neck, (0.255, 1.55, 0.9), (0.065, 0.12, 0.065), "horn"),
        Part("tail", "body", (0, 1.27, -0.73), (0, 0.99, -0.755), (0.07, 0.56, 0.07), "white"),
        Part("tuft", "tail", (0, 1.27, -0.73), (0, 0.66, -0.755), (0.12, 0.16, 0.12), "black"),
    ]
    quadruped_clips(path, parts, materials, seed=7, period=1.0, swing=24, run_period=0.55, run_swing=38,
                    width=0.84, graze=(0.5, 0.1), lift=0.42, chew=True)


def pig(path: Path) -> None:
    """A pink pig: round body, flat snout with nostrils, floppy ears, a
    curly tail."""
    skin = mottle(64, (240, 168, 166), 5, 81, spots=((232, 158, 156), 10))
    materials = {
        "skin": Material((240, 168, 166), skin, texels=40),
        "snout": Material((246, 188, 186)),
        "nostril": Material((150, 70, 74)),
        "ear": Material((230, 150, 150)),
        "hoof": Material((150, 96, 92)),
        "eye": Material((22, 18, 18)),
        "glint": Material((250, 250, 250)),
    }
    body_y = 0.53
    parts = [
        Part("root", None, (0, 0, 0)),
        Part("body", "root", (0, body_y, 0), (0, body_y, 0), (0.64, 0.56, 1.0), "skin"),
    ]
    parts += legs_of(0.28, 0.19, 0.33, -0.33, 0.17, "skin", hoof=("hoof", 0.06))
    neck = (0, 0.62, 0.45)
    parts += [
        Part("head", "body", neck, (0, 0.66, 0.68), (0.48, 0.44, 0.38), "skin"),
        Part("snout", "head", neck, (0, 0.6, 0.905), (0.26, 0.19, 0.09), "snout"),
        Part("nostril_l", "head", neck, (-0.055, 0.6, 0.952), (0.05, 0.07, 0.012), "nostril"),
        Part("nostril_r", "head", neck, (0.055, 0.6, 0.952), (0.05, 0.07, 0.012), "nostril"),
        Part("eye_l", "head", neck, (-0.14, 0.76, 0.872), (0.07, 0.07, 0.012), "glint"),
        Part("pupil_l", "head", neck, (-0.13, 0.755, 0.876), (0.045, 0.05, 0.012), "eye"),
        Part("eye_r", "head", neck, (0.14, 0.76, 0.872), (0.07, 0.07, 0.012), "glint"),
        Part("pupil_r", "head", neck, (0.13, 0.755, 0.876), (0.045, 0.05, 0.012), "eye"),
        Part("ear_l", "head", (-0.17, 0.88, 0.62), (-0.19, 0.88, 0.7), (0.15, 0.04, 0.17), "ear",
             rest=quat_mul(quat_z(20), quat_x(28))),
        Part("ear_r", "head", (0.17, 0.88, 0.62), (0.19, 0.88, 0.7), (0.15, 0.04, 0.17), "ear",
             rest=quat_mul(quat_z(-20), quat_x(28))),
        Part("tail", "body", (0, 0.7, -0.5), (0, 0.72, -0.56), (0.05, 0.05, 0.12), "ear"),
        Part("tail_curl", "tail", (0, 0.7, -0.5), (0, 0.77, -0.61), (0.05, 0.08, 0.05), "ear"),
    ]
    quadruped_clips(path, parts, materials, seed=8, period=0.7, swing=30, run_period=0.4, run_swing=42,
                    width=0.64, graze=(0.1, 0.04), lift=0.32, chew=False)


def sheep(path: Path) -> None:
    """A woolly sheep: a fluffy cream fleece, dark face and legs."""
    wool = fleece(96, (232, 226, 210), 91)
    materials = {
        "wool": Material((232, 226, 210), wool, texels=64),
        "face": Material((66, 60, 58)),
        "legs": Material((58, 54, 52)),
        "nose": Material((40, 34, 34)),
        "eye": Material((228, 196, 120)),
        "pupil": Material((20, 18, 16)),
    }
    body_y = 0.82
    parts = [
        Part("root", None, (0, 0, 0)),
        Part("body", "root", (0, body_y, 0), (0, body_y, 0), (0.86, 0.72, 1.2), "wool"),
    ]
    parts += legs_of(0.5, 0.24, 0.38, -0.38, 0.14, "legs")
    for leg in LEGS:
        hip = next(p for p in parts if p.name == leg).pivot
        parts.append(Part(leg + "_wool", leg, hip, (hip[0], 0.42, hip[2]), (0.22, 0.18, 0.22), "wool"))
    neck = (0, 1.0, 0.55)
    parts += [
        Part("head", "body", neck, (0, 1.03, 0.79), (0.34, 0.38, 0.4), "face"),
        Part("neck", "head", neck, (0, 0.96, 0.56), (0.36, 0.36, 0.34), "wool"),
        Part("cap", "head", neck, (0, 1.235, 0.73), (0.42, 0.13, 0.34), "wool"),
        Part("nose", "head", neck, (0, 0.93, 0.992), (0.13, 0.07, 0.012), "nose"),
        Part("eye_l", "head", neck, (-0.172, 1.09, 0.88), (0.012, 0.06, 0.07), "eye"),
        Part("pupil_l", "head", neck, (-0.177, 1.09, 0.885), (0.012, 0.025, 0.05), "pupil"),
        Part("eye_r", "head", neck, (0.172, 1.09, 0.88), (0.012, 0.06, 0.07), "eye"),
        Part("pupil_r", "head", neck, (0.177, 1.09, 0.885), (0.012, 0.025, 0.05), "pupil"),
        Part("ear_l", "head", (-0.17, 1.12, 0.72), (-0.26, 1.12, 0.72), (0.18, 0.06, 0.11), "face",
             rest=quat_z(18)),
        Part("ear_r", "head", (0.17, 1.12, 0.72), (0.26, 1.12, 0.72), (0.18, 0.06, 0.11), "face",
             rest=quat_z(-18)),
        Part("tail", "body", (0, 1.0, -0.6), (0, 0.93, -0.64), (0.16, 0.18, 0.1), "wool"),
    ]
    quadruped_clips(path, parts, materials, seed=9, period=0.9, swing=26, run_period=0.5, run_swing=40,
                    width=0.86, graze=(0.42, 0.06), lift=0.43, chew=True)


def quadruped_clips(path, parts, materials, seed, period, swing, run_period, run_swing, width, graze, lift, chew):
    """idle (grazing), walk, run, hurt and death for a four-legged animal."""
    has = {p.name for p in parts}
    # Walk: diagonal pairs together, a slight bob and nod.
    walk = merge(gait(LEGS, period, swing, {"leg_fl": 0.0, "leg_br": 0.0, "leg_fr": 0.5, "leg_bl": 0.5}),
                 {"head": {"r": wave(5, period, 0.25)}, "body": {"t": bob(0.025, period / 2)}})
    if "tail" in has:
        walk["tail"] = {"r": wave(10, period, 0.1, quat_z)}
    # Run: a gallop, front pair then back pair, the body rocking.
    run = merge(gait(LEGS, run_period, run_swing, {"leg_fl": 0.0, "leg_fr": 0.12, "leg_bl": 0.5, "leg_br": 0.62}),
                {"head": {"r": wave(9, run_period, 0.3)},
                 "body": {"r": wave(5, run_period, 0.0), "t": bob(0.06, run_period, 0.25)}})
    if "tail" in has:
        run["tail"] = {"r": keys(quat_x, [(0.0, -35), (run_period, -35)])}
    # Idle: a nod, then the head goes down to the grass and chews, comes back
    # up and looks to one side; the tail swishes and the ears flick.
    idle_len = 8.0
    reach_down, reach_fore = graze
    # (time, head pitch down, head turn, how far down the neck stretches)
    pose = [(0.0, 0, 0, 0), (1.2, 0, 0, 0), (1.5, 6, 0, 0), (2.0, 0, 0, 0), (2.5, 62, 0, 1), (5.4, 62, 0, 1),
            (6.0, 0, 0, 0), (6.5, 0, -18, 0), (7.3, 0, -18, 0), (7.7, 0, 0, 0), (idle_len, 0, 0, 0)]
    idle = {"head": {"r": [(t, quat_mul(quat_y(turn), quat_x(pitch))) for t, pitch, turn, _ in pose],
                     "t": [(t, (0, -reach_down * s, reach_fore * s)) for t, _, _, s in pose]},
            "body": {"r": [(t, quat_x(5 * s)) for t, _, _, s in pose]}}
    if chew:
        chew_keys = [(0.0, (0, 0, 0)), (2.6, (0, 0, 0))]
        for k in range(10):
            t = 2.7 + k * 0.27
            chew_keys += [(t, (0, -0.02, 0.01)), (t + 0.135, (0, 0, 0))]
        chew_keys.append((idle_len, (0, 0, 0)))
        idle["muzzle" if "muzzle" in has else "nose"] = {"t": chew_keys}
    if "tail" in has:
        idle["tail"] = {"r": wave(16, 2.0, 0.0, quat_z, 8) + [(2.0 + t, q) for t, q in wave(16, 2.0, 0.0, quat_z, 8)[1:]]
                        + [(4.0 + t, q) for t, q in wave(6, 4.0, 0.0, quat_z, 8)[1:]]}
    for ear, sign in (("ear_l", 1), ("ear_r", -1)):
        if ear in has:
            idle[ear] = {"r": keys(quat_z, [(0.0, 0), (3.0, 0), (3.1, sign * 25), (3.25, 0), (7.0, 0), (7.1, sign * 20),
                                            (7.2, 0), (idle_len, 0)])}
    hurt = {"body": {"r": keys(quat_x, [(0.0, 0), (0.08, -10), (0.35, 0)]),
                     "t": [(0.0, (0, 0, 0)), (0.08, (0, 0.06, 0)), (0.35, (0, 0, 0))]},
            "head": {"r": keys(quat_x, [(0.0, 0), (0.08, -25), (0.35, 0)])}}
    death = death_clip(width, lift, legs=LEGS)
    write_figure(path, parts, materials, {"idle": (idle_len, idle), "walk": (period, walk), "run": (run_period, run),
                                          "hurt": (0.35, hurt), "death": (0.9, death)}, seed=seed)


def chicken(path: Path) -> None:
    """A white hen: red comb and wattle, yellow beak, orange legs, wings that
    flap when she runs."""
    plumage = feathers(64, (244, 242, 236), 101)
    materials = {
        "feathers": Material((244, 242, 236), plumage, texels=90),
        "wing": Material((226, 224, 218), plumage, texels=90),
        "beak": Material((242, 176, 44)),
        "comb": Material((214, 40, 36)),
        "legs": Material((236, 150, 40)),
        "eye": Material((20, 16, 14)),
    }
    parts = [
        Part("root", None, (0, 0, 0)),
        Part("body", "root", (0, 0.38, 0), (0, 0.38, -0.02), (0.34, 0.3, 0.44), "feathers"),
        Part("leg_l", "body", (-0.07, 0.24, 0.0), (-0.07, 0.12, 0.0), (0.045, 0.24, 0.045), "legs"),
        Part("foot_l", "leg_l", (-0.07, 0.24, 0.0), (-0.07, 0.012, 0.03), (0.11, 0.024, 0.13), "legs"),
        Part("leg_r", "body", (0.07, 0.24, 0.0), (0.07, 0.12, 0.0), (0.045, 0.24, 0.045), "legs"),
        Part("foot_r", "leg_r", (0.07, 0.24, 0.0), (0.07, 0.012, 0.03), (0.11, 0.024, 0.13), "legs"),
        Part("head", "body", (0, 0.5, 0.14), (0, 0.63, 0.19), (0.19, 0.23, 0.17), "feathers"),
        Part("beak", "head", (0, 0.5, 0.14), (0, 0.62, 0.325), (0.085, 0.06, 0.1), "beak"),
        Part("comb", "head", (0, 0.5, 0.14), (0, 0.775, 0.2), (0.045, 0.09, 0.15), "comb"),
        Part("wattle", "head", (0, 0.5, 0.14), (0, 0.545, 0.285), (0.055, 0.08, 0.04), "comb"),
        Part("eye_l", "head", (0, 0.5, 0.14), (-0.098, 0.665, 0.23), (0.012, 0.04, 0.04), "eye"),
        Part("eye_r", "head", (0, 0.5, 0.14), (0.098, 0.665, 0.23), (0.012, 0.04, 0.04), "eye"),
        Part("wing_l", "body", (-0.17, 0.48, 0.02), (-0.185, 0.39, -0.04), (0.04, 0.2, 0.3), "wing"),
        Part("wing_r", "body", (0.17, 0.48, 0.02), (0.185, 0.39, -0.04), (0.04, 0.2, 0.3), "wing"),
        Part("tail", "body", (0, 0.44, -0.22), (0, 0.52, -0.28), (0.2, 0.2, 0.08), "feathers", rest=quat_x(-28)),
    ]
    period = 0.5
    walk = {"leg_l": {"r": wave(32, period)}, "leg_r": {"r": wave(32, period, 0.5)},
            # The head bobs forward and back with each step.
            "head": {"t": [(period * k / 4, (0, 0, 0.04 * (1 if k % 2 else 0))) for k in range(5)]},
            "body": {"t": bob(0.015, period / 2)}}
    run_period = 0.3
    # Running flat out, wings beating (outward is -Z turn on the left).
    run = {"leg_l": {"r": wave(48, run_period)}, "leg_r": {"r": wave(48, run_period, 0.5)},
           "body": {"r": keys(quat_x, [(0.0, 14), (run_period, 14)]), "t": bob(0.03, run_period / 2)},
           "wing_l": {"r": flaps(-60, run_period, 2)}, "wing_r": {"r": flaps(60, run_period, 2)}}
    # Idle: peck at the ground twice, look about, a ruffle of the wings.
    idle = {"head": {"r": keys(quat_x, [(0.0, 0), (0.6, 0), (0.75, 70), (0.9, 0), (1.3, 0), (1.45, 72), (1.6, 0),
                                        (2.4, 0), (2.6, 0), (4.0, 0)])},
            "body": {"r": keys(quat_x, [(0.0, 0), (0.6, 0), (0.75, 18), (0.9, 0), (1.3, 0), (1.45, 18), (1.6, 0),
                                        (4.0, 0)])},
            "wing_l": {"r": keys(quat_z, [(0.0, 0), (3.0, 0), (3.1, -30), (3.2, 0), (3.3, -30), (3.4, 0), (4.0, 0)])},
            "wing_r": {"r": keys(quat_z, [(0.0, 0), (3.0, 0), (3.1, 30), (3.2, 0), (3.3, 30), (3.4, 0), (4.0, 0)])}}
    idle["head"]["r"] = [(t, quat_mul(quat_y(25 if 2.0 <= t <= 2.6 else 0), q)) for t, q in idle["head"]["r"]]
    hurt = {"body": {"t": [(0.0, (0, 0, 0)), (0.1, (0, 0.12, 0)), (0.3, (0, 0, 0))]},
            "wing_l": {"r": keys(quat_z, [(0.0, 0), (0.1, -70), (0.3, 0)])},
            "wing_r": {"r": keys(quat_z, [(0.0, 0), (0.1, 70), (0.3, 0)])}}
    death = death_clip(0.34, 0.17, duration=0.6, legs=("leg_l", "leg_r"), leg_kick=30)
    write_figure(path, parts, materials, {"idle": (4.0, idle), "walk": (period, walk), "run": (run_period, run),
                                          "hurt": (0.3, hurt), "death": (0.6, death)}, seed=10)


# ---------------------------------------------------------------------------
# icons (64x64, transparent; drawn chunky so they read at slot size)
# ---------------------------------------------------------------------------

OUTLINE = (44, 28, 22, 255)


def icon_apple() -> Image.Image:
    image, draw = icon_canvas()
    draw.ellipse((10, 16, 36, 56), fill=(200, 34, 38, 255), outline=(96, 16, 18, 255), width=3)
    draw.ellipse((28, 16, 54, 56), fill=(200, 34, 38, 255), outline=(96, 16, 18, 255), width=3)
    draw.rectangle((24, 20, 40, 52), fill=(200, 34, 38, 255))
    draw.ellipse((16, 22, 26, 34), fill=(250, 140, 130, 255))
    draw.line([(32, 20), (34, 6)], fill=(96, 62, 30, 255), width=4)
    draw.polygon([(35, 10), (50, 4), (46, 16)], fill=(86, 160, 58, 255), outline=(40, 90, 30, 255))
    return image


def steak(cooked: bool) -> Image.Image:
    image, draw = icon_canvas()
    flesh = (140, 76, 40, 255) if cooked else (204, 52, 58, 255)
    edge = (70, 36, 18, 255) if cooked else (110, 22, 30, 255)
    fat = (220, 186, 140, 255) if cooked else (246, 226, 214, 255)
    draw.polygon([(8, 26), (20, 12), (42, 10), (56, 22), (54, 44), (38, 56), (16, 52), (6, 40)], fill=fat,
                 outline=edge)
    draw.polygon([(13, 27), (22, 16), (41, 15), (51, 24), (49, 42), (37, 51), (18, 48), (11, 39)], fill=flesh)
    if cooked:
        for x in (16, 26, 36):
            draw.line([(x, 20), (x + 14, 46)], fill=(64, 30, 14, 255), width=3)
    else:
        for a, b in (((20, 30), (34, 24)), ((24, 40), (44, 34)), ((30, 20), (42, 22))):
            draw.line([a, b], fill=(240, 196, 196, 255), width=2)
    return image


def porkchop(cooked: bool) -> Image.Image:
    image, draw = icon_canvas()
    flesh = (196, 132, 82, 255) if cooked else (244, 166, 164, 255)
    edge = (110, 66, 34, 255) if cooked else (150, 80, 84, 255)
    rind = (230, 196, 150, 255) if cooked else (252, 222, 212, 255)
    draw.ellipse((6, 10, 50, 50), fill=rind, outline=edge, width=3)
    draw.ellipse((12, 16, 44, 44), fill=flesh)
    draw.polygon([(40, 38), (58, 52), (54, 58), (36, 46)], fill=(238, 230, 214, 255), outline=(150, 140, 120, 255))
    draw.ellipse((52, 48, 62, 60), fill=(238, 230, 214, 255), outline=(150, 140, 120, 255), width=2)
    if cooked:
        for x in (16, 26):
            draw.line([(x, 20), (x + 12, 40)], fill=(120, 70, 36, 255), width=3)
    return image


def drumstick(cooked: bool) -> Image.Image:
    image, draw = icon_canvas()
    flesh = (206, 140, 62, 255) if cooked else (244, 196, 180, 255)
    edge = (118, 70, 26, 255) if cooked else (168, 112, 100, 255)
    draw.line([(36, 34), (54, 52)], fill=(236, 228, 210, 255), width=8)
    draw.ellipse((48, 44, 60, 56), fill=(236, 228, 210, 255), outline=(150, 140, 120, 255), width=2)
    draw.ellipse((54, 50, 63, 60), fill=(236, 228, 210, 255), outline=(150, 140, 120, 255), width=2)
    draw.ellipse((6, 6, 44, 42), fill=flesh, outline=edge, width=3)
    draw.ellipse((13, 12, 25, 22), fill=(236, 186, 120, 255) if cooked else (252, 222, 214, 255))
    return image


def mutton(cooked: bool) -> Image.Image:
    image, draw = icon_canvas()
    flesh = (120, 66, 40, 255) if cooked else (168, 40, 50, 255)
    edge = (60, 30, 14, 255) if cooked else (90, 18, 24, 255)
    draw.polygon([(8, 20), (24, 8), (44, 14), (50, 34), (36, 50), (16, 46)], fill=flesh, outline=edge)
    draw.arc((12, 14, 44, 46), 200, 320, fill=(232, 210, 190, 255), width=4)
    draw.line([(42, 42), (56, 56)], fill=(236, 228, 210, 255), width=6)
    draw.ellipse((50, 50, 62, 62), fill=(236, 228, 210, 255), outline=(150, 140, 120, 255), width=2)
    return image


def icon_bucket(content=None) -> Image.Image:
    image, draw = icon_canvas()
    draw.arc((14, 2, 50, 34), 180, 360, fill=(90, 92, 98, 255), width=3)
    draw.polygon([(10, 20), (54, 20), (48, 58), (16, 58)], fill=(150, 154, 162, 255), outline=(62, 64, 70, 255))
    draw.line([(12, 30), (52, 30)], fill=(110, 112, 120, 255), width=2)
    draw.line([(14, 46), (50, 46)], fill=(110, 112, 120, 255), width=2)
    draw.ellipse((10, 14, 54, 26), fill=(70, 72, 78, 255), outline=(62, 64, 70, 255), width=2)
    if content is not None:
        light, dark = content
        draw.ellipse((13, 16, 51, 25), fill=dark)
        draw.ellipse((18, 17, 36, 21), fill=light)
    draw.line([(18, 24), (20, 54)], fill=(196, 200, 206, 255), width=3)
    return image


def icon_bottle(water: bool) -> Image.Image:
    image, draw = icon_canvas()
    glass = (190, 222, 232, 150)
    outline = (70, 110, 126, 255)
    draw.rounded_rectangle((14, 24, 50, 60), radius=10, fill=glass, outline=outline, width=3)
    draw.rectangle((25, 8, 39, 26), fill=glass, outline=outline, width=3)
    draw.rectangle((23, 4, 41, 10), fill=(150, 104, 62, 255), outline=(80, 52, 30, 255), width=2)
    if water:
        draw.rounded_rectangle((17, 34, 47, 57), radius=8, fill=(52, 128, 214, 230))
        draw.line([(18, 35), (46, 35)], fill=(150, 200, 245, 255), width=2)
    draw.line([(20, 30), (20, 52)], fill=(245, 252, 255, 220), width=3)
    return image


def icon_waterskin(full: bool) -> Image.Image:
    image, draw = icon_canvas()
    body = (150, 98, 56, 255) if full else (132, 92, 60, 255)
    if full:
        draw.ellipse((8, 18, 56, 60), fill=body, outline=(70, 42, 20, 255), width=3)
    else:
        draw.polygon([(12, 30), (30, 22), (52, 30), (54, 54), (32, 60), (10, 52)], fill=body,
                     outline=(70, 42, 20, 255))
    draw.rectangle((26, 6, 38, 22), fill=(110, 70, 38, 255), outline=(60, 36, 18, 255), width=2)
    draw.line([(10, 16), (54, 16)], fill=(96, 60, 30, 255), width=3)
    if full:
        draw.ellipse((18, 26, 30, 36), fill=(190, 140, 96, 255))
    for x in (20, 32, 44):
        draw.point((x, 46), fill=(70, 42, 20, 255))
    return image


def icon_leather() -> Image.Image:
    image, draw = icon_canvas()
    draw.polygon([(10, 14), (24, 8), (40, 10), (56, 18), (52, 36), (56, 52), (38, 58), (20, 54), (8, 44), (12, 30)],
                 fill=(160, 96, 50, 255), outline=(80, 44, 20, 255))
    for x, y in ((22, 22), (36, 30), (26, 42), (42, 46)):
        draw.ellipse((x - 3, y - 2, x + 3, y + 2), fill=(134, 78, 40, 255))
    return image


def icon_feather() -> Image.Image:
    image, draw = icon_canvas()
    draw.polygon([(12, 56), (22, 34), (40, 12), (54, 6), (50, 22), (30, 44)], fill=(246, 244, 238, 255),
                 outline=(150, 146, 140, 255))
    draw.line([(10, 58), (50, 10)], fill=(150, 146, 140, 255), width=2)
    for k in range(6):
        x = 20 + k * 5
        draw.line([(x, 48 - k * 6), (x + 8, 44 - k * 6)], fill=(206, 204, 198, 255), width=1)
    return image


def icon_wool() -> Image.Image:
    image, draw = icon_canvas()
    for cx, cy, r in ((22, 30, 13), (38, 26, 14), (30, 40, 15), (44, 40, 12), (18, 42, 11)):
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(240, 234, 220, 255), outline=(170, 160, 140, 255),
                     width=2)
    for cx, cy in ((24, 28), (38, 24), (32, 40), (44, 40)):
        draw.arc((cx - 5, cy - 5, cx + 5, cy + 5), 200, 340, fill=(200, 192, 176, 255), width=2)
    return image


def icon_bowl(stew: bool) -> Image.Image:
    image, draw = icon_canvas()
    if stew:
        draw.ellipse((8, 22, 56, 38), fill=(150, 82, 40, 255))
        for x, y, c in ((20, 28, (196, 120, 60)), (34, 26, (120, 60, 30)), (44, 30, (200, 60, 50)),
                        (28, 32, (230, 170, 80))):
            draw.ellipse((x - 4, y - 3, x + 4, y + 3), fill=(*c, 255))
        for x in (22, 32, 42):
            draw.arc((x - 4, 4, x + 4, 18), 90, 270, fill=(230, 230, 230, 160), width=2)
    draw.chord((6, 14, 58, 58), 0, 180, fill=(150, 98, 52, 255), outline=(76, 46, 22, 255), width=3)
    draw.ellipse((6, 24, 58, 40), outline=(76, 46, 22, 255), width=3, fill=None if stew else (110, 70, 36, 255))
    draw.line([(14, 46), (50, 46)], fill=(120, 76, 40, 255), width=2)
    return image


def icon_oven() -> Image.Image:
    image, draw = icon_canvas()
    draw.rectangle((6, 10, 58, 58), fill=(120, 116, 112, 255), outline=(52, 50, 48, 255), width=3)
    for y in (22, 34, 46):
        draw.line([(8, y), (56, y)], fill=(88, 84, 80, 255), width=2)
    draw.rounded_rectangle((18, 30, 46, 56), radius=6, fill=(30, 22, 18, 255))
    draw.polygon([(22, 56), (28, 40), (32, 48), (36, 36), (42, 56)], fill=(250, 150, 40, 255))
    draw.polygon([(27, 56), (31, 46), (35, 56)], fill=(255, 226, 120, 255))
    return image


def icon_cube(face: Image.Image) -> Image.Image:
    """A block's inventory icon: three faces of a cube, the sides shaded."""
    image = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    tile = face.convert("RGBA").resize((32, 32), Image.NEAREST)

    def shaded(amount):
        out = tile.copy()
        pixels = out.load()
        for y in range(32):
            for x in range(32):
                r, g, b, a = pixels[x, y]
                pixels[x, y] = (clamp(r * amount), clamp(g * amount), clamp(b * amount), a)
        return out

    # Top: a rhombus; sides: parallelograms. Each is the square tile under an
    # affine map (PIL wants output -> input, so these are the inverses).
    k, h = 32 / 28, 16 / 28
    top = tile.transform((64, 64), Image.AFFINE, (h, -k, -4 * h + 18 * k, h, k, -4 * h - 18 * k), Image.NEAREST)
    left = shaded(0.8).transform((64, 64), Image.AFFINE, (k, 0, -4 * k, -h, k, 4 * h - 18 * k), Image.NEAREST)
    right = shaded(0.62).transform((64, 64), Image.AFFINE, (k, 0, -32 * k, h, k, -32 * h - 32 * k), Image.NEAREST)
    for layer in (left, right, top):
        image.alpha_composite(layer)
    return image


# HUD icons: glossy like the game's own hearts (an outline, a body, a soft
# highlight), drawn at 4x and scaled down for smooth edges. Each comes full,
# half (the left half) and empty (a dark ghost of the shape).
HUD_SIZE = 32
HUD_SCALE = 4


def hud_shape(kind: str, draw: ImageDraw.ImageDraw, k: float, grow: float, fill) -> None:
    """The silhouette of a HUD icon, `grow` pixels (at 32 px) larger all round."""
    g = grow * k
    if kind == "hunger":
        # A drumstick: the meat top left, the bone down to the bottom right.
        draw.line([(15 * k, 15 * k), (25.5 * k, 25.5 * k)], fill=fill, width=int((5 + 2 * grow) * k))
        for cx, cy in ((26.5, 23.5), (23.5, 26.5)):
            draw.ellipse(((cx - 3) * k - g, (cy - 3) * k - g, (cx + 3) * k + g, (cy + 3) * k + g), fill=fill)
        draw.ellipse((3 * k - g, 3 * k - g, 21 * k + g, 21 * k + g), fill=fill)
        draw.ellipse((9 * k - g, 9 * k - g, 22 * k + g, 22 * k + g), fill=fill)
    else:
        # A water drop: a point at the top, round at the bottom.
        draw.polygon([(16 * k, 2 * k - 1.6 * g), (6.2 * k - g, 18.5 * k), (25.8 * k + g, 18.5 * k)], fill=fill)
        draw.ellipse((6 * k - g, 10.5 * k - g, 26 * k + g, 30.5 * k + g), fill=fill)


def hud_icon(kind: str, fill: str) -> Image.Image:
    k = HUD_SCALE
    size = HUD_SIZE * k
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    outline = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    hud_shape(kind, ImageDraw.Draw(outline), k, 1.6, (24, 14, 10, 235) if kind == "hunger" else (10, 18, 40, 235))
    out.alpha_composite(outline)
    body = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(body)
    if fill == "empty":
        hud_shape(kind, draw, k, 0, (40, 34, 32, 150) if kind == "hunger" else (28, 34, 50, 150))
        out.alpha_composite(body)
        return out.resize((HUD_SIZE, HUD_SIZE), Image.LANCZOS)
    if kind == "hunger":
        hud_shape(kind, draw, k, 0, (236, 228, 210, 255))  # the bone
        draw.ellipse((3 * k, 3 * k, 21 * k, 21 * k), fill=(198, 104, 40, 255))
        draw.ellipse((9 * k, 9 * k, 22 * k, 22 * k), fill=(198, 104, 40, 255))
        draw.ellipse((4.5 * k, 4.5 * k, 19 * k, 18 * k), fill=(222, 132, 58, 255))
        draw.ellipse((6.5 * k, 6 * k, 12.5 * k, 11 * k), fill=(255, 214, 160, 255))
    else:
        hud_shape(kind, draw, k, 0, (40, 128, 226, 255))
        draw.ellipse((9 * k, 13 * k, 24 * k, 28.5 * k), fill=(70, 160, 246, 255))
        draw.ellipse((10.5 * k, 16 * k, 15.5 * k, 23 * k), fill=(214, 238, 255, 255))
    if fill == "half":
        # The right half is the empty ghost.
        ghost = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        hud_shape(kind, ImageDraw.Draw(ghost), k, 0, (40, 34, 32, 150) if kind == "hunger" else (28, 34, 50, 150))
        body.paste(ghost.crop((size // 2, 0, size, size)), (size // 2, 0))
    out.alpha_composite(body)
    return out.resize((HUD_SIZE, HUD_SIZE), Image.LANCZOS)


def oven_side() -> Image.Image:
    """The stone oven's sides: cut stone around a glowing fire mouth."""
    rng = random.Random(121)
    size = 32
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            d = rng.randint(-9, 9)
            pixels[x, y] = (clamp(124 + d), clamp(120 + d), clamp(116 + d), 255)
    draw = ImageDraw.Draw(image)
    for row in range(0, size, 8):
        draw.line([(0, row), (size - 1, row)], fill=(74, 70, 68, 255))
        offset = 0 if (row // 8) % 2 == 0 else 8
        for x in range(offset, size, 16):
            draw.line([(x, row), (x, row + 7)], fill=(74, 70, 68, 255))
    draw.rectangle((8, 14, 23, 31), fill=(26, 18, 14, 255))
    draw.rectangle((7, 13, 24, 14), fill=(60, 56, 52, 255))
    for x, h, c in ((10, 8, (250, 140, 30)), (13, 12, (255, 180, 50)), (16, 9, (250, 120, 30)),
                    (19, 11, (255, 196, 70)), (21, 6, (240, 110, 30))):
        draw.line([(x, 31), (x, 31 - h)], fill=(*c, 255), width=2)
    draw.line([(9, 30), (22, 30)], fill=(255, 236, 160, 255), width=2)
    return image


def oven_top() -> Image.Image:
    rng = random.Random(122)
    size = 32
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            d = rng.randint(-8, 8)
            pixels[x, y] = (clamp(112 + d), clamp(108 + d), clamp(104 + d), 255)
    draw = ImageDraw.Draw(image)
    draw.rectangle((1, 1, 30, 30), outline=(74, 70, 68, 255))
    draw.rectangle((9, 9, 22, 22), fill=(40, 34, 30, 255), outline=(64, 60, 56, 255))
    draw.rectangle((11, 11, 20, 20), fill=(70, 46, 30, 255))
    return image


# ---------------------------------------------------------------------------
# sounds
# ---------------------------------------------------------------------------

RATE = 22050


def normalized(samples: list[float], peak: float = 0.85) -> list[float]:
    """The samples scaled so their loudest is `peak`."""
    top = max(1e-6, max(abs(v) for v in samples))
    return [v * peak / top for v in samples]


def formant_tone(seconds, pitch, formants, rate=RATE, vibrato=(0.0, 0.0), breath=0.0, seed=0, envelope=None):
    """A voiced call: harmonics of a gliding pitch, each weighted by the
    formants (centre, width, gain) it falls near, plus a little breath."""
    rng = random.Random(seed)
    out = []
    phase = 0.0
    for i in range(int(rate * seconds)):
        t = i / rate
        f0 = pitch(t) * (1.0 + vibrato[0] * math.sin(2 * math.pi * vibrato[1] * t))
        phase += 2 * math.pi * f0 / rate
        sample = 0.0
        for h in range(1, 40):
            f = f0 * h
            if f > rate / 2 - 500:
                break
            weight = sum(g * math.exp(-((f - c) / w) ** 2) for c, w, g in formants(t)) / h ** 0.3
            sample += weight * math.sin(h * phase)
        sample += breath * rng.uniform(-1, 1)
        env = envelope(t) if envelope else 1.0
        out.append(sample * env)
    peak = max(1e-6, max(abs(s) for s in out))
    return [0.7 * s / peak for s in out]


def moo() -> list[float]:
    seconds = 1.35

    def pitch(t):
        return 108 + 34 * math.sin(math.pi * min(1.0, t / 0.9)) - 14 * max(0.0, t - 0.9)

    def formants(t):
        opening = min(1.0, max(0.0, (t - 0.12) / 0.35))  # "mm" opening to "oo"
        return [(220, 90, 1.0), (330 + 250 * opening, 140, 0.4 + 0.6 * opening), (2400, 400, 0.04)]

    def envelope(t):
        return min(1.0, t / 0.12) * min(1.0, (seconds - t) / 0.3)

    return formant_tone(seconds, pitch, formants, vibrato=(0.012, 5.0), breath=0.02, seed=1, envelope=envelope)


def oink() -> list[float]:
    out = []
    for grunt, start in enumerate((0.0, 0.17)):
        length = 0.13 if grunt == 0 else 0.16
        part = formant_tone(length, lambda t: 150 - 120 * t, lambda t: [(420, 160, 1.0), (1100, 300, 0.5)],
                            breath=0.25, seed=2 + grunt,
                            envelope=lambda t, n=length: math.sin(math.pi * min(1.0, t / n)) ** 0.6)
        out += [0.0] * (int(RATE * start) - len(out)) + part
    return out


def cluck() -> list[float]:
    out = []
    for k, (start, pitch0) in enumerate(((0.0, 520), (0.12, 500), (0.24, 760))):
        length = 0.07 if k < 2 else 0.18
        part = formant_tone(length, lambda t, p=pitch0: p + 400 * t, lambda t: [(1100, 260, 1.0), (2300, 420, 0.5)],
                            breath=0.15, seed=10 + k,
                            envelope=lambda t, n=length: math.sin(math.pi * min(1.0, t / n)))
        out += [0.0] * (int(RATE * start) - len(out)) + part
    return out


def baa() -> list[float]:
    seconds = 0.9

    def envelope(t):
        return min(1.0, t / 0.06) * min(1.0, (seconds - t) / 0.25)

    return formant_tone(seconds, lambda t: 290 - 30 * t, lambda t: [(780, 180, 1.0), (1250, 220, 0.6), (2600, 400, 0.1)],
                        vibrato=(0.05, 9.0), breath=0.04, seed=4, envelope=envelope)


def munch() -> list[float]:
    """Three crunchy bites."""
    rng = random.Random(5)
    out = []
    lowpass = 0.0
    for i in range(int(RATE * 0.62)):
        t = i / RATE
        bite = (t % 0.2) / 0.2
        env = math.exp(-bite * 9) * (1.0 if t < 0.58 else 0.0)
        noise = rng.uniform(-1, 1)
        lowpass += 0.35 * (noise - lowpass)
        out.append(0.6 * env * (0.6 * lowpass + 0.4 * noise * (1 if rng.random() < 0.4 else 0.3)))
    return out


def gulps() -> list[float]:
    one = gulp_sound()
    return one + [0.0] * int(RATE * 0.08) + [s * 0.8 for s in one]


def splash_fill() -> list[float]:
    """Water poured into a vessel: bubbly noise rising in pitch."""
    rng = random.Random(6)
    out = []
    band = 0.0
    prev = 0.0
    seconds = 0.8
    for i in range(int(RATE * seconds)):
        t = i / RATE
        noise = rng.uniform(-1, 1)
        band += 0.25 * (noise - band)
        high = band - prev
        prev = band
        bubble = math.sin(2 * math.pi * (300 + 500 * t) * t) * (0.5 + 0.5 * math.sin(2 * math.pi * 23 * t))
        env = min(1.0, t / 0.05) * min(1.0, (seconds - t) / 0.2)
        out.append(env * (0.45 * high * 3 + 0.18 * bubble))
    return out


def squirt() -> list[float]:
    """Milking: short hissing squirts into a bucket."""
    rng = random.Random(7)
    out = []
    prev = 0.0
    for i in range(int(RATE * 0.7)):
        t = i / RATE
        phase = (t % 0.23) / 0.23
        env = math.exp(-phase * 7) * (1 if t < 0.66 else 0)
        noise = rng.uniform(-1, 1)
        high = noise - prev
        prev = noise
        out.append(0.4 * env * (high * 0.6 + 0.3 * math.sin(2 * math.pi * 900 * t)))
    return out


def sizzle() -> list[float]:
    """Meat on a hot stone: crackling hiss."""
    rng = random.Random(8)
    out = []
    prev = 0.0
    seconds = 1.0
    for i in range(int(RATE * seconds)):
        t = i / RATE
        noise = rng.uniform(-1, 1)
        high = noise - prev
        prev = noise
        crackle = (1.0 if rng.random() < 0.004 else 0.0) * rng.uniform(0.5, 1.0)
        env = min(1.0, t / 0.04) * min(1.0, (seconds - t) / 0.3)
        out.append(env * (0.22 * high + 0.6 * crackle))
    return out


# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2] / "examples" / "mods"))
    root = Path(parser.parse_args().root) / "wilderness" / "assets"

    models = root / "models"
    cow(models / "cow.gltf")
    pig(models / "pig.gltf")
    sheep(models / "sheep.gltf")
    chicken(models / "chicken.gltf")

    icons = root / "textures" / "icons"
    save_png(icon_apple(), icons / "apple.png")
    save_png(icon_berries(), icons / "berries.png")
    save_png(steak(False), icons / "raw_beef.png")
    save_png(steak(True), icons / "steak.png")
    save_png(porkchop(False), icons / "raw_pork.png")
    save_png(porkchop(True), icons / "cooked_pork.png")
    save_png(drumstick(False), icons / "raw_chicken.png")
    save_png(drumstick(True), icons / "cooked_chicken.png")
    save_png(mutton(False), icons / "raw_mutton.png")
    save_png(mutton(True), icons / "cooked_mutton.png")
    save_png(icon_bowl(False), icons / "bowl.png")
    save_png(icon_bowl(True), icons / "stew.png")
    save_png(icon_bucket(), icons / "bucket.png")
    save_png(icon_bucket(((150, 200, 245, 255), (46, 120, 210, 255))), icons / "water_bucket.png")
    save_png(icon_bucket(((255, 255, 255, 255), (236, 232, 222, 255))), icons / "milk_bucket.png")
    save_png(icon_bottle(False), icons / "bottle.png")
    save_png(icon_bottle(True), icons / "water_bottle.png")
    save_png(icon_waterskin(False), icons / "waterskin.png")
    save_png(icon_waterskin(True), icons / "waterskin_full.png")
    save_png(icon_leather(), icons / "leather.png")
    save_png(icon_feather(), icons / "feather.png")
    save_png(icon_wool(), icons / "wool.png")
    save_png(icon_oven(), icons / "stone_oven.png")

    hud = root / "ui"
    for kind in ("hunger", "thirst"):
        save_png(hud_icon(kind, "full"), hud / f"{kind}.png")
        save_png(hud_icon(kind, "half"), hud / f"{kind}_half.png")
        save_png(hud_icon(kind, "empty"), hud / f"{kind}_empty.png")

    wool_tile = fleece(32, (236, 232, 220), 131)
    save_png(wool_tile, root / "textures" / "wool_block.png")
    save_png(icon_cube(wool_tile), icons / "wool_block.png")
    save_png(oven_side(), root / "textures" / "stone_oven_side.png")
    save_png(oven_top(), root / "textures" / "stone_oven_top.png")

    sounds = root / "sounds"
    write_wav(sounds / "moo.wav", moo())
    write_wav(sounds / "oink.wav", oink())
    write_wav(sounds / "cluck.wav", cluck())
    write_wav(sounds / "baa.wav", baa())
    write_wav(sounds / "eat.wav", normalized(munch()))
    write_wav(sounds / "drink.wav", gulps())
    write_wav(sounds / "fill.wav", splash_fill())
    write_wav(sounds / "milk.wav", normalized(squirt(), 0.8))
    write_wav(sounds / "sizzle.wav", sizzle())
    print(f"wilderness survival assets written under {root}")


if __name__ == "__main__":
    main()
