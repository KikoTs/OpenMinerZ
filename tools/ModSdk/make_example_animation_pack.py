"""Generates examples/mods/animation_pack: two ORIGINAL player clips.

    py tools/ModSdk/make_example_animation_pack.py <rig.glb> [<mod folder>]

<rig.glb> is a file written by `openminerz_mod_sdk export-rig` from your own
imported game. This script contains no game data: it takes the bone NAMES and
the hierarchy from that file (and, for the sway, where the hips, knees and
ankles sit, so the feet stay planted), and the length of the two clips the
pack replaces. Every key it writes is computed here:

  Stand    an exaggerated idle sway: the hips shift from side to side, the legs
           lean with them and the feet stay flat where they stand.
  PickUse  a wave instead of the pickaxe swing: the right arm reaches forward,
           the forearm upright, palm out, and waves once per swing.

The pack's files hold only the bones' names, their parents and these keys: no
joint offsets (the game reads rotations, and the Root's position), no mesh.

Authoring frame (docs/modding/animation.md): +Y up, the character faces
+Z, its left hand on +X, one unit is one block. At rest every joint's frame is
parallel to the Root's, so a rotation below is the bone's rotation relative to
its parent with both in the T-pose.
"""

import base64
import json
import math
import struct
import sys
from pathlib import Path

FRAME_RATE = 30


# --- quaternions (x, y, z, w) ------------------------------------------------

def axis_angle(axis, degrees):
    x, y, z = axis
    length = math.sqrt(x * x + y * y + z * z)
    half = math.radians(degrees) / 2.0
    s = math.sin(half) / length
    return (x * s, y * s, z * s, math.cos(half))


def multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def inverse(q):
    return (-q[0], -q[1], -q[2], q[3])


def rotate(q, v):
    p = multiply(multiply(q, (v[0], v[1], v[2], 0.0)), inverse(q))
    return (p[0], p[1], p[2])


def from_to(a, b):
    """The shortest rotation taking direction a to direction b."""
    a = normalized(a)
    b = normalized(b)
    cross = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    return normalized_quat((cross[0], cross[1], cross[2], 1.0 + dot))


def normalized(v):
    length = math.sqrt(sum(c * c for c in v))
    return tuple(c / length for c in v)


def normalized_quat(q):
    length = math.sqrt(sum(c * c for c in q))
    return tuple(c / length for c in q)


IDENTITY = (0.0, 0.0, 0.0, 1.0)
X, Y, Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)


# --- the exported rig ----------------------------------------------------------

def read_rig(path):
    data = Path(path).read_bytes()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        sys.exit(f"{path}: not a binary glTF written by export-rig")
    length, kind = struct.unpack_from("<II", data, 12)
    document = json.loads(data[20:20 + length])
    info = document.get("asset", {}).get("extras", {}).get("openminerz")
    if not info:
        sys.exit(f"{path}: not written by `openminerz_mod_sdk export-rig`")
    nodes = document["nodes"]
    bones = {}
    for index in document["skins"][0]["joints"]:
        node = nodes[index]
        bones[node["name"]] = {"index": index, "translation": node.get("translation", [0, 0, 0]),
                               "parent": None}
    by_index = {bone["index"]: name for name, bone in bones.items()}
    for name, bone in bones.items():
        for child in nodes[bone["index"]].get("children", []):
            if child in by_index:
                bones[by_index[child]]["parent"] = name
    lengths = {clip["name"]: clip["seconds"] for clip in info["clips"]}
    return bones, lengths


# --- the clips ------------------------------------------------------------------

def sway(bones, seconds):
    """Hips shifting side to side twice a loop, legs leaning, feet planted."""
    root = bones["Root"]["translation"]
    keys = round(seconds * FRAME_RATE) + 1
    channels = {"Root": {"translation": [], "rotation": []}}
    legs = {}
    for side in ("Left", "Right"):
        hip = bones["Hip" + side]["translation"]
        knee = bones["Knee" + side]["translation"]
        ankle = bones["Ankle" + side]["translation"]
        leg = (knee[0] + ankle[0], knee[1] + ankle[1])  # hip to ankle, frontal plane
        legs[side] = leg
        for bone in ("Hip", "Knee", "Ankle"):
            channels[bone + side] = {"rotation": []}
    for key in range(keys):
        time = key / FRAME_RATE
        shift = 0.07 * math.sin(2.0 * math.pi * 2.0 * time / seconds)
        # Lower the hips so neither leg has to stretch to reach its ankle.
        drops = []
        for leg in legs.values():
            reach = math.hypot(leg[0], leg[1])
            drops.append(leg[1] + math.sqrt(max(0.0, reach * reach - (leg[0] - shift) ** 2)))
        drop = min(drops)
        channels["Root"]["translation"].append((root[0] + shift, root[1] + drop, root[2]))
        channels["Root"]["rotation"].append(IDENTITY)
        for side, leg in legs.items():
            # The leg turns in the frontal plane from its rest direction to the
            # ankle, which has not moved; the ankle turns back, so the foot stays flat.
            target = (leg[0] - shift, leg[1] - drop)
            lean = math.degrees(math.atan2(target[1], target[0]) - math.atan2(leg[1], leg[0]))
            channels["Hip" + side]["rotation"].append(axis_angle(Z, lean))
            channels["Knee" + side]["rotation"].append(IDENTITY)
            channels["Ankle" + side]["rotation"].append(axis_angle(Z, -lean))
    return channels, keys


