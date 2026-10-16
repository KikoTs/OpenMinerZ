"""Draws the weapons of the Wilderness example (examples/mods/wilderness).

Two low-poly melee weapons as glTF models with their own clips (`idle`,
`equip`, `use`) and their 32x32 inventory icons:

  * the Iron Broadsword - a diamond-section steel blade with a fuller and
    bright bevelled edges, a brass crossguard, a leather grip with darker
    wraps and an iron pommel;
  * the Stone Club - a crude wooden haft lashed to a lump of rock.

Held-item conventions (docs/modding/authoring.md, "Held items"):
one unit is a metre; the origin is where the fist closes around the grip; +Y
runs from the fist up the blade; +Z is the face shown to the holder (the flat
of the blade), so the guard and the edges lie along X. Made
the way make_example_assets.py makes the other examples' art, with its
helpers; nothing comes from CastleMiner Z. Deterministic: running it again
rewrites the same bytes.

    py -3 tools/ModSdk/make_wilderness_weapons.py [--root examples/mods]

Needs Python 3.10+ and Pillow.
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import struct
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_example_assets import quat_x, save_png  # noqa: E402  (the shared helpers)

# ---------------------------------------------------------------------------
# meshes: flat-shaded polygons, one primitive per material
# ---------------------------------------------------------------------------


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(v):
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / length, v[1] / length, v[2] / length)


class Mesh:
    """Polygons per material, each with its own face normal (the low-poly look)."""

    def __init__(self):
        self.parts: dict[str, list[list[tuple]]] = {}

    def polygon(self, material: str, corners: list[tuple], outward: tuple | None = None):
        """A convex polygon, counter-clockwise seen from outside (glTF's front).
        With `outward`, the winding is turned to face that way."""
        normal = unit(cross(sub(corners[1], corners[0]), sub(corners[2], corners[0])))
        if outward is not None and sum(normal[k] * outward[k] for k in range(3)) < 0:
            corners = list(reversed(corners))
        self.parts.setdefault(material, []).append(corners)

    def box(self, material: str, center, size):
        cx, cy, cz = center
        hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
        self.prism(material, [(cx + hx, cz + hz), (cx - hx, cz + hz), (cx - hx, cz - hz), (cx + hx, cz - hz)],
                   cy - hy, cy + hy)

    def prism(self, material: str, ring: list[tuple[float, float]], bottom: float, top: float,
              top_ring: list[tuple[float, float]] | None = None, caps: bool = True):
        """A prism along Y between two rings of (x, z) points (the second may be smaller)."""
        upper = top_ring or ring
        count = len(ring)
        centre = (sum(p[0] for p in ring) / count, 0.0, sum(p[1] for p in ring) / count)
        for i in range(count):
            j = (i + 1) % count
            a = (ring[i][0], bottom, ring[i][1])
            b = (ring[j][0], bottom, ring[j][1])
            c = (upper[j][0], top, upper[j][1])
            d = (upper[i][0], top, upper[i][1])
            mid = ((a[0] + b[0]) / 2 - centre[0], 0.0, (a[2] + b[2]) / 2 - centre[2])
            self.polygon(material, [a, b, c, d], mid)
        if caps:
            self.polygon(material, [(p[0], top, p[1]) for p in upper], (0.0, 1.0, 0.0))
            self.polygon(material, [(p[0], bottom, p[1]) for p in ring], (0.0, -1.0, 0.0))

    def scale(self, factor: float):
        for polygons in self.parts.values():
            for index, corners in enumerate(polygons):
                polygons[index] = [(x * factor, y * factor, z * factor) for x, y, z in corners]

    def turn_about_y(self, degrees: float):
        """Turns every polygon about Y (built edge-on, shown flat-on)."""
        c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
        for polygons in self.parts.values():
            for index, corners in enumerate(polygons):
                polygons[index] = [(x * c + z * s, y, -x * s + z * c) for x, y, z in corners]

    def triangles(self, material):
        positions, normals = [], []
        for corners in self.parts[material]:
            normal = unit(cross(sub(corners[1], corners[0]), sub(corners[2], corners[0])))
            for k in range(1, len(corners) - 1):
                for corner in (corners[0], corners[k], corners[k + 1]):
                    positions.append(corner)
                    normals.append(normal)
        return positions, normals


def write_item_model(path: Path, name: str, mesh: Mesh, materials: dict[str, tuple],
                     clips: dict[str, tuple[float, list, list]]) -> None:
    """One node `name` holding the mesh (a primitive per material); clips:
    clip -> (duration, [(time, rotation quaternion)], [(time, translation)])."""
    buffer = bytearray()
    accessors, views = [], []

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

    material_list, primitives = [], []
    for material, (color, metallic, roughness) in materials.items():
        if material not in mesh.parts:
            continue
        material_list.append({"name": material, "pbrMetallicRoughness": {
            "baseColorFactor": [round(c / 255, 4) for c in color] + [1.0],
            "metallicFactor": metallic, "roughnessFactor": roughness}})
        positions, normals = mesh.triangles(material)
        positions = [tuple(round(v, 5) for v in p) for p in positions]
        p = add_view(b"".join(struct.pack("<3f", *v) for v in positions), 34962)
        n = add_view(b"".join(struct.pack("<3f", *v) for v in normals), 34962)
        lo = [min(v[k] for v in positions) for k in range(3)]
        hi = [max(v[k] for v in positions) for k in range(3)]
        pa = add_accessor(p, "VEC3", 5126, len(positions), {"min": lo, "max": hi})
        na = add_accessor(n, "VEC3", 5126, len(normals))
        primitives.append({"attributes": {"POSITION": pa, "NORMAL": na}, "material": len(material_list) - 1})
    animations = []
    for clip, (duration, rotations, translations) in clips.items():
        samplers, channels = [], []
        for path_name, keys, kind, size in (("rotation", rotations, "VEC4", 4), ("translation", translations, "VEC3", 3)):
            if not keys:
                continue
            times = add_view(b"".join(struct.pack("<f", t) for t, _ in keys))
            values = add_view(b"".join(struct.pack(f"<{size}f", *v) for _, v in keys))
            ta = add_accessor(times, "SCALAR", 5126, len(keys), {"min": [keys[0][0]], "max": [keys[-1][0]]})
            va = add_accessor(values, kind, 5126, len(keys))
            samplers.append({"input": ta, "output": va, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": 0, "path": path_name}})
        assert abs(max(t for t, _ in rotations + translations) - duration) < 1e-6, clip
        animations.append({"name": clip, "samplers": samplers, "channels": channels})
    document = {
        "asset": {"version": "2.0", "generator": "OpenMinerZ make_wilderness_weapons.py"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": primitives}],
        "materials": material_list, "accessors": accessors, "bufferViews": views, "animations": animations,
        "buffers": [{"byteLength": len(buffer),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")


def octagon(radius: float, squash: float = 1.0):
    return [(radius * math.cos(math.pi / 8 + k * math.pi / 4) * squash, radius * math.sin(math.pi / 8 + k * math.pi / 4))
            for k in range(8)]


# ---------------------------------------------------------------------------
# the clips: played on the whole weapon, relative to the hand
# ---------------------------------------------------------------------------

IDENTITY = (0.0, 0.0, 0.0, 1.0)


def quat_z(degrees):
    r = math.radians(degrees) / 2
    return (0.0, 0.0, math.sin(r), math.cos(r))


def weapon_clips(swing_degrees: float, use_seconds: float):
    """idle: a slow breath of movement; equip: brought up from lowered with a
    little overshoot; use: the blade is drawn back, then whips across in its
    own plane through the strike (the arm's own swing does the rest) and
    comes back."""
    idle_rotations = [(t, quat_z(2.0 * math.sin(2 * math.pi * t / 3.0))) for t in (0.0, 0.75, 1.5, 2.25, 3.0)]
    equip = [(0.0, quat_x(70.0)), (0.12, quat_x(30.0)), (0.22, quat_x(-8.0)), (0.32, quat_x(2.0)), (0.4, IDENTITY)]
    equip_lift = [(0.0, (0.0, -0.12, 0.0)), (0.22, (0.0, 0.01, 0.0)), (0.4, (0.0, 0.0, 0.0))]
    u = use_seconds
    use = [(0.0, IDENTITY), (0.2 * u, quat_z(-0.4 * swing_degrees)), (0.5 * u, quat_z(swing_degrees)),
           (0.75 * u, quat_z(0.35 * swing_degrees)), (u, IDENTITY)]
    use_reach = [(0.0, (0.0, 0.0, 0.0)), (0.5 * u, (0.0, 0.03, -0.04)), (u, (0.0, 0.0, 0.0))]
    return {"idle": (3.0, idle_rotations, []), "equip": (0.4, equip, equip_lift), "use": (u, use, use_reach)}


# ---------------------------------------------------------------------------
# the Iron Broadsword
# ---------------------------------------------------------------------------

SWORD_MATERIALS = {
    "steel": ((178, 188, 204), 0.0, 0.5),
    "edge": ((236, 242, 250), 0.0, 0.4),
    "fuller": ((112, 122, 140), 0.0, 0.6),
    "brass": ((206, 160, 62), 0.0, 0.5),
    "leather": ((104, 64, 36), 0.0, 0.9),
    "wrap": ((58, 34, 20), 0.0, 0.9),
    "iron": ((92, 96, 108), 0.0, 0.6),
}


def broadsword_mesh() -> Mesh:
    mesh = Mesh()
    # The blade: a diamond section (ridge down the middle of each flat,
    # bevelled edges), wide at the guard, narrowing, then a point.
    base_y, shoulder_y, tip_y = 0.085, 0.58, 0.70
    def ring(width, thickness):
        t, w = thickness, width
        return [(t, 0.0), (0.55 * t, 0.62 * w), (0.0, w), (-0.55 * t, 0.62 * w),
                (-t, 0.0), (-0.55 * t, -0.62 * w), (0.0, -w), (0.55 * t, -0.62 * w)]
    lower, upper = ring(0.038, 0.009), ring(0.031, 0.007)
    for i in range(8):
        j = (i + 1) % 8
        a = (lower[i][0], base_y, lower[i][1])
        b = (lower[j][0], base_y, lower[j][1])
        c = (upper[j][0], shoulder_y, upper[j][1])
        d = (upper[i][0], shoulder_y, upper[i][1])
        # Faces from a point on the ridge to a bevel are the flats; from a
        # bevel to the edge, the sharpened bevel.
        material = "edge" if i in (1, 2, 5, 6) else "steel"
        mid = ((a[0] + b[0]) / 2, 0.0, (a[2] + b[2]) / 2)
        mesh.polygon(material, [a, b, c, d], mid)
        tip = (0.0, tip_y, 0.0)
        mesh.polygon(material, [d, c, tip], mid)
    mesh.polygon("steel", [(p[0], base_y, p[1]) for p in lower], (0.0, -1.0, 0.0))
    # The fuller: a dark groove down both flats, just proud of the ridge.
    for side in (1.0, -1.0):
        x = side * 0.0092
        mesh.polygon("fuller", [(x, 0.10, -0.006), (x, 0.10, 0.006), (x, 0.46, 0.0045), (x, 0.46, -0.0045)],
                     (side, 0.0, 0.0))
    # The crossguard: a brass bar across the blade's width, its tips swept
    # a little towards the blade.
    mesh.box("brass", (0.0, 0.072, 0.0), (0.034, 0.028, 0.15))
    for side in (1.0, -1.0):
        z0, z1 = side * 0.075, side * 0.125
        lo = [(0.016, 0.058, z0), (-0.016, 0.058, z0), (-0.012, 0.072, z1), (0.012, 0.072, z1)]
        hi = [(0.016, 0.086, z0), (-0.016, 0.086, z0), (-0.012, 0.094, z1), (0.012, 0.094, z1)]
        # Four sides and an end cap of the swept tip.
        out = (0.0, 0.0, side)
        mesh.polygon("brass", [lo[0], lo[3], hi[3], hi[0]], (1.0, 0.0, 0.0))
        mesh.polygon("brass", [lo[1], hi[1], hi[2], lo[2]], (-1.0, 0.0, 0.0))
        mesh.polygon("brass", [hi[0], hi[3], hi[2], hi[1]], (0.0, 1.0, 0.0))
        mesh.polygon("brass", [lo[0], lo[1], lo[2], lo[3]], (0.0, -1.0, 0.0))
        mesh.polygon("brass", [lo[3], lo[2], hi[2], hi[3]], out)
    # The grip: leather over an octagonal core, three darker wraps.
    mesh.prism("leather", octagon(0.017, 0.85), -0.085, 0.058)
    for y in (-0.06, -0.015, 0.03):
        mesh.prism("wrap", octagon(0.0195, 0.85), y - 0.006, y + 0.006)
    # The pommel: an iron knob under the grip.
    mesh.prism("iron", octagon(0.022), -0.115, -0.093, octagon(0.026))
    mesh.prism("iron", octagon(0.026), -0.093, -0.085, octagon(0.018))
    mesh.prism("iron", octagon(0.016), -0.128, -0.115, octagon(0.022))
    mesh.turn_about_y(90.0)  # flat towards the holder, guard and edges along X
    # A blocky world's broadsword: 0.62 m from pommel to point, so it reads
    # in the hand without filling the view.
    mesh.scale(0.75)
    return mesh


# ---------------------------------------------------------------------------
# the Stone Club
# ---------------------------------------------------------------------------

CLUB_MATERIALS = {
    "wood": ((128, 86, 48), 0.0, 0.9),
    "bark": ((86, 58, 34), 0.0, 0.95),
    "stone": ((138, 136, 128), 0.0, 0.85),
    "stone_dark": ((98, 96, 92), 0.0, 0.9),
    "cord": ((196, 170, 112), 0.0, 0.9),
}


def club_mesh() -> Mesh:
    mesh = Mesh()
    # A haft that thickens towards the head.
    mesh.prism("wood", octagon(0.018), -0.11, 0.33, octagon(0.024))
    mesh.prism("bark", octagon(0.021), -0.13, -0.11, octagon(0.018))
    # A lump of rock lashed to the top, rough faces of two greys.
    rock = [(0.06, 0.0), (0.04, 0.055), (-0.02, 0.07), (-0.065, 0.02), (-0.05, -0.05), (0.01, -0.07), (0.055, -0.04)]
    mesh.prism("stone", rock, 0.30, 0.40, [(x * 0.85, z * 0.85 + 0.006) for x, z in rock], caps=False)
    mesh.prism("stone_dark", [(x * 0.85, z * 0.85 + 0.006) for x, z in rock], 0.40, 0.45,
               [(x * 0.45, z * 0.45) for x, z in rock])
    mesh.prism("stone_dark", [(x * 0.7, z * 0.7) for x, z in rock], 0.27, 0.30, rock)
    mesh.polygon("stone", [(x * 0.45, 0.45, z * 0.45) for x, z in rock], (0.0, 1.0, 0.0))
    mesh.polygon("stone_dark", [(x * 0.7, 0.27, z * 0.7) for x, z in rock], (0.0, -1.0, 0.0))
    # The lashing under the rock.
    for y in (0.245, 0.262):
        mesh.prism("cord", octagon(0.028), y - 0.006, y + 0.006)
    mesh.turn_about_y(90.0)
    mesh.scale(0.8)
    return mesh


# ---------------------------------------------------------------------------
# icons: 32x32 pixel art, held by the lower left like a sword sprite
# ---------------------------------------------------------------------------


def diagonal_icon(paint) -> Image.Image:
    """A 32x32 icon painted along the diagonal: `paint(u, s)` gives the colour
    (or None) of the pixel `u` along the item from the lower-left grip
    towards the upper-right tip and `s` across it (negative: the lit,
    upper-left side), in pixels. Every empty pixel that touches a painted
    one becomes the dark outline."""
    image = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    px = image.load()
    origin_x, origin_y = 11.0, 21.0
    for y in range(32):
        for x in range(32):
            dx, dy = x + 0.5 - origin_x, y + 0.5 - origin_y
            color = paint((dx - dy) / 2.0, (dx + dy) / 2.0)
            if color is not None:
                px[x, y] = color + (255,)
    outline = (30, 26, 34, 255)
    painted = {(x, y) for y in range(32) for x in range(32) if px[x, y][3] > 0}
    for y in range(32):
        for x in range(32):
            if (x, y) in painted:
                continue
            if any((x + dx, y + dy) in painted for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                px[x, y] = outline
    return image


def icon_broadsword() -> Image.Image:
    def paint(u, s):
        # The blade: three pixels across, its lit edge bright, a dark fuller
        # down the middle, tapering to the point.
        if 0.0 <= u <= 15.5:
            half = 0.75 if u < 12.0 else 0.75 * (15.5 - u) / 3.5 + 0.01
            if abs(s) <= half:
                if s < -0.25:
                    return (242, 246, 252)
                if s > 0.25:
                    return (150, 160, 180)
                return (116, 126, 146) if 1.0 < u < 10.5 else (190, 200, 216)
        # The crossguard across the foot of the blade.
        if -1.6 <= u < -0.1 and abs(s) <= 4.1:
            return (234, 188, 84) if s < 0.0 else (192, 144, 50)
        # The grip, wrapped.
        if -5.6 <= u < -1.6 and abs(s) <= 0.3:
            return (70, 40, 22) if int(math.floor(u)) % 2 == 0 else (116, 72, 40)
        # The pommel.
        if -7.2 <= u < -5.6 and abs(s) <= 0.8:
            return (150, 154, 168) if s < 0.0 else (100, 104, 118)
        return None

    return diagonal_icon(paint)


def icon_club() -> Image.Image:
    def paint(u, s):
        # A rough lump of rock on the end.
        dx, dy = u - 11.5, s * 1.15
        r = math.hypot(dx, dy)
        if r <= 4.2 + 0.6 * math.sin(3.0 * math.atan2(dy, dx)):
            shade = 156 if dx + dy < 0.0 else 116
            return (shade, shade - 2, shade - 10)
        # The lashing below it.
        if 6.0 <= u < 7.4 and abs(s) <= 0.8:
            return (214, 188, 128)
        # The haft, thicker towards the head.
        if -7.5 <= u < 7.5 and abs(s) <= (0.3 if u < 0.0 else 0.8):
            return (152, 104, 60) if s <= 0.0 else (112, 76, 42)
        return None

    return diagonal_icon(paint)


# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2] / "examples" / "mods")
    arguments = parser.parse_args()
    mod = arguments.root / "wilderness"
    models = mod / "assets" / "models" / "weapons"
    icons = mod / "assets" / "textures" / "icons"
    write_item_model(models / "iron_broadsword.gltf", "iron_broadsword", broadsword_mesh(), SWORD_MATERIALS,
                     weapon_clips(55.0, 0.5))
    write_item_model(models / "stone_club.gltf", "stone_club", club_mesh(), CLUB_MATERIALS,
                     weapon_clips(45.0, 0.6))
    save_png(icon_broadsword(), icons / "iron_broadsword.png")
    save_png(icon_club(), icons / "stone_club.png")
    print(f"wrote the Wilderness weapons under {mod}")


if __name__ == "__main__":
    main()