def wave(seconds):
    """The right arm forward and the forearm up, palm out, waving once a loop."""
    keys = max(2, round(seconds * FRAME_RATE))  # the last key lands just before the loop restarts
    # The upper arm: its own axis turned a quarter (so the elbow bends upward),
    # then swung 70 degrees forward from the T-pose. The forearm: bent a quarter
    # at the elbow. In the T-pose the right arm points along -X, palm down.
    reach = multiply(axis_angle(Y, 70.0), axis_angle(X, -90.0))
    upper_arm = rotate(reach, (-1.0, 0.0, 0.0))
    # Fingers up, palm forward, whatever the arm's swing.
    forearm_world = multiply(axis_angle(X, -90.0), axis_angle(Y, 90.0))
    elbow = multiply(inverse(reach), forearm_world)
    channels = {"ShoulderRight": {"rotation": []}, "ElbowRight": {"rotation": []},
                "WristRight": {"rotation": []}}
    for key in range(keys):
        phase = 2.0 * math.pi * (key / FRAME_RATE) / seconds
        # Waving is the upper arm rolling about itself: the forearm swings
        # from side to side in front of the shoulder, the wrist trailing. The
        # swing leans outward, so the held item stays clear of the head.
        channels["ShoulderRight"]["rotation"].append(
            multiply(axis_angle(upper_arm, -12.0 + 20.0 * math.sin(phase)), reach))
        channels["ElbowRight"]["rotation"].append(elbow)
        # (The hand's own Y axis is the palm's normal here.)
        channels["WristRight"]["rotation"].append(axis_angle(Y, -14.0 * math.sin(phase - 0.6)))
    return channels, keys


# --- glTF ------------------------------------------------------------------------

def write_clip(path, bones, name, channels, keys, note):
    names = list(bones)
    index = {bone: position for position, bone in enumerate(names)}
    nodes = []
    for bone in names:
        node = {"name": bone}
        children = [index[child] for child in names if bones[child]["parent"] == bone]
        if children:
            node["children"] = children
        nodes.append(node)
    buffer = bytearray()
    views, accessors, samplers, out_channels = [], [], [], []

    def add(values, kind, minimum=None, maximum=None):
        while len(buffer) % 4:
            buffer.append(0)
        offset = len(buffer)
        flat = [component for value in values for component in (value if isinstance(value, tuple) else (value,))]
        buffer.extend(struct.pack(f"<{len(flat)}f", *flat))
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(flat) * 4})
        accessor = {"bufferView": len(views) - 1, "componentType": 5126, "count": len(values), "type": kind}
        if minimum is not None:
            accessor["min"], accessor["max"] = [minimum], [maximum]
        accessors.append(accessor)
        return len(accessors) - 1

    times = add([key / FRAME_RATE for key in range(keys)], "SCALAR", 0.0, (keys - 1) / FRAME_RATE)
    for bone, paths in channels.items():
        for path_name, values in paths.items():
            output = add([tuple(round(c, 7) for c in v) for v in values],
                         "VEC4" if path_name == "rotation" else "VEC3")
            samplers.append({"input": times, "output": output, "interpolation": "LINEAR"})
            out_channels.append({"sampler": len(samplers) - 1, "target": {"node": index[bone], "path": path_name}})
    document = {
        "asset": {"version": "2.0", "generator": "OpenMinerZ tools/ModSdk/make_example_animation_pack.py",
                  "copyright": "Original animation, OpenMinerZ examples"},
        "scene": 0,
        "scenes": [{"nodes": [index["Root"]]}],
        "nodes": nodes,
        "animations": [{"name": name, "channels": out_channels, "samplers": samplers,
                        "extras": {"openminerz": note}}],
        "buffers": [{"byteLength": len(buffer),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
        "bufferViews": views,
        "accessors": accessors,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {path} ({keys} keys, {len(out_channels)} channels)")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bones, lengths = read_rig(sys.argv[1])
    folder = Path(sys.argv[2]) if len(sys.argv) > 2 else \
        Path(__file__).resolve().parents[2] / "examples" / "mods" / "animation_pack"
    animations = folder / "assets" / "animations"
    channels, keys = sway(bones, lengths["Stand"])
    write_clip(animations / "Stand.gltf", bones, "Stand", channels, keys,
               "An exaggerated idle sway: hips side to side, feet planted.")
    channels, keys = wave(lengths["PickUse"])
    write_clip(animations / "PickUse.gltf", bones, "PickUse", channels, keys,
               "A wave in place of the pickaxe swing, once per swing.")


if __name__ == "__main__":
    main()
