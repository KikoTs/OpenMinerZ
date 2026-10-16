"""Generates examples/mods/tactical_animations: the "Tactical Animations" pack.

    py -3 tools/ModSdk/make_tactical_animations.py <rig.glb> [<mod folder>]

<rig.glb> is a file written by `openminerz_mod_sdk export-rig` from your own
imported game. This script contains no game data: it takes the bone NAMES, the
hierarchy and the joint offsets from that file and the length of each clip the
pack replaces. Motion is authored here; recorded foley is credited separately.

The pack replaces the arm clips of every gun family (idle, walk, aim, fire and
reload) with original animation, cues licensed recorded foley from events
inside the clips, moves parts of the guns (magazines, slides, bolts, pumps,
shells, energy cells, rockets) and turns on the engine's procedural viewmodel
motion with values tuned per family. It is a client mod: presentation only.

How the clips are made
----------------------
Nothing is keyed by hand bone by bone. Each clip is a timeline of a few
high-level tracks, evaluated 30 times a second:

  * where the gun is, in the first-person camera's own space (an offset and a
    pitch / yaw / roll about a pivot in the gun), from a per-family hold;
  * where the support (left) hand is: on a named point of the gun, at the
    magazine pouch, holding a part, ...;
  * how far each hand's fingers close, the trigger finger, the thumb;
  * small torso, neck and head motion (the camera rides the head in first
    person, so this is the "camera feel" of a reload).

From those, inverse kinematics places both arms (the right hand never lets go
of the grip: the gun hangs from the right hand's prop bone, as in the game),
the forearm and upper-arm twist helpers take part of the twist as the game's
own clips do, and the result is written as bone rotations. Timing comes from
easing curves with anticipation and overshoot.

Authoring frame (docs/modding/animation.md): +Y up, the character faces
+Z, its left hand on +X, one unit is one block. At rest every joint's frame is
parallel to the Root's, so a rotation below is the bone's rotation relative to
its parent with both in the T-pose.

Needs Python 3.10+; the sounds also need numpy (tools/ModSdk/tactical_foley.py).
Deterministic: running it again rewrites the same bytes.
"""

from __future__ import annotations

import base64
import json
import math
import struct
import sys
from dataclasses import dataclass, field, replace
from pathlib import Path
import tactical_authoring

sys.path.insert(0, str(Path(__file__).resolve().parent))

FRAME_RATE = 30

# ---------------------------------------------------------------------------
# Vectors and quaternions (x, y, z, w), column convention: v' = q v q*.
# ---------------------------------------------------------------------------


def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def v_len(a):
    return math.sqrt(v_dot(a, a))


def v_norm(a):
    length = v_len(a)
    return (a[0] / length, a[1] / length, a[2] / length) if length > 1e-12 else (0.0, 0.0, 0.0)


def v_lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


IDENTITY = (0.0, 0.0, 0.0, 1.0)
X, Y, Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)


def q_axis_angle(axis, degrees):
    axis = v_norm(axis)
    half = math.radians(degrees) * 0.5
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def q_inv(q):
    return (-q[0], -q[1], -q[2], q[3])


def q_norm(q):
    length = math.sqrt(sum(c * c for c in q))
    return tuple(c / length for c in q)


def q_rotate(q, v):
    # v + 2w (u x v) + 2 u x (u x v)
    u = (q[0], q[1], q[2])
    t = v_scale(v_cross(u, v), 2.0)
    return v_add(v_add(v, v_scale(t, q[3])), v_cross(u, t))


def q_from_to(a, b):
    a = v_norm(a)
    b = v_norm(b)
    dot = v_dot(a, b)
    if dot < -0.999999:
        side = v_cross(a, X if abs(a[0]) < 0.9 else Y)
        return q_axis_angle(side, 180.0)
    c = v_cross(a, b)
    return q_norm((c[0], c[1], c[2], 1.0 + dot))


def q_slerp(a, b, t):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b = tuple(-c for c in b)
        dot = -dot
    if dot > 0.9995:
        return q_norm(tuple(x + (y - x) * t for x, y in zip(a, b)))
    theta = math.acos(dot)
    s = math.sin(theta)
    wa = math.sin((1.0 - t) * theta) / s
    wb = math.sin(t * theta) / s
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def q_angle(q):
    return math.degrees(2.0 * math.acos(min(1.0, abs(q[3]))))


def q_from_basis(x_axis, y_axis, z_axis):
    """Rotation whose columns are the given orthonormal axes."""
    m00, m10, m20 = x_axis
    m01, m11, m21 = y_axis
    m02, m12, m22 = z_axis
    trace = m00 + m11 + m22
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        q = ((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s)
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        q = (0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        q = ((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s)
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        q = ((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s)
    return q_norm(q)


def q_look(forward, up):
    """Rotation taking +Z to `forward` and +Y as close to `up` as it can."""
    z = v_norm(forward)
    x = v_norm(v_cross(up, z))
    y = v_cross(z, x)
    return q_from_basis(x, y, z)


def q_twist(q, axis):
    """The twist part of q about `axis` (swing-twist decomposition)."""
    axis = v_norm(axis)
    p = v_dot((q[0], q[1], q[2]), axis)
    twist = (axis[0] * p, axis[1] * p, axis[2] * p, q[3])
    length = math.sqrt(sum(c * c for c in twist))
    if length < 1e-9:
        return IDENTITY
    return tuple(c / length for c in twist)


def q_power(q, t):
    """q scaled to the fraction t of its angle."""
    return q_slerp(IDENTITY, q, t)


@dataclass
class Frame:
    """A rigid transform: p + R v."""
    p: tuple = (0.0, 0.0, 0.0)
    q: tuple = IDENTITY

    def apply(self, v):
        return v_add(self.p, q_rotate(self.q, v))

    def direction(self, v):
        return q_rotate(self.q, v)

    def compose(self, local: "Frame") -> "Frame":
        return Frame(self.apply(local.p), q_norm(q_mul(self.q, local.q)))

    def inverse(self) -> "Frame":
        qi = q_inv(self.q)
        return Frame(v_scale(q_rotate(qi, self.p), -1.0), qi)


def lerp_frame(a: Frame, b: Frame, t: float) -> Frame:
    return Frame(v_lerp(a.p, b.p, t), q_slerp(a.q, b.q, t))


# ---------------------------------------------------------------------------
# The exported rig and forward kinematics
# ---------------------------------------------------------------------------

# Where the first-person camera sits: the game looks out of the head joint,
# 5 cm above it, along the head's own forward axis (Avatar.EyePointCamera).
EYE_ABOVE_HEAD = 0.05


def read_rig(path):
    """Bone names, parents, rest offsets and the clip lengths from an export-rig .glb."""
    data = Path(path).read_bytes()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        sys.exit(f"{path}: not a binary glTF written by export-rig")
    length, _kind = struct.unpack_from("<II", data, 12)
    document = json.loads(data[20:20 + length])
    info = document.get("asset", {}).get("extras", {}).get("openminerz")
    if not info:
        sys.exit(f"{path}: not written by `openminerz_mod_sdk export-rig`")
    nodes = document["nodes"]
    joints = document["skins"][0]["joints"]
    by_index = {index: nodes[index]["name"] for index in joints}
    bones = {}
    for index in joints:
        node = nodes[index]
        bones[node["name"]] = {"translation": tuple(node.get("translation", (0.0, 0.0, 0.0))),
                               "parent": None, "order": len(bones)}
    for index in joints:
        for child in nodes[index].get("children", []):
            if child in by_index:
                bones[by_index[child]]["parent"] = by_index[index]
    lengths = {clip["name"]: clip["seconds"] for clip in info["clips"]}
    return bones, lengths, info.get("generation", "")


class Skeleton:
    def __init__(self, bones):
        tactical_authoring.configure(sys.modules[__name__])
        tactical_authoring.validate(tactical_authoring.ACTIVE, set(bones))
        mapping = tactical_authoring.ACTIVE.get("bones", {})
        reverse = {source: role for role, source in mapping.items()}
        self.source_names = mapping
        bones = {reverse.get(name, name): {**bone, "parent": reverse.get(bone["parent"], bone["parent"])}
                 for name, bone in bones.items()}
        self.bones = bones
        for side in ("Left", "Right"):
            for child,parent in (("Shoulder","Collar"),("Elbow","Shoulder"),("Wrist","Elbow"),("Prop","Wrist")):
                if bones[child+side]["parent"] != parent+side:
                    raise ValueError(f"Broken {side} arm mapping: {child} must descend directly from {parent}")
        self.names = sorted(bones, key=lambda name: bones[name]["order"])
        self.children = {name: [] for name in self.names}
        for name in self.names:
            parent = bones[name]["parent"]
            if parent is not None:
                self.children[parent].append(name)

    def offset(self, name):
        return self.bones[name]["translation"]

    def parent(self, name):
        return self.bones[name]["parent"]

    def world(self, rotations, root=None):
        """World frames of every bone for local rotations (missing: identity)."""
        frames = {}
        for name in self.names:
            parent = self.bones[name]["parent"]
            local = Frame(self.bones[name]["translation"], rotations.get(name, IDENTITY))
            if parent is None:
                frames[name] = root if root is not None else local
            else:
                frames[name] = frames[parent].compose(local)
        return frames

    def chain_world(self, frames, rotations, start):
        """Recompute `start` and everything below it after its rotations changed."""
        stack = [start]
        while stack:
            name = stack.pop()
            parent = self.bones[name]["parent"]
            local = Frame(self.bones[name]["translation"], rotations.get(name, IDENTITY))
            frames[name] = frames[parent].compose(local)
            stack.extend(self.children[name])


def camera_frame(head: Frame) -> Frame:
    """The first-person camera: x right, y up, z toward the viewer (it looks down -z)."""
    position = head.apply((0.0, EYE_ABOVE_HEAD, 0.0))
    # A half turn about the head's up axis: camera right is the head's -X (the
    # character's right) and the camera looks along the head's +Z.
    return Frame(position, q_mul(head.q, q_axis_angle(Y, 180.0)))


def in_view(x, y, depth):
    """A point of the first-person view: right, up, and how far ahead of the eye."""
    return (x, y, -depth)


# ---------------------------------------------------------------------------
# Arms: two-bone inverse kinematics with a chosen elbow swivel
# ---------------------------------------------------------------------------

ARMS = {
    # side: +1 when the arm's rest direction is +X (the left arm), -1 for the right.
    "Left": {"collar": "CollarLeft", "shoulder": "ShoulderLeft", "elbow": "ElbowLeft",
             "wrist": "WristLeft", "prop": "PropLeft", "upper_twist": "27", "fore_twist": "31",
             "side": 1.0},
    "Right": {"collar": "CollarRight", "shoulder": "ShoulderRight", "elbow": "ElbowRight",
              "wrist": "WristRight", "prop": "PropRight", "upper_twist": "30", "fore_twist": "34",
              "side": -1.0},
}

# How much of the wrist's twist the forearm helper takes (the game's own clips
# hand the helper most of it, so the sleeve turns with the hand).
FOREARM_TWIST_SHARE = 0.65
UPPER_ARM_UNTWIST_SHARE = 0.4


def _swing_twist_cost(q_local, side):
    """How strained a wrist rotation looks: bend and sideways bend weigh most."""
    twist = q_twist(q_local, X)
    swing = q_mul(q_local, q_inv(twist))
    # Swing about Z is flexion/extension, about Y the sideways (radial/ulnar) bend.
    sz = math.radians(q_angle(q_twist(swing, Z)))
    sy = math.radians(q_angle(q_twist(swing, Y)))
    tw = math.radians(q_angle(twist))
    return sz * sz + 2.5 * sy * sy + 0.35 * tw * tw


def solve_arm(skeleton, frames, rotations, arm, wrist_target: Frame, swivel_hint=None,
              swivel_preference=0.0, swivel_range=(-100.0, 100.0)):
    """Places shoulder, elbow and wrist so the wrist lands on `wrist_target`.

    The elbow's swivel about the shoulder-wrist line is searched: the one that
    keeps the wrist least bent wins, pulled toward `swivel_preference` (0 is
    elbow down, positive swings it out to the side) and toward `swivel_hint`
    (the previous frame's, so the elbow never pops). Returns the swivel.
    """
    info = ARMS[arm]
    side = info["side"]
    shoulder, elbow, wrist = info["shoulder"], info["elbow"], info["wrist"]
    parent = frames[info["collar"]]
    a_vec = skeleton.offset(elbow)
    b_vec = skeleton.offset(wrist)
    a = v_len(a_vec)
    b = v_len(b_vec)
    u0 = v_norm(a_vec)
    h0 = v_norm(v_sub(Y, v_scale(u0, v_dot(Y, u0))))
    q_rest = q_from_basis(u0, h0, v_cross(u0, h0))
    shoulder_p = parent.apply(skeleton.offset(shoulder))

    def place(target_p, swivel):
        to = v_sub(target_p, shoulder_p)
        d = min(max(v_len(to), abs(a - b) * 1.05 + 1e-4), (a + b) * 0.9995)
        direction = v_norm(to)
        cos_a = (a * a + d * d - b * b) / (2.0 * a * d)
        alpha = math.acos(max(-1.0, min(1.0, cos_a)))
        # A vertical pole becomes singular when the hand passes under the
        # shoulder on its way to a pouch. Bias it outward and behind the body
        # so that crossing that line cannot flip the forearm inside out.
        pole = (side * 0.55, -0.75, -0.35)
        down = v_sub(pole, v_scale(direction, v_dot(pole, direction)))
        if v_len(down) < 1e-4:
            down = v_sub((0.0, 0.0, -1.0), v_scale(direction, -direction[2]))
        n0 = v_norm(down)
        m0 = v_scale(v_cross(direction, n0), side)  # positive swivel: elbow out to its own side
        phi = math.radians(swivel)
        n = v_add(v_scale(n0, math.cos(phi)), v_scale(m0, math.sin(phi)))
        elbow_p = v_add(shoulder_p, v_scale(v_add(v_scale(direction, math.cos(alpha)),
                                                  v_scale(n, math.sin(alpha))), a))
        wrist_p = v_add(shoulder_p, v_scale(direction, d))
        upper = v_norm(v_sub(elbow_p, shoulder_p))
        fore = v_norm(v_sub(wrist_p, elbow_p))
        hinge = v_cross(upper, fore) if side < 0 else v_cross(fore, upper)
        if v_len(hinge) < 1e-5:
            hinge = v_cross(upper, n) if side < 0 else v_cross(n, upper)
        hinge = v_norm(hinge)
        q_world = q_mul(q_from_basis(upper, hinge, v_cross(upper, hinge)), q_inv(q_rest))
        q_shoulder = q_norm(q_mul(q_inv(parent.q), q_world))
        bent = q_rotate(q_world, v_norm(b_vec))
        theta = math.atan2(v_dot(hinge, v_cross(bent, fore)), v_dot(bent, fore))
        q_elbow = q_axis_angle(h0, math.degrees(theta))
        q_elbow_world = q_norm(q_mul(q_world, q_elbow))
        q_wrist = q_norm(q_mul(q_inv(q_elbow_world), wrist_target.q))
        return q_shoulder, q_elbow, q_wrist

    def cost(swivel):
        _, _, q_wrist = place(wrist_target.p, swivel)
        value = _swing_twist_cost(q_wrist, side)
        value += 0.6 * math.radians(swivel - swivel_preference) ** 2
        if swivel_hint is not None:
            value += 4.0 * math.radians(swivel - swivel_hint) ** 2
        return value

    lo, hi = swivel_range
    if swivel_hint is not None:
        lo = max(lo, swivel_hint - 25.0)
        hi = min(hi, swivel_hint + 25.0)
    best = min((cost(s), s) for s in frange(lo, hi, 4.0))[1]
    best = min((cost(s), s) for s in frange(max(lo, best - 4.0), min(hi, best + 4.0), 0.5))[1]

    # The rig's bones are not exactly straight: correct the target twice so
    # the wrist really lands where it was asked to.
    target_p = wrist_target.p
    for _ in range(3):
        qs, qe, qw = place(target_p, best)
        rotations[shoulder], rotations[elbow], rotations[wrist] = qs, qe, qw
        skeleton.chain_world(frames, rotations, shoulder)
        error = v_sub(wrist_target.p, frames[wrist].p)
        if v_len(error) < 1e-5:
            break
        target_p = v_add(target_p, error)

    # Twist helpers: the forearm takes most of the wrist's roll, the top of the
    # upper arm unwinds part of the shoulder's.
    rotations[info["fore_twist"]] = q_power(q_twist(rotations[wrist], X), FOREARM_TWIST_SHARE)
    rotations[info["upper_twist"]] = q_power(q_inv(q_twist(rotations[shoulder], X)), UPPER_ARM_UNTWIST_SHARE)
    skeleton.chain_world(frames, rotations, shoulder)
    return best


def frange(lo, hi, step):
    count = int(math.floor((hi - lo) / step + 1e-9)) + 1
    return [lo + step * i for i in range(count)]


# ---------------------------------------------------------------------------
# Hands
# ---------------------------------------------------------------------------

FINGERS = ("Index", "Middle", "Ring", "Small")


@dataclass
class Grip:
    """How a hand is closed: 0 open and flat, 1 a tight fist."""
    curl: float = 0.0
    trigger: float | None = None   # the index finger on its own (None: like the others)
    thumb: float | None = None     # the thumb's wrap (None: follows curl)
    spread: float = 0.0            # fingers fanned (a reaching, open hand)
    index_joints: tuple | None = None  # optional MCP/PIP/DIP fit to a particular trigger
    handle: float = 0.0           # straight knuckles, curled tips around a handguard/handle
    thumb_opposition: float = 0.0 # degrees across the palm, independent of thumb flexion


def lerp_grip(a: Grip, b: Grip, t: float) -> Grip:
    def mix(x, y, fallback_x, fallback_y):
        x = fallback_x if x is None else x
        y = fallback_y if y is None else y
        return x + (y - x) * t
    index_joints = a.index_joints if b.index_joints is None else b.index_joints
    if a.index_joints is not None and b.index_joints is not None:
        index_joints = v_lerp(a.index_joints, b.index_joints, t)
    return Grip(a.curl + (b.curl - a.curl) * t,
                mix(a.trigger, b.trigger, a.curl, b.curl),
                mix(a.thumb, b.thumb, a.curl, b.curl),
                a.spread + (b.spread - a.spread) * t, index_joints,
                a.handle + (b.handle - a.handle) * t,
                a.thumb_opposition + (b.thumb_opposition - a.thumb_opposition) * t)


# Joint angles of a full curl, base to tip, in degrees; each finger a little
# more than the one before it, as a real hand closes.
_CURL = {"Index": (62.0, 85.0, 50.0), "Middle": (70.0, 92.0, 52.0),
         "Ring": (76.0, 95.0, 55.0), "Small": (82.0, 98.0, 58.0)}
# A weapon handle is held against the palm with the proximal phalanges almost
# straight. Closing those first joints like a fist pulls the remaining fingers
# off the front of the handle. The last two joints wrap around its opposite side.
_HANDLE_CURL = {"Index": (20.0, 105.0, 78.0), "Middle": (18.0, 112.0, 80.0),
                "Ring": (20.0, 114.0, 82.0), "Small": (22.0, 116.0, 84.0)}
_SPREAD = {"Index": 7.0, "Middle": 1.5, "Ring": -4.0, "Small": -9.0}


def hand_rotations(skeleton, arm, grip: Grip, handle: bool = False):
    """Finger and thumb rotations of one hand."""
    side = ARMS[arm]["side"]
    out = {}
    # Fingers point along the arm (+X left, -X right); they close toward the
    # palm (-Y): a turn about Z, of opposite sign on the two hands.
    for finger in FINGERS:
        amount = grip.curl if not (finger == "Index" and grip.trigger is not None) else grip.trigger
        # A support hand cradles a handguard across its palm. Its first
        # phalanges stay nearly straight while the distal joints wrap the far
        # side. The rounded magazine/fist profile bends the first joints too
        # early and lifts the fingertips through the sights. Blend this fit
        # continuously when leaving the support grip to reload.
        angles = (_HANDLE_CURL[finger] if handle else
                  v_lerp(_CURL[finger], _HANDLE_CURL[finger], grip.handle))
        if finger == "Index" and grip.index_joints is not None:
            angles, amount = grip.index_joints, 1.0
        names = (f"Finger{finger}{arm}", f"Finger{finger}2{arm}", f"Finger{finger}3{arm}")
        for joint, (name, angle) in enumerate(zip(names, angles)):
            q = q_axis_angle(Z, -side * angle * amount)
            if joint == 0 and grip.spread:
                q = q_mul(q_axis_angle(Y, side * _SPREAD[finger] * grip.spread), q)
            out[name] = q
    thumb = grip.curl if grip.thumb is None else grip.thumb
    # The thumb swings across the palm toward the fingers' side.
    base = skeleton.offset(f"FingerThumb2{arm}")
    direction = v_norm(base)
    across = v_norm(v_cross(direction, (0.0, -0.75, -0.65)))
    out[f"FingerThumb{arm}"] = q_axis_angle(across, 28.0 * thumb)
    if grip.thumb_opposition:
        out[f"FingerThumb{arm}"] = q_mul(
            q_axis_angle(Y, side * grip.thumb_opposition), out[f"FingerThumb{arm}"])
    out[f"FingerThumb2{arm}"] = q_axis_angle(across, 34.0 * thumb)
    out[f"FingerThumb3{arm}"] = q_axis_angle(across, 30.0 * thumb)
    return out


# ---------------------------------------------------------------------------
# A pose: where the gun and the hands are, and how the body carries them
# ---------------------------------------------------------------------------

# The point a closed hand holds things at, in the wrist's own frame (the palm,
# between the heel of the hand and the knuckles). The hand's own axes: the
# fingers run along +X on the left hand and -X on the right, the palm faces -Y,
# the thumb is on +Z.
PALM = {"Left": (0.062, -0.042, 0.004), "Right": (-0.062, -0.042, 0.004)}


def euler(yaw=0.0, pitch=0.0, roll=0.0):
    """Degrees: yaw turns toward the character's left (+Y), pitch nods down (+X),
    roll tips the top toward the character's right (+Z)."""
    return q_mul(q_mul(q_axis_angle(Y, yaw), q_axis_angle(X, pitch)), q_axis_angle(Z, roll))


def hand_frame(fingers, palm_normal):
    """A hand's orientation from where its fingers point and where its palm faces
    (both in the space the frame is placed in); the left hand's convention, so the
    right hand passes its fingers negated."""
    x = v_norm(fingers)
    y = v_norm(v_sub(v_scale(palm_normal, -1.0), v_scale(x, v_dot(v_scale(palm_normal, -1.0), x))))
    return q_from_basis(x, y, v_cross(x, y))


def wrist_cone(rotation: tuple, prior: tuple | None = None, limit: float = 55.0) -> tuple:
    """Limit flexion and pronation, including the twist-helper half-turn seam."""
    twist = q_twist(rotation, X)
    swing = q_norm(q_mul(rotation, q_inv(twist)))
    angle = q_angle(swing)
    turn = math.degrees(2.0 * math.atan2(twist[0], twist[3]))
    turn = (turn + 180.0) % 360.0 - 180.0
    if prior is not None:
        previous = q_twist(prior, X)
        previous_turn = math.degrees(2.0 * math.atan2(previous[0], previous[3]))
        previous_turn = (previous_turn + 180.0) % 360.0 - 180.0
        turn += 360.0 * round((previous_turn - turn) / 360.0)
        # Baked at 30 Hz. A contact crossing the twist branch cut must not
        # change a sleeve's roll by half a turn in one frame.
        turn = max(previous_turn - 12.0, min(previous_turn + 12.0, turn))
    twist = q_axis_angle(X, max(-90.0, min(90.0, turn)))
    if angle > limit:
        swing = q_slerp(IDENTITY, swing, limit / angle)
    return q_norm(q_mul(swing, twist))


@dataclass
class Pose:
    """One instant of a clip. Angles are degrees, offsets metres."""
    gun: Frame                       # the gun model in the steady view's space
    left: Frame | None = None        # left palm (hand_frame axes) in the gun's model space ...
    left_space: str = "gun"          # ... or "view" (steady view) or "body" (BackUpper)
    torso: tuple = (0.0, 0.0, 0.0)   # BackUpper yaw, pitch, roll
    neck: tuple = (0.0, 0.0, 0.0)    # neck and head as the gun is held: the steady view
    head: tuple = (0.0, 0.0, 0.0)
    look: tuple = (0.0, 0.0, 0.0)    # the head turned beyond that: moves the camera over the gun
    collar_left: tuple = (0.0, 0.0)  # raise, reach forward
    collar_right: tuple = (0.0, 0.0)
    left_grip: Grip = field(default_factory=Grip)
    right_grip: Grip = field(default_factory=lambda: Grip(0.85, trigger=0.35, thumb=0.8))
    right_handle: bool = True       # deep distal curl around a handle; grenades use a rounded grip
    left_elbow: float = 10.0         # preferred elbow swivel (0 down, + out)
    right_elbow: float = 25.0
    prop_right: tuple = IDENTITY    # object orientation within the gripping hand


@dataclass
class Solved:
    rotations: dict
    frames: dict
    camera: Frame
    view: Frame          # the steady view the gun is placed in
    gun: Frame           # the gun model in the world (authoring space)
    swivels: tuple


def solve_pose(skeleton, pose: Pose, previous: Solved | None = None) -> Solved:
    rotations = {}
    collar_l = q_mul(q_axis_angle(Z, pose.collar_left[0]), q_axis_angle(Y, -pose.collar_left[1]))
    collar_r = q_mul(q_axis_angle(Z, -pose.collar_right[0]), q_axis_angle(Y, pose.collar_right[1]))
    rotations["BackUpper"] = euler(*pose.torso)
    rotations["CollarLeft"] = collar_l
    rotations["CollarRight"] = collar_r

    # The steady view: the camera with the head held as the gun is held. The
    # gun is placed in it, so `look` moves the first-person camera over the gun
    # (the "camera feel" of a reload) and the torso carries both.
    rotations["Neck"] = euler(*pose.neck)
    rotations["Head"] = euler(*pose.head)
    steady = skeleton.world(rotations)
    view = camera_frame(steady["Head"])
    rotations["Head"] = q_mul(euler(*pose.head), euler(*pose.look))
    frames = skeleton.world(rotations)
    camera = camera_frame(frames["Head"])

    gun = view.compose(pose.gun)
    rotations["PropRight"] = pose.prop_right
    rotations["PropLeft"] = IDENTITY
    prop = skeleton.offset("PropRight")
    wrist_orientation = q_mul(gun.q, q_inv(pose.prop_right))
    right_wrist = Frame(v_sub(gun.p, q_rotate(wrist_orientation, prop)), wrist_orientation)
    hint_r = previous.swivels[1] if previous else None
    hint_l = previous.swivels[0] if previous else None
    swivel_r = solve_arm(skeleton, frames, rotations, "Right", right_wrist, hint_r, pose.right_elbow)

    if pose.left is None:
        palm = Frame(gun.apply((-0.30, 0.12, 0.02)), gun.q)
    elif pose.left_space == "gun":
        palm = gun.compose(pose.left)
    elif pose.left_space == "view":
        palm = view.compose(pose.left)
    else:
        palm = frames["BackUpper"].compose(pose.left)
    left_wrist = palm.compose(Frame(v_scale(PALM["Left"], -1.0), IDENTITY))
    # Keep the palm's contact while relaxing an impossible wrist angle. A
    # six-pass positional solve lets the elbow share the bend. This is baked
    # into the clip, not a runtime correction or a change to weapon handling.
    for _ in range(6):
        swivel_l = solve_arm(skeleton, frames, rotations, "Left", left_wrist, hint_l, pose.left_elbow)
        limited = wrist_cone(rotations["WristLeft"], previous.rotations["WristLeft"] if previous else None)
        if q_angle(q_mul(limited, q_inv(rotations["WristLeft"]))) < .05:
            break
        orientation = q_norm(q_mul(frames["ElbowLeft"].q, limited))
        left_wrist = Frame(v_sub(palm.p, q_rotate(orientation, PALM["Left"])), orientation)
    rotations["WristLeft"] = wrist_cone(rotations["WristLeft"], previous.rotations["WristLeft"] if previous else None)
    rotations[ARMS["Left"]["fore_twist"]] = q_power(q_twist(rotations["WristLeft"], X), FOREARM_TWIST_SHARE)

    rotations.update(hand_rotations(skeleton, "Left", pose.left_grip))
    rotations.update(hand_rotations(skeleton, "Right", pose.right_grip, handle=pose.right_handle))
    frames = skeleton.world(rotations)
    return Solved(rotations, frames, camera, view, gun, (swivel_l, swivel_r))


# ---------------------------------------------------------------------------
# Holding a gun up to the eye, and moving it about a point of its own
# ---------------------------------------------------------------------------

def view_turn(yaw=0.0, pitch=0.0, roll=0.0):
    """A turn in the view's space (degrees): yaw swings the muzzle left, pitch
    raises it, roll tips the top of the gun to the left (counter-clockwise)."""
    return q_mul(q_mul(q_axis_angle(Y, yaw), q_axis_angle(X, pitch)), q_axis_angle(Z, roll))


def aimed(sight_eye, sight_forward, sight_up, relief=0.0):
    """The gun with its sight line on the view's axis: the model point
    `sight_eye` lands `relief` metres in front of the eye (negative: behind)."""
    f = v_norm(sight_forward)
    u = v_norm(v_sub(sight_up, v_scale(f, v_dot(sight_up, f))))
    right = v_cross(f, u)
    q = q_inv(q_from_basis(right, u, v_scale(f, -1.0)))
    return Frame(v_sub((0.0, 0.0, -relief), q_rotate(q, sight_eye)), q)


def moved(frame: Frame, pivot, offset=(0.0, 0.0, 0.0), turn=IDENTITY) -> Frame:
    """`frame` (model -> view) turned by `turn` about the model point `pivot`,
    then shifted by `offset` in the view's space."""
    pivot_view = frame.apply(pivot)
    q = q_norm(q_mul(turn, frame.q))
    p = v_sub(pivot_view, q_rotate(q, pivot))
    return Frame(v_add(p, offset), q)


# ---------------------------------------------------------------------------
# Timing: eased keys
# ---------------------------------------------------------------------------

def ease(kind, t):
    """Easing curves. `back` and `settle` overshoot the target (anticipation /
    follow-through); `snap` is a fast start that lands softly (a hand slapping
    something home)."""
    t = min(max(t, 0.0), 1.0)
    if kind == "linear":
        return t
    if kind == "in":
        return t * t * t
    if kind == "out":
        return 1.0 - (1.0 - t) ** 3
    if kind == "inout":
        return t * t * (3.0 - 2.0 * t)
    if kind == "smooth":
        return t * t * t * (t * (6.0 * t - 15.0) + 10.0)
    if kind == "snap":
        return 1.0 - (1.0 - t) ** 5
    if kind == "back":       # overshoots by ~10 % and comes back
        s = 1.70158
        u = t - 1.0
        return 1.0 + u * u * ((s + 1.0) * u + s)
    if kind == "anticipate":  # dips back first, then goes
        s = 1.2
        return t * t * ((s + 1.0) * t - s)
    if kind == "settle":     # a damped spring: overshoot, then rings out
        return 1.0 - math.exp(-6.5 * t) * math.cos(9.0 * t) * (1.0 - t)
    if kind == "hold":
        return 0.0
    raise ValueError(kind)


def lerp_value(a, b, t):
    if isinstance(a, (int, float)):
        return a + (b - a) * t
    if isinstance(a, Grip):
        return lerp_grip(a, b, t)
    if isinstance(a, Frame):
        return lerp_frame(a, b, t)
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def continuous_curve(times, values, time):
    """Shape-preserving cubic motion through keys, with stationary endpoints.

    Weighted harmonic tangents carry velocity through a travelling key, stop
    at reversals/holds, and cannot overshoot a contact. Unlike easing every
    segment independently, fetching a magazine is one continuous gesture.
    """
    if time <= times[0]: return values[0]
    if time >= times[-1]: return values[-1]
    i = next(k for k in range(len(times)-1) if times[k+1] > time)
    h = times[i+1]-times[i]
    u = (time-times[i])/h
    def tangent(k, axis):
        if k == 0 or k == len(times)-1: return 0.0
        before, after = times[k]-times[k-1], times[k+1]-times[k]
        a = (values[k][axis]-values[k-1][axis])/before
        b = (values[k+1][axis]-values[k][axis])/after
        if a*b <= 0: return 0.0
        w1, w2 = 2*after+before, after+2*before
        return (w1+w2)/(w1/a+w2/b)
    return tuple((2*u**3-3*u*u+1)*values[i][a] + (u**3-2*u*u+u)*h*tangent(i,a)
                 + (-2*u**3+3*u*u)*values[i+1][a] + (u**3-u*u)*h*tangent(i+1,a)
                 for a in range(len(values[0])))


class Track:
    """Keys (time, value, easing into it); values are numbers, tuples, Frames or Grips."""

    def __init__(self, value=None):
        self.keys = []
        self.flow = False
        if value is not None:
            self.keys.append((0.0, value, "linear"))

    def key(self, time, value, easing="inout"):
        # A contact at the same instant replaces the previous pose. Duplicate
        # timestamps otherwise make frame zero differ from its right-hand limit.
        self.keys = [key for key in self.keys if abs(key[0] - float(time)) > 1e-8]
        self.keys.append((float(time), value, easing))
        self.keys.sort(key=lambda key: key[0])
        return self

    def hold(self, time):
        """Stay at the previous key's value until `time`."""
        return self.key(time, self.value(time), "linear")

    def segment(self, time):
        keys = self.keys
        if time <= keys[0][0]:
            return keys[0], keys[0], 0.0
        for index in range(1, len(keys)):
            if time < keys[index][0]:
                first, second = keys[index - 1], keys[index]
                span = second[0] - first[0]
                return first, second, ease(second[2], (time - first[0]) / span if span > 0 else 1.0)
        return keys[-1], keys[-1], 0.0

    def value(self, time):
        if self.flow and len(self.keys) > 1:
            return continuous_curve([k[0] for k in self.keys], [k[1] for k in self.keys], time)
        first, second, t = self.segment(time)
        if first is second:
            return first[1]
        return lerp_value(first[1], second[1], t)


# ---------------------------------------------------------------------------
# A clip: tracks for the gun, the support hand, the fingers, the body, parts
# and the cues fired along the way
# ---------------------------------------------------------------------------

@dataclass
class HandAt:
    """Where the support (left) hand's palm is: a frame in the gun's model
    space ("gun") or in the steady view ("view"). `arc` bows the path that
    arrives here (view space, metres at mid-way)."""
    frame: Frame
    space: str = "gun"
    arc: tuple = (0.0, 0.0, 0.0)


@dataclass
class Cue:
    time: float
    action: dict


class Clip:
    def __init__(self, family, name, length, base: Frame, pivot):
        self.family = family
        self.name = name
        self.length = length
        self.base = base          # the gun in the steady view at offset zero
        self.pivot = pivot        # model point the gun turns about
        self.gun = Track((0.0, 0.0, 0.0, 0.0, 0.0, 0.0))   # dx, dy, dz, yaw, pitch, roll
        self.left = Track()
        self.left_grip = Track()
        self.right_grip = Track(Grip(0.85, trigger=0.35, thumb=0.8))
        self.torso = Track((0.0, 0.0, 0.0))
        self.neck = Track((0.0, 0.0, 0.0))
        self.head = Track((0.0, 0.0, 0.0))
        self.look = Track((0.0, 0.0, 0.0))
        self.collar_left = Track((0.0, 0.0))
        self.collar_right = Track((0.0, 0.0))
        self.elbows = Track((10.0, 25.0))
        self.parts = {}           # part name -> Track of Frames (model space, about its pivot)
        self.part_follow = {}     # part name -> (in-hand frame, from, to, pivot): rides the left hand
        self.cues = []
        self.looping = False
        self.match_end_pose = False

    def cue(self, time, **action):
        self.cues.append(Cue(float(time), action))

    def part(self, name):
        if name not in self.parts:
            self.parts[name] = Track(Frame())
        return self.parts[name]

    def gun_frame(self, time):
        dx, dy, dz, yaw, pitch, roll = self.gun.value(time)
        return moved(self.base, self.pivot, (dx, dy, dz), view_turn(yaw, pitch, roll))

    def left_frame(self, time, gun: Frame):
        """The left palm in the steady view's space."""
        first, second, t = self.left.segment(time)
        a, b = first[1], second[1]
        if self.left.flow and len(self.left.keys) > 1:
            # Transform at the current weapon frame so a held contact stays
            # locked to its moving socket, including between authored keys.
            values = [gun.compose(k[1].frame) if k[1].space == "gun" else k[1].frame for k in self.left.keys]
            times = [k[0] for k in self.left.keys]
            rotations = []
            for value in values:
                q = value.q
                if rotations and sum(x*y for x,y in zip(rotations[-1],q)) < 0: q = tuple(-v for v in q)
                rotations.append(q)
            frame = Frame(continuous_curve(times, [v.p for v in values], time),
                          q_norm(continuous_curve(times, rotations, time)))
        elif a.space == "gun" and b.space == "gun":
            local = lerp_frame(a.frame, b.frame, t)
            frame = gun.compose(local)
        else:
            fa = gun.compose(a.frame) if a.space == "gun" else a.frame
            fb = gun.compose(b.frame) if b.space == "gun" else b.frame
            frame = lerp_frame(fa, fb, t)
        if first is not second and any(b.arc):
            # A clearance arc must arrive with zero extra velocity.
            bow = 16.0*t*t*(1.0-t)*(1.0-t)
            frame = Frame(v_add(frame.p, v_scale(b.arc, bow)), frame.q)
        return frame

    def pose(self, time):
        gun = self.gun_frame(time)
        left = self.left_frame(time, gun)
        elbow_l, elbow_r = self.elbows.value(time)
        return Pose(gun=gun, left=left, left_space="view", torso=self.torso.value(time),
                    neck=self.neck.value(time), head=self.head.value(time), look=self.look.value(time),
                    collar_left=self.collar_left.value(time), collar_right=self.collar_right.value(time),
                    left_grip=self.left_grip.value(time), right_grip=self.right_grip.value(time),
                    left_elbow=elbow_l, right_elbow=elbow_r, prop_right=self.family.stance.prop_right,
                    right_handle=self.family.gun.model != "grenade")


def frame_count(length):
    """Keys every 1/30 s, the last one exactly at the clip's end."""
    return max(2, int(math.ceil(length * FRAME_RATE - 1e-6)) + 1)


def evaluate(skeleton, clip: Clip):
    """Solves the clip at 30 Hz: per frame the bone rotations, the gun and the
    parts' model-space transforms."""
    poses = []
    previous = None
    elbow_endpoints = None
    if clip.match_end_pose:
        elbow_endpoints = (solve_pose(skeleton, clip.pose(0.0)).swivels,
                           solve_pose(skeleton, clip.pose(clip.length)).swivels)
    for index in range(frame_count(clip.length)):
        time = min(clip.length, index / FRAME_RATE)
        hint = previous
        if previous is not None and elbow_endpoints is not None:
            blend = ease("inout", time / clip.length)
            start, end = elbow_endpoints
            # Give the short raise an endpoint-aware elbow path. Pure previous-
            # frame damping leaves the elbow several centimetres from the idle
            # pose and produces a visible pop when that next clip begins.
            hint = replace(previous, swivels=tuple(a + (b - a) * blend for a, b in zip(start, end)))
        solved = solve_pose(skeleton, clip.pose(time), hint)
        if clip.match_end_pose and index == frame_count(clip.length) - 1:
            # This sample is also the next idle clip's first pose. Previous-
            # frame wrist relaxation can otherwise leave a small but visible
            # snap even when the elbow endpoint already matches.
            solved = solve_pose(skeleton, clip.pose(time))
        poses.append((time, solved))
        previous = solved
    # Calibrate carried props to the constrained wrist at their contact frame.
    # A fresh part seats precisely even when the anatomical solve relaxed the
    # artist's palm orientation; an old part starts exactly in its socket.
    follows = {}
    for name, (in_hand, start, end, pivot) in clip.part_follow.items():
        contact = end if name.endswith("_new") or name in ("shell", "clip") else start
        before = max((pair for pair in poses if pair[0] <= contact+1e-8), key=lambda pair: pair[0])
        after = min((pair for pair in poses if pair[0] >= contact-1e-8), key=lambda pair: pair[0])
        blend = (contact-before[0])/(after[0]-before[0]) if after[0]>before[0] else 0.0
        rotations = {bone:q_slerp(before[1].rotations.get(bone,IDENTITY),
                                  after[1].rotations.get(bone,IDENTITY),blend) for bone in skeleton.names}
        contact_frames = skeleton.world(rotations)
        # Calibrate against the actual socket, not the old guessed palm frame.
        # Editing a contact must not make the magazine pop away from the gun.
        grip = contact_frames["WristLeft"].inverse().compose(contact_frames["PropRight"].compose(Frame(pivot, IDENTITY)))
        # The runtime samples both bones and part tracks at 30 Hz, then takes
        # a bone-local grip at the cue's exact (often fractional-frame) time.
        # A missing part key becomes identity on export. Starting a fresh
        # part's track only at `start` therefore blended its pouch pose with
        # its socket pose, permanently attaching it at the wrong offset.
        # Hidden fresh parts can follow the wrist before they are shown. An
        # old part needs the preceding sample as well so interpolation across
        # its attach cue uses two carried poses. Keep the next sample beyond
        # seating too; the detach cue controls when it returns to the gun.
        fresh = name.endswith("_new") or name in ("shell", "clip")
        sample_start = 0.0 if fresh else math.floor(start * FRAME_RATE + 1e-8) / FRAME_RATE
        sample_end = math.ceil(end * FRAME_RATE - 1e-8) / FRAME_RATE
        follows[name] = (grip, sample_start, sample_end, pivot)
    frames = []
    for time, solved in poses:
        parts = {name: track.value(time) for name, track in clip.parts.items()}
        for name, (in_hand, start, end, pivot) in follows.items():
            if start <= time <= end:
                # The part sits in the left hand: its model-space motion is the
                # hand's place relative to the gun, times its grip in the hand.
                held = solved.gun.inverse().compose(solved.frames["WristLeft"].compose(in_hand))
                parts[name] = Frame(v_sub(held.p, pivot), held.q)
        frames.append((time, solved, parts))
    return frames


def grab_frame(skeleton, clip: Clip, time, pivot):
    """How a part resting at `pivot` sits in the left hand when the hand closes
    on it at `time` (for Clip.part_follow): the part's rest frame seen from the wrist."""
    solved = solve_pose(skeleton, clip.pose(time))
    home = solved.gun.compose(Frame(pivot, IDENTITY))
    return solved.frames["WristLeft"].inverse().compose(home)


# ---------------------------------------------------------------------------
# glTF output
# ---------------------------------------------------------------------------

def reduce_keys(times, values, rotation, tolerance_degrees=0.04, tolerance_metres=0.0002):
    """Drops keys that linear interpolation between their neighbours already
    gives (the game resamples a clip at 30 Hz anyway), so still bones and
    straight stretches cost two keys."""
    def close(a, b):
        if rotation:
            dot = min(1.0, abs(sum(x * y for x, y in zip(a, b))))
            return math.degrees(2.0 * math.acos(dot)) <= tolerance_degrees
        return v_len(v_sub(a, b)) <= tolerance_metres

    def between(a, b, t):
        return q_slerp(a, b, t) if rotation else v_lerp(a, b, t)

    kept = [0]
    anchor = 0
    index = 2
    while index < len(values):
        span = times[index] - times[anchor]
        if not all(close(between(values[anchor], values[index], (times[k] - times[anchor]) / span), values[k])
                   for k in range(anchor + 1, index)):
            anchor = index - 1
            kept.append(anchor)
        index += 1
    if kept[-1] != len(values) - 1:
        kept.append(len(values) - 1)
    return [times[k] for k in kept], [values[k] for k in kept]


def upper_body(skeleton):
    """BackUpper and every bone below it: what an arm or gun clip writes."""
    out = []
    stack = ["BackUpper"]
    while stack:
        name = stack.pop()
        out.append(name)
        stack.extend(reversed(skeleton.children[name]))
    return [name for name in skeleton.names if name in set(out)]


def write_clips(path, skeleton, clips, part_names, note):
    """One glTF file holding several clips: the bones' rotations and the parts' tracks."""
    names = list(skeleton.names)
    index = {bone: position for position, bone in enumerate(names)}
    nodes = []
    for bone in names:
        node = {"name": skeleton.source_names.get(bone, bone)}
        children = [index[child] for child in skeleton.children[bone]]
        if children:
            node["children"] = children
        nodes.append(node)
    part_index = {}
    for part in part_names:
        part_index[part] = len(nodes)
        nodes.append({"name": part})
    buffer = bytearray()
    views, accessors, animations = [], [], []

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

    def rounded(values):
        return [tuple(round(c, 6) for c in v) for v in values]

    def canonical(quats):
        # Keep neighbouring keys on the same hemisphere so slerp takes the short way.
        out = []
        for q in quats:
            if out and sum(a * b for a, b in zip(out[-1], q)) < 0.0:
                q = tuple(-c for c in q)
            out.append(q)
        return out

    bones = upper_body(skeleton)
    for clip, frames in clips:
        times = [time for time, _, _ in frames]
        time_accessors = {}
        samplers, channels = [], []

        def channel(node, path, keys, values):
            kept_times, kept = reduce_keys(keys, values, path == "rotation")
            stamp = tuple(kept_times)
            if stamp not in time_accessors:
                time_accessors[stamp] = add(list(kept_times), "SCALAR", 0.0, kept_times[-1])
            samplers.append({"input": time_accessors[stamp], "output": add(rounded(kept), "VEC4" if path == "rotation"
                                                                          else "VEC3"), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})

        # These retail slots only drive the right arm. Exporting a solved
        # torso/left arm here would be ignored and misleading to pack authors.
        base_name = clip.name.split("@")[0]
        clip_bones = bones
        if base_name.startswith("Pick") or base_name in ("GrenadeIdle", "GrenadeWalk"):
            clip_bones = []
            for bone in bones:
                ancestor = bone
                while ancestor is not None and ancestor != "CollarRight":
                    ancestor = skeleton.parent(ancestor)
                if ancestor is not None:
                    clip_bones.append(bone)
        for bone in clip_bones:
            channel(index[bone], "rotation", times,
                    canonical([solved.rotations.get(bone, IDENTITY) for _, solved, _ in frames]))
        for part in part_names:
            moving = [parts.get(part) for _, _, parts in frames]
            if all(frame is None for frame in moving):
                continue
            moving = [frame if frame is not None else Frame() for frame in moving]
            channel(part_index[part], "translation", times, [f.p for f in moving])
            channel(part_index[part], "rotation", times, canonical([f.q for f in moving]))
        animations.append({"name": clip.name, "channels": channels, "samplers": samplers})
    document = {
        "asset": {"version": "2.0", "generator": "OpenMinerZ tools/ModSdk/make_tactical_animations.py",
                  "copyright": "Original animation, OpenMinerZ examples", "extras": {"openminerz": note}},
        "scene": 0,
        "scenes": [{"nodes": [index["Root"]] + [part_index[p] for p in part_names]}],
        "nodes": nodes,
        "animations": animations,
        "buffers": [{"byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(binary_gltf(document, buffer))
    return path.stat().st_size


def binary_gltf(document, buffer):
    """A .glb: the JSON chunk and the binary chunk, each padded to 4 bytes."""
    text = json.dumps(document, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    data = bytes(buffer) + b"\0" * (-len(buffer) % 4)
    total = 12 + 8 + len(text) + 8 + len(data)
    return (struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(text), 0x4E4F534A) + text +
            struct.pack("<II", len(data), 0x004E4942) + data)


# ---------------------------------------------------------------------------
# Guns: where their sights, balance point, butt, grips and parts are
# ---------------------------------------------------------------------------
# Every number here is measured on the models in their own space (barrel toward
# -X, up +Z, the gun's right side +Y; the origin is where the right hand's prop
# bone holds it): the sight line runs through the rear sight and the front
# post, `pivot` is about where the gun balances, `butt` the middle of the butt
# plate (or the back of the frame), `support` the left palm on the handguard.

@dataclass
class Gun:
    model: str
    sight_eye: tuple            # a point on the sight line behind the rear sight (the eye)
    sight_ahead: tuple          # a point on the sight line in front of it
    sight_up: tuple             # the gun's up direction about that line
    pivot: tuple
    butt: tuple
    support: Frame              # left palm on the handguard (or the off-hand grip of a pistol)
    relief: float = 0.0         # aimed: how much closer to the eye than the measured eye point

    def aimed(self):
        return aimed(self.sight_eye, v_sub(self.sight_ahead, self.sight_eye), self.sight_up, self.relief)


def palm(point, fingers, palm_normal):
    """A left-palm frame in a gun's model space."""
    return Frame(point, hand_frame(fingers, palm_normal))


GUNS = {
    "modern/ak": Gun("modern/ak", (0.043, 0.104, 0.152), (-0.452, 0.148, 0.095), (-0.114, -0.010, 0.993),
              pivot=(-0.22, 0.13, 0.04), butt=(0.235, 0.125, 0.06),
              support=palm((-0.320, 0.115, 0.055), (-0.6, 1.0, 0.2), (0.0, -0.2, 1.0)),
              relief=0.08),
    # The off hand of a two-handed pistol grip wraps the right hand's fingers.
    "pistol": Gun("pistol", (0.278, 0.1145, 0.1205), (-0.222, 0.1063, 0.1239), (0.0064, 0.0191, 0.9998),
                  pivot=(-0.06, 0.112, 0.06), butt=(0.035, 0.112, 0.08),
                  support=palm((-0.004, 0.068, -0.03), (-0.45, 0.55, -0.7), (0.15, 0.95, 0.25))),
    "smg": Gun("smg", (0.275, 0.1185, 0.1237), (-0.225, 0.1185, 0.1237), (0.0, 0.0363, 0.9993),
               pivot=(-0.06, 0.115, 0.06), butt=(0.07, 0.115, 0.095),
               support=palm((-0.26, 0.114, 0.052), (0.05, 1.0, 0.55), (0.0, -0.25, 1.0))),
    "rifle": Gun("rifle", (0.1406, 0.1114, 0.0429), (-0.3314, 0.0828, 0.2052), (0.3291, -0.106, 0.9383),
                 pivot=(-0.16, 0.1, 0.12), butt=(0.165, 0.105, -0.055),
                 support=palm((-0.255, 0.095, 0.120), (-1.4, 1.0, 0.2), (0.3291, -0.5, 0.9383))),
    "classic/ak": Gun("classic/ak", (0.1319, 0.1076, 0.1308), (-0.3681, 0.1076, 0.1308), (0.0, -0.1736, 0.9848),
                      pivot=(-0.19, 0.13, 0.06), butt=(0.185, 0.13, 0.04),
                      support=palm((-0.280, 0.118, 0.080), (-0.9, 1.0, 0.5), (0.0, 0.0, 1.0)),
                      relief=0.08),
    "classic/shotgun": Gun("classic/shotgun", (0.13, 0.125, 0.135), (-0.37, 0.125, 0.123), (0.0, 0.0, 1.0),
                           pivot=(-0.2, 0.125, 0.07), butt=(0.205, 0.125, 0.06),
                           support=palm((-0.290, 0.125, 0.060), (-1.0, 1.0, 0.2), (0.0, -0.2, 1.0))),
    "modern/shotgun": Gun("modern/shotgun", (0.0595, 0.0966, 0.1594), (-0.4213, 0.1653, 0.0404), (-0.2393, 0.0082, 0.9709),
                   pivot=(-0.2, 0.14, 0.04), butt=(0.21, 0.125, 0.07),
                   support=palm((-0.325, 0.155, 0.010), (-1.0, 1.0, 0.2), (-0.2393, -0.2, 0.9709))),
    "lmg": Gun("lmg", (0.0524, 0.0892, 0.18), (-0.4403, 0.1571, 0.1295), (-0.1033, -0.0107, 0.9946),
               pivot=(-0.15, 0.13, 0.06), butt=(0.335, 0.075, 0.1),
               support=palm((-0.310, 0.140, 0.055), (-1.0, 1.0, 0.2), (-0.1033, -0.2, 0.9946))),
    "rpg": Gun("rpg", (0.198, 0.1, 0.199), (-0.298, 0.1, 0.131), (-0.135, 0.0, 0.991),
               pivot=(-0.1, 0.1, 0.08), butt=(0.4, 0.1, 0.1),
               support=palm((-0.19, 0.072, -0.038), (-1.0, 0.25, -0.3), (0.0, 1.0, 0.0))),
    "space_ak": Gun("space_ak", (0.1416, 0.1589, 0.1724), (-0.3569, 0.1244, 0.1544), (-0.0566, 0.3245, 0.9442),
                    pivot=(-0.18, 0.1, 0.05), butt=(0.15, 0.1, 0.07),
                    support=palm((-0.245, 0.110, 0.055), (-1.0, 1.0, 0.2), (-0.0566, -0.2, 0.9442))),
    "space_rifle": Gun("space_rifle", (0.1416, 0.1589, 0.1724), (-0.3569, 0.1244, 0.1544),
                       (-0.0566, 0.3245, 0.9442), pivot=(-0.2, 0.1, 0.06), butt=(0.14, 0.1, 0.08),
                       support=palm((-0.245, 0.108, 0.068), (-1.0, 1.0, 0.2), (-0.0566, -0.2, 0.9442))),
    "space_pistol": Gun("space_pistol", (0.2245, 0.1441, 0.1439), (-0.274, 0.1097, 0.1259),
                        (-0.0465, 0.1557, 0.9867), pivot=(-0.12, 0.11, 0.06), butt=(0.02, 0.11, 0.09),
                        support=palm((-0.03, 0.062, -0.03), (-0.45, 0.55, -0.7), (0.15, 0.95, 0.25))),
    "space_smg": Gun("space_smg", (0.2245, 0.1441, 0.1439), (-0.274, 0.1097, 0.1259), (-0.0465, 0.1557, 0.9867),
                     pivot=(-0.15, 0.1, 0.06), butt=(0.015, 0.1, 0.09),
                     support=palm((-0.245, 0.108, 0.055), (-1.0, 1.0, 0.2), (-0.0465, -0.2, 0.9867))),
    "space_shotgun": Gun("space_shotgun", (0.1416, 0.1589, 0.1724), (-0.3569, 0.1244, 0.1544),
                         (-0.0566, 0.3245, 0.9442), pivot=(-0.2, 0.1, 0.06), butt=(0.15, 0.1, 0.08),
                         support=palm((-0.215, 0.108, 0.055), (-1.0, 1.0, 0.2), (-0.0566, -0.2, 0.9442))),
}


# ---------------------------------------------------------------------------
# Families: how each class of gun is carried, and its clips
# ---------------------------------------------------------------------------

@dataclass
class Stance:
    """How a family carries its gun at the hip, relative to aiming it: the gun
    swung about its butt (or the back of a pistol's frame) and moved in the
    view, the torso bladed, the support shoulder reaching."""
    hip_offset: tuple = (0.12, -0.075, -0.02)
    hip_turn: tuple = (1.0, 4.0, 12.0)       # yaw (muzzle left), pitch (up), roll (top left)
    torso: tuple = (-30.0, 2.0, 0.0)
    aim_torso: tuple = (-26.0, 2.0, 0.0)
    collar_left: tuple = (3.0, 22.0)
    collar_right: tuple = (0.0, 0.0)
    aim_collar_left: tuple | None = None
    aim_collar_right: tuple | None = None
    elbows: tuple = (10.0, 25.0)
    aim_elbows: tuple = (5.0, 45.0)
    left_grip: Grip = field(default_factory=lambda: Grip(0.72, thumb=0.55))
    right_grip: Grip = field(default_factory=lambda: Grip(0.85, trigger=0.35, thumb=0.8))
    prop_right: tuple = IDENTITY


class Family:
    def __init__(self, key, gun: Gun, stance: Stance):
        self.key = key
        self.gun = gun
        if gun.model in ("rifle", "modern/shotgun", "classic/shotgun", "lmg", "space_ak",
                         "space_rifle", "space_smg", "space_shotgun"):
            # Cradle the fore-end with fingers crossing its width. The shallow
            # knuckle bend and deeper distal curl wrap the far side without
            # raising four fingertips through the sight line. The thumb
            # opposes the fingers against the near side, instead of sticking up.
            aim_yaw = {"rifle": 0.0, "lmg": 0.0, "space_ak": 0.0,
                       "space_rifle": 0.0, "space_shotgun": 20.0}.get(gun.model, -20.0)
            # Give the firing forearm room behind the grip while keeping each
            # shoulder on its own side of the chest. Compensate the hip offset
            # so this sight-distance correction leaves reloads and hip carries
            # at their existing world positions.
            aimed_fit = {
                "classic/shotgun": (0.03, -40.0, -20.0, -20.0),
                "lmg": (0.06, -20.0, -20.0, 0.0),
                "space_ak": (0.03, -20.0, -20.0, -10.0),
                "space_rifle": (0.03, -20.0, -20.0, -10.0),
                "space_smg": (0.0, -20.0, 5.0, -30.0),
                "space_shotgun": (0.06, -20.0, -20.0, 0.0),
            }.get(gun.model)
            left_reach = 5.0 if gun.model == "space_smg" else -20.0
            if aimed_fit is not None:
                relief, aim_yaw, left_reach, right_reach = aimed_fit
                self.gun = replace(gun, relief=gun.relief + relief)
                stance = replace(stance, hip_offset=v_add(stance.hip_offset, (0.0, 0.0, relief)),
                                 collar_right=(0.0, right_reach))
            stance = replace(stance, left_grip=Grip(0.80, thumb=0.0, handle=1.0, thumb_opposition=15.0),
                             elbows=(-95.0 if gun.model in ("space_ak", "space_rifle") else -50.0,
                                     stance.elbows[1]),
                             aim_elbows=(-100.0 if gun.model == "classic/shotgun" else -60.0,
                                         stance.aim_elbows[1]),
                             aim_torso=(aim_yaw, 2.0, 0.0),
                             aim_collar_left=(3.0, left_reach),
                             aim_collar_right=(0.0, -60.0))
        if gun.model in ("modern/ak", "classic/ak"):
            # Cradle the handguard across the palm. Keep the knuckles low and
            # curl the fingertips around its far side; a longitudinal fist
            # makes a C-shaped claw even when its virtual contact is exact.
            stance = replace(stance, left_grip=Grip(0.80, thumb=0.0, handle=1.0,
                                                   thumb_opposition=15.0),
                             elbows=(-30.0, stance.elbows[1]),
                             aim_elbows=(-30.0, stance.aim_elbows[1]),
                             torso=(-40.0, 2.0, 0.0) if gun.model == "classic/ak" else stance.torso,
                             collar_left=(0.0, 20.0) if gun.model == "classic/ak" else stance.collar_left,
                             aim_torso=(-40.0 if gun.model == "classic/ak" else -20.0, 2.0, 0.0),
                             aim_collar_left=(0.0, 0.0),
                             aim_collar_right=(0.0, -60.0 if gun.model == "classic/ak" else -65.0))
        if gun.model in GUNS:
            # The source thumb already rests alongside a weapon grip. A deep
            # fist curl pulled its tip sideways across empty space; keep only
            # a slight bend here, with explicit latch presses in reload clips.
            stance = replace(stance, right_grip=replace(stance.right_grip, thumb=0.10))
        self.stance = stance

    def ads(self):
        return self.gun.aimed()

    def hip(self):
        s = self.stance
        return moved(self.ads(), self.gun.butt, s.hip_offset, view_turn(*s.hip_turn))

    def hip_in_ads(self):
        """The hip hold as a gun-track key of a clip based on the aimed hold."""
        return relative_key(self.ads(), self.hip(), self.gun.pivot)

    def clip(self, name, length, aimed_base=False):
        s = self.stance
        clip = Clip(self, name, length, self.ads() if aimed_base else self.hip(), self.gun.pivot)
        torso = s.aim_torso if aimed_base else s.torso
        clip.torso = Track(torso)
        clip.neck = Track((-torso[0] * 0.5, -torso[1] * 0.5, -torso[2] * 0.5))
        clip.head = Track((-torso[0] * 0.5, -torso[1] * 0.5, -torso[2] * 0.5))
        clip.collar_left = Track(s.aim_collar_left if aimed_base and s.aim_collar_left is not None else s.collar_left)
        clip.collar_right = Track(s.aim_collar_right if aimed_base and s.aim_collar_right is not None else s.collar_right)
        clip.elbows = Track(s.aim_elbows if aimed_base else s.elbows)
        clip.left = Track(HandAt(self.gun.support))
        clip.left_grip = Track(s.left_grip)
        clip.right_grip = Track(s.right_grip)
        return clip


def relative_key(base: Frame, target: Frame, pivot):
    """The (dx, dy, dz, yaw, pitch, roll) key that moves `base` onto `target`
    (about `pivot`); exact for any turn (the angles are a yaw-pitch-roll split)."""
    turn = q_mul(target.q, q_inv(base.q))
    # Decompose turn = Ry(yaw) Rx(pitch) Rz(roll).
    x, y, z, w = turn
    m02 = 2 * (x * z + y * w)
    m12 = 2 * (y * z - x * w)
    m22 = 1 - 2 * (x * x + y * y)
    m10 = 2 * (x * y + z * w)
    m11 = 1 - 2 * (x * x + z * z)
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, -m12))))
    yaw = math.degrees(math.atan2(m02, m22))
    roll = math.degrees(math.atan2(m10, m11))
    rebuilt = moved(base, pivot, (0.0, 0.0, 0.0), view_turn(yaw, pitch, roll))
    offset = v_sub(target.apply(pivot), rebuilt.apply(pivot))
    return (offset[0], offset[1], offset[2], yaw, pitch, roll)


# The first-person view the clips are composed for: the game's viewmodel
# camera (90 degrees vertical) on a 16:9 screen.
VIEW_ASPECT = 16.0 / 9.0


def shown_at(family, point, screen, depth, turn):
    """A gun-track key (relative to the hip hold) that turns the gun by `turn`
    (yaw, pitch, roll) and puts its model point `point` where the eye sees it
    at `screen` (fractions of the width and height from the top left), `depth`
    metres ahead: how a reload presents the magazine well to the camera."""
    base = family.hip()
    pivot = family.gun.pivot
    target = (2.0 * (screen[0] - 0.5) * depth * VIEW_ASPECT, 2.0 * (0.5 - screen[1]) * depth, -depth)
    turned = moved(base, pivot, (0.0, 0.0, 0.0), view_turn(*turn))
    offset = v_sub(target, turned.apply(point))
    return (offset[0], offset[1], offset[2]) + tuple(turn)


def part_motion(pivot, offset=(0.0, 0.0, 0.0), turn=IDENTITY):
    """A part's model-space motion as the engine composes it: turned about its
    pivot, then shifted. As a Frame acting on model points."""
    return Frame(v_sub(v_add(pivot, offset), q_rotate(turn, pivot)), turn)


def track_value(motion: Frame, pivot):
    """The part-track key (translation, rotation) of a part motion Frame."""
    return Frame(v_sub(v_add(motion.p, q_rotate(motion.q, pivot)), pivot), motion.q)


def model_turn(tip_forward=0.0, swing_left=0.0, roll_right=0.0):
    """A turn of a part in a gun's model space (degrees): `tip_forward` swings
    its bottom toward the muzzle (about the gun's side axis +Y), `swing_left`
    turns it about the gun's up axis, `roll_right` about the barrel axis."""
    return q_mul(q_mul(q_axis_angle(Y, tip_forward), q_axis_angle(Z, swing_left)),
                 q_axis_angle(X, roll_right))


def wave(time, period, amplitude, phase=0.0):
    return amplitude * math.sin(2.0 * math.pi * (time / period + phase))


def loop_keys(track, length, function, steps=8, easing="linear"):
    """Samples a periodic function into a track (first key equals the last)."""
    for step in range(steps + 1):
        time = length * step / steps
        track.key(time, function(time), easing)


def recoil(clip, kick, rise, roll=0.0, side=0.0, start=0.0, settle=None, easing="settle"):
    """A shot: the gun snaps back and up, then springs home."""
    settle = settle if settle is not None else clip.length
    clip.gun.key(start, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "linear")
    clip.gun.key(start + 0.033, (side * 0.4, rise * 0.004, kick, side, rise, roll), "snap")
    clip.gun.key(settle, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), easing)


# Where the left hand finds things that are not on the gun, in the steady view:
# the magazine pouches on the front of the belt (left of the buckle).
POUCH = Frame(in_view(-0.18, -0.39, 0.16), hand_frame((0.05, -1.0, -0.15), (0.0, 0.25, 1.0)))
POUCH_LOW = Frame(in_view(-0.18, -0.42, 0.14), POUCH.q)

OPEN = Grip(0.12, spread=0.6)
RELAXED = Grip(0.3, spread=0.15)
FIST = Grip(0.92, thumb=0.95)
HOLD_PART = Grip(0.78, thumb=0.56)


def held(motion: Frame, grab: Frame) -> Frame:
    """The left palm while it holds a part that `motion` moves (gun space)."""
    return motion.compose(grab)


class MagSwap:
    """A box magazine (or cell) taken out by the left hand and a fresh one put in.

    `grab` is the left palm closed on the part at rest (gun space); `pivot`
    the part's pivot (its front lug or top). Motions are part motions in model
    space (rotation about the pivot, then a shift)."""

    def __init__(self, family, part, pivot, grab, new_part=None):
        self.family = family
        self.part = part
        self.new_part = new_part or part + "_new"
        self.pivot = pivot
        self.grab = grab

    def motion(self, offset=(0.0, 0.0, 0.0), tip=0.0, swing=0.0, roll=0.0):
        return part_motion(self.pivot, offset, model_turn(tip, swing, roll))

    def palm(self, **motion):
        return HandAt(held(self.motion(**motion), self.grab))


def common_clips(family, prefix, lengths, walk_name, shoot=(0.022, 3.0, 1.5), aim_shoot=(0.016, 1.6, 0.8),
                 shoulder="Shoulder"):
    """Idle, walk, fire and aim clips every gun family has (names after `prefix`)."""
    clips = []
    idle = family.clip(prefix + "Idle", lengths[prefix + "Idle"])
    idle.looping = True
    clips.append(idle)

    walk = family.clip(walk_name, lengths[walk_name])
    walk.looping = True
    period = walk.length
    loop_keys(walk.gun, period, lambda t: (wave(t, period, 0.004), -0.006 - abs(wave(t, period, 0.005, 0.25)),
                                           0.0, wave(t, period, 0.8), wave(t, period, 0.6, 0.25), wave(t, period, 1.5)))
    clips.append(walk)

    fire = family.clip(prefix + "Shoot", lengths[prefix + "Shoot"])
    kick, rise, roll = shoot
    recoil(fire, kick, rise, roll, settle=fire.length)
    fire.right_grip = Track(family.stance.right_grip).key(
        0.02, replace(family.stance.right_grip, curl=0.9, trigger=0.55), "snap") \
        .key(fire.length, family.stance.right_grip, "inout")
    clips.append(fire)

    raise_name = prefix + shoulder
    if raise_name in lengths:
        lift = family.clip(raise_name, lengths[raise_name], aimed_base=True)
        lift.match_end_pose = True
        hip = family.hip_in_ads()
        mid = tuple(a * 0.45 for a in hip)
        lift.gun = Track(hip).key(lift.length * 0.55, (mid[0], mid[1] - 0.012, mid[2], mid[3], mid[4] + 1.5,
                                                       mid[5] + 3.0), "inout") \
            .key(lift.length, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "out")
        stance = family.stance
        lift.torso = Track(stance.torso).key(lift.length, stance.aim_torso, "inout")
        # Head compensation and collar reach must follow the same blend as the
        # torso. Leaving either at its aimed value snaps the shoulders/camera
        # as soon as the raise clip starts, even when the gun track is smooth.
        for attribute in ("neck", "head"):
            setattr(lift, attribute, Track(tuple(-v * .5 for v in stance.torso)).key(
                lift.length, tuple(-v * .5 for v in stance.aim_torso), "inout"))
        for side in ("left", "right"):
            start = getattr(stance, "collar_" + side)
            end = getattr(stance, "aim_collar_" + side)
            setattr(lift, "collar_" + side, Track(start).key(
                lift.length, start if end is None else end, "inout"))
        lift.elbows = Track(stance.elbows).key(lift.length, stance.aim_elbows, "inout")
        clips.append(lift)

    for name, looping in ((prefix + "ShoulderIdle", True),):
        if name in lengths:
            still = family.clip(name, lengths[name], aimed_base=True)
            still.looping = looping
            clips.append(still)

    aim_walk_name = prefix + "ShoulderWalk"
    if aim_walk_name in lengths:
        aim_walk = family.clip(aim_walk_name, lengths[aim_walk_name], aimed_base=True)
        aim_walk.looping = True
        # Aimed, the sights stay put: the walk's bob comes from the procedural
        # motion (damped while aimed), so the clip only settles the gun a hair.
        aim_walk.gun = Track((0.0, -0.002, 0.0, 0.0, -0.3, 0.0))
        clips.append(aim_walk)

    aim_fire_name = prefix + "ShoulderShoot"
    if aim_fire_name in lengths:
        aim_fire = family.clip(aim_fire_name, lengths[aim_fire_name], aimed_base=True)
        kick, rise, roll = aim_shoot
        recoil(aim_fire, kick, rise, roll, settle=aim_fire.length)
        clips.append(aim_fire)
    return clips


def assault_rifle(model="modern/ak"):
    """The assault rifle. Classic and Modern ship different AK models, so the
    Classic one gets its own clips (`GunReload@classic/ak`, ...)."""
    classic = model.startswith("classic")
    gun = GUNS[model]
    family = Family("Gun", gun, Stance())
    clips = common_clips(family, "Gun", LENGTHS, "GunRun")
    pivot = part_pivot_at(model, "mag")
    # Grip the volume inside the magazine, with the fingers coming upward
    # around its front edge. The old point was on the outside/front surface:
    # a mathematically coincident PALM still left the magazine at the fingertips.
    grab = palm((-0.190, 0.133, -0.035) if classic else (-0.215, 0.132, -0.063),
                (-0.81915204, 0.0, 0.57357644) if classic else (-0.90630779, 0.0, 0.42261826),
                (0.0, 1.0, 0.0))
    swap = MagSwap(family, "mag", pivot, grab)
    well = (-0.19, 0.13, 0.03) if classic else (-0.21, 0.135, 0.0)
    spec = SwapSpec(
        reference=2.983, present=shown_at(family, well, (0.5, 0.66), 0.36 if classic else 0.34,
                                        (16.0, 9.0, -30.0)),
        grab_at=0.38, release_at=0.44,
        out=[(0.62, dict(offset=(-0.012, 0.0, -0.035), tip=9.0), "inout"),
             (0.76, dict(offset=(-0.05, -0.02, -0.14), tip=14.0, roll=-5.0), "in")],
        drop_at=0.78, drop_velocity=(-0.7, -1.3, -0.4), pouch_at=1.04, show_at=1.08,
        insert=[(1.38, dict(offset=(-0.04, -0.01, -0.11), tip=15.0, roll=-4.0), "out"),
                (1.55, dict(offset=(-0.004, 0.0, -0.012), tip=10.0), "inout")],
        seat_at=1.68,
        sounds={"release": "sounds/ar_mag_release", "out": "sounds/ar_mag_out", "in": "sounds/ar_mag_in",
                "seat": "sounds/ar_mag_seat"})
    clips.append(box_mag_reload(family, swap, "GunReload", LENGTHS["GunReload"], spec))
    handle = palm(part_pivot_at(model, "bolt"),
                  (-0.8660254, 0.0, 0.5), (0.0, 1.0, 0.0))
    clips.append(box_mag_reload(family, swap, "GunReloadEmpty", LENGTHS["GunReload"], spec,
                                empty_tail=ak_charging_handle(family, swap, handle)))

    def free_hand_path(clip: Clip, start: float, end: float, bow: tuple,
                       turn_power: float, anchor_start: bool = False) -> None:
        """Turn a released hand in view space while clearing the receiver.

        The magazine and charging-handle contacts remain in gun space. A
        released hand must not inherit the gun's presentation roll on top of
        its own turn between the underhand support and those side contacts.
        """
        first = clip.left_frame(start, clip.gun_frame(start))
        last = clip.left_frame(end, clip.gun_frame(end))
        clip.left.keys = [key for key in clip.left.keys
                          if key[0] <= start + 1e-8 or key[0] >= end - 1e-8]
        if anchor_start:
            clip.left.key(start, HandAt(first, "view"), "linear")
        for fraction in (0.2, 0.4, 0.6, 0.8):
            position = v_add(v_lerp(first.p, last.p, fraction),
                             v_scale(bow, math.sin(math.pi * fraction)))
            orientation = q_slerp(first.q, last.q, fraction ** turn_power)
            clip.left.key(start + (end - start) * fraction,
                          HandAt(Frame(position, orientation), "view"), "linear")

    for clip in clips[-2:]:
        empty = clip.name.endswith("Empty")
        speed = spec.hurry if empty else 1.0
        # Let the arm share the entry turn before the magazine is attached.
        # These are IK preferences, not literal anatomical elbow angles.
        clip.elbows.key(spec.grab_at * speed, (120.0, family.stance.elbows[1]), "linear")
        clip.elbows.key(0.68 * speed, (-25.0, family.stance.elbows[1]), "inout")
        clip.elbows.key(1.85 * speed, (-25.0, family.stance.elbows[1]), "linear")
        clip.elbows.key(clip.length - 0.22, family.stance.elbows, "inout")
        pickup = Frame(in_view(-0.16, -0.36, 0.26), hand_frame((0.0, 0.0, -1.0), (1.0, 0.0, 0.0)))
        clip.left.key(spec.pouch_at * speed, HandAt(pickup, "view"), "inout")
        clip.left.key((spec.show_at + 0.04) * speed, HandAt(pickup, "view"), "linear")
        free_hand_path(clip, 0.0, spec.grab_at * speed, (-0.025, -0.03, 0.0), 0.8,
                       anchor_start=True)
        # After the magazine seats (or the charging handle is released),
        # clear below the gun before turning the palm up to the handguard.
        # Its existing contact and mechanical-cue times are unchanged.
        return_start = spec.seat_at * speed + (0.54 if empty else 0.0)
        return_end = spec.seat_at * speed + (1.05 if empty else 0.54)
        free_hand_path(clip, return_start, return_end, (-0.025, -0.04, 0.0), 1.2,
                       anchor_start=empty)
        clip.elbows.key(return_start, (-25.0, family.stance.elbows[1]), "linear")
        clip.elbows.key(return_end, (-120.0, family.stance.elbows[1]), "linear")
        clip.elbows.key(clip.length - 0.04, family.stance.elbows, "inout")
    if classic:
        for clip in clips:
            clip.name += "@classic/ak"
    return family, clips


def classic_assault_rifle():
    return assault_rifle("classic/ak")


@dataclass
class SwapSpec:
    """The choreography of a magazine (or cell) change, on a reference timeline
    of `reference` seconds (scaled to the clip's length; squeezed by `hurry`
    when the reload is from empty, to make room for the bolt or slide)."""
    reference: float
    present: tuple                       # the gun tilted to show the magazine well
    grab_at: float                       # the hand closes on the old magazine
    release_at: float                    # the release is pressed
    out: list                            # (time, part motion kwargs, easing) pulling it out
    drop_at: float
    drop_velocity: tuple
    pouch_at: float                      # the hand is at the pouch
    show_at: float
    insert: list                         # (time, part motion kwargs, easing) bringing the new one in
    seat_at: float
    sounds: dict                         # release, out, pouch, in, seat
    hurry: float = 0.74
    drop_spin: tuple = (5.0, 1.0, 3.0)
    gun_wobble: float = 1.0
    seat_kick: float = 0.7


def box_mag_reload(family, swap: MagSwap, name, length, spec: SwapSpec, empty_tail=None, grip_release=None):
    """A magazine change: tilt the gun, take the old magazine out with the left
    hand and flick it away, a fresh one from the pouch, seat it; then either
    back to the handguard, or (from empty) `empty_tail` runs the bolt or slide."""
    clip = family.clip(name, length)
    s = length / spec.reference
    k = s * (spec.hurry if empty_tail else 1.0)
    q = lambda t: t * k
    gun = family.gun
    present = spec.present
    w = spec.gun_wobble

    def tilt(dx=0.0, dy=0.0, dz=0.0, yaw=0.0, pitch=0.0, roll=0.0):
        return (present[0] + dx * w, present[1] + dy * w, present[2] + dz * w, present[3] + yaw * w,
                present[4] + pitch * w, present[5] + roll * w)

    g = clip.gun
    g.key(q(spec.grab_at * 0.26), (0.005, -0.01, 0.0, -2.0, -2.0, 3.0), "inout")
    g.key(q(spec.grab_at * 0.95), present, "out")
    g.key(q(spec.release_at + 0.02), tilt(dy=-0.004, pitch=-1.5, roll=-1.0), "inout")
    g.key(q(spec.out[0][0]), tilt(dx=0.004, dy=0.008, pitch=1.5, roll=-3.0), "out")
    g.key(q(spec.drop_at), tilt(dx=0.01, dy=-0.004, yaw=-3.0, roll=4.0), "inout")
    g.key(q(spec.pouch_at), tilt(dx=0.012, dy=-0.02, dz=-0.005, yaw=-5.0, pitch=-2.0, roll=8.0), "inout")
    g.key(q(spec.insert[0][0]), tilt(dy=0.004, pitch=1.0, roll=-1.0), "inout")
    g.key(q(spec.seat_at - 0.08), tilt(dy=-0.002, roll=-2.0), "inout")
    g.key(q(spec.seat_at), tilt(dy=0.014, pitch=4.0, roll=-3.5), "snap")
    g.key(q(spec.seat_at + 0.14), tilt(roll=-1.0), "out")

    hand = clip.left
    hand.key(q(spec.grab_at * 0.2), HandAt(gun.support), "linear")
    hand.key(q(spec.grab_at), HandAt(swap.grab, arc=(0.0, -0.05, 0.0)), "out")
    hand.key(q(spec.release_at + 0.02), swap.palm(offset=(0.0, 0.0, 0.003)), "inout")
    for time, motion, easing in spec.out:
        hand.key(q(time), swap.palm(**motion), easing)
    hand.key(q(spec.pouch_at), HandAt(POUCH, "view", arc=(-0.04, 0.0, 0.02)), "inout")
    hand.key(q(spec.show_at + 0.04), HandAt(POUCH_LOW, "view"), "inout")
    for time, motion, easing in spec.insert:
        hand.key(q(time), swap.palm(**motion), easing)
    hand.key(q(spec.seat_at), swap.palm(), "snap")

    grip = clip.left_grip
    grip.key(q(spec.grab_at * 0.26), OPEN, "inout")
    grip.key(q(spec.grab_at * 0.95), Grip(0.25, spread=0.3), "inout")
    grip.key(q(spec.release_at), HOLD_PART, "snap")
    grip.key(q(spec.drop_at - 0.04), HOLD_PART, "linear")
    grip.key(q(spec.drop_at + 0.02), Grip(0.05, spread=0.9), "snap")
    grip.key(q(spec.pouch_at - 0.02), RELAXED, "inout")
    grip.key(q(spec.show_at + 0.02), HOLD_PART, "inout")
    grip.key(q(spec.seat_at + 0.02), HOLD_PART, "linear")
    if grip_release is not None:
        # The right thumb presses a release on the grip.
        right = family.stance.right_grip
        pressed = Grip(right.curl, trigger=right.trigger, thumb=0.2)
        clip.right_grip.key(q(spec.release_at - 0.06), right, "linear")
        clip.right_grip.key(q(spec.release_at), pressed, "snap")
        clip.right_grip.key(q(spec.release_at + 0.12), right, "inout")

    look = clip.look
    look.key(q(spec.grab_at), (2.0, 4.0, -2.0), "inout")
    look.key(q(spec.show_at), (0.0, 1.5, 0.0), "inout")
    look.key(q(spec.seat_at - 0.18), (2.0, 5.0, -3.0), "inout")
    look.key(q(spec.seat_at + 0.02), (2.0, 3.0, -2.5), "snap")

    torso = family.stance.torso
    clip.torso.key(q(spec.grab_at), (torso[0] - 4.0, torso[1] + 3.0, torso[2]), "inout")
    clip.torso.key(q(spec.show_at), (torso[0] - 2.0, torso[1] + 4.0, torso[2]), "inout")
    clip.torso.key(q(spec.seat_at - 0.08), (torso[0] - 4.0, torso[1] + 3.0, torso[2]), "inout")

    sounds = spec.sounds
    clip.cue(q(0.02), sound="sounds/cloth_raise", volume=0.45, pitch_jitter=0.05)
    clip.cue(q(spec.release_at), attach=swap.part, bone="PropLeft")
    clip.cue(q(spec.release_at + 0.01), sound=sounds["release"], volume=0.9, pitch_jitter=0.04)
    clip.cue(q(spec.out[0][0] - 0.05), sound=sounds["out"], volume=0.95, pitch_jitter=0.04)
    clip.cue(q(spec.drop_at), drop=swap.part, velocity=spec.drop_velocity, spin=spec.drop_spin, life=4.0)
    clip.cue(q(spec.drop_at) + 0.42, sound=sounds.get("ground", "sounds/mag_hit_ground"), volume=0.3,
             pitch_jitter=0.08, who="self")
    clip.cue(q(spec.pouch_at - 0.02), sound=sounds.get("pouch", "sounds/pouch_mag"), volume=0.8, pitch_jitter=0.05)
    clip.cue(q(spec.show_at), show=swap.new_part, bone="PropLeft")
    clip.cue(q(spec.seat_at - 0.15), sound=sounds["in"], volume=0.9, pitch_jitter=0.04)
    clip.cue(q(spec.seat_at), sound=sounds["seat"], volume=1.0, pitch_jitter=0.03)
    clip.cue(q(spec.seat_at), kick={"pitch": -spec.seat_kick, "roll": 0.5, "seconds": 0.28})
    clip.cue(q(spec.seat_at), detach=swap.new_part, blend=0.06)
    clip.part_follow[swap.new_part] = (grab_after(swap), q(spec.show_at - 0.06), q(spec.seat_at), swap.pivot)
    clip.part_follow[swap.part] = (grab_after(swap), q(spec.release_at), q(spec.drop_at), swap.pivot)

    t0 = q(spec.seat_at)
    if empty_tail is None:
        hand.key(t0 + 0.12 * s, HandAt(Frame(v_add(swap.grab.p, (0.0, -0.005, -0.03)), swap.grab.q)), "out")
        hand.key(t0 + 0.54 * s, HandAt(gun.support, arc=(0.0, -0.03, 0.0)), "inout")
        grip.key(t0 + 0.12 * s, Grip(0.3, spread=0.2), "out")
        grip.key(t0 + 0.47 * s, Grip(0.25), "inout")
        grip.key(t0 + 0.64 * s, family.stance.left_grip, "inout")
        g.key(t0 + 0.62 * s, (0.004, -0.004, 0.0, -1.0, -1.0, 2.0), "inout")
        g.key(length, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "settle")
        look.key(t0 + 0.72 * s, (0.0, 0.0, 0.0), "inout")
        clip.torso.key(t0 + 0.72 * s, torso, "inout")
        clip.cue(t0 + 0.54 * s, sound="sounds/cloth_lower", volume=0.4, pitch_jitter=0.05)
        clip.cue(t0 + 0.62 * s, sound="sounds/grip_tap", volume=0.45, pitch_jitter=0.06)
    else:
        empty_tail(clip, t0, s)
    return clip


def ak_charging_handle(family, swap, handle):
    """Keep the receiver tilted while the off hand pulls its charging handle."""
    gun = family.gun

    def tail(clip, t0, s):
        g, hand, grip, look = clip.gun, clip.left, clip.left_grip, clip.look
        # Keep the magazine-change tilt. Rolling 54 degrees the opposite way
        # here formerly demanded a near-half-turn correction at the left wrist.
        back = Frame(v_add(handle.p, (0.08, 0.0, 0.0)), handle.q)
        g.key(t0 + 0.22 * s, (0.04, 0.05, -0.04, -8.0, 7.0, -20.0), "inout")
        g.key(t0 + 0.42 * s, (0.04, 0.046, -0.04, -9.0, 6.0, -20.0), "inout")
        g.key(t0 + 0.52 * s, (0.045, 0.062, -0.032, -9.0, 9.5, -20.0), "anticipate")
        g.key(t0 + 0.60 * s, (0.04, 0.04, -0.044, -7.0, 5.0, -20.0), "snap")
        g.key(t0 + 1.05 * s, (0.004, -0.004, 0.0, -1.0, -1.0, 2.0), "inout")
        g.key(clip.length, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "settle")
        hand.key(t0 + 0.12 * s, HandAt(Frame(v_add(swap.grab.p, (0.0, -0.01, -0.03)), swap.grab.q)), "out")
        hand.key(t0 + 0.40 * s, HandAt(handle, arc=(0.0, -0.04, 0.0)), "inout")
        hand.key(t0 + 0.52 * s, HandAt(back), "anticipate")
        hand.key(t0 + 0.62 * s, HandAt(Frame(v_add(back.p, (0.03, 0.03, 0.04)), back.q)), "out")
        hand.key(t0 + 1.05 * s, HandAt(gun.support, arc=(0.0, -0.03, 0.0)), "inout")
        grip.key(t0 + 0.12 * s, Grip(0.3, spread=0.2), "out")
        grip.key(t0 + 0.34 * s, Grip(0.55, trigger=.7, spread=.05), "inout")
        grip.key(t0 + 0.42 * s, Grip(0.8, trigger=.9, thumb=.5), "out")
        grip.key(t0 + 0.54 * s, Grip(0.8, trigger=.9, thumb=.5), "linear")
        grip.key(t0 + 0.60 * s, Grip(0.35, spread=.15), "out")
        grip.key(t0 + 0.95 * s, Grip(0.25), "inout")
        grip.key(t0 + 1.12 * s, family.stance.left_grip, "inout")
        look.key(t0 + 0.40 * s, (-2.0, 3.0, 2.0), "inout")
        look.key(t0 + 0.55 * s, (-2.0, 2.0, 2.0), "inout")
        look.key(t0 + 1.10 * s, (0.0, 0.0, 0.0), "inout")
        clip.torso.key(t0 + 1.10 * s, family.stance.torso, "inout")
        clip.cue(t0 + 0.45 * s, sound="sounds/ar_charge_back", volume=0.95, pitch_jitter=0.03)
        clip.cue(t0 + 0.53 * s, sound="sounds/ar_charge_forward", volume=1.0, pitch_jitter=0.03)
        clip.cue(t0 + 0.53 * s, kick={"pitch": -0.9, "yaw": 0.4, "seconds": 0.3})
        clip.cue(t0 + 1.00 * s, sound="sounds/grip_tap", volume=0.45, pitch_jitter=0.06)
        if any(part["name"] == "bolt" for part in PARTS[gun.model]):
            bolt = clip.part("bolt")
            bolt.key(t0 + 0.40 * s, Frame(), "linear")
            bolt.key(t0 + 0.52 * s, Frame((0.08, 0.0, 0.0), IDENTITY), "inout")
            bolt.key(t0 + 0.57 * s, Frame(), "in")
    return tail


def gun_swap_spec(**changes):
    """The assault rifle's magazine change, which the other box-magazine guns retime."""
    spec = dict(
        reference=2.983, present=(-0.05, 0.062, 0.012, 16.0, 9.0, -30.0),
        grab_at=0.38, release_at=0.44,
        out=[(0.62, dict(offset=(-0.012, 0.0, -0.035), tip=18.0), "inout"),
             (0.76, dict(offset=(-0.05, -0.02, -0.14), tip=28.0, roll=-10.0), "in")],
        drop_at=0.78, drop_velocity=(-0.7, -1.3, -0.4), pouch_at=1.04, show_at=1.08,
        insert=[(1.38, dict(offset=(-0.04, -0.01, -0.11), tip=30.0, roll=-8.0), "out"),
                (1.55, dict(offset=(-0.004, 0.0, -0.012), tip=20.0), "inout")],
        seat_at=1.68,
        sounds={"release": "sounds/ar_mag_release", "out": "sounds/ar_mag_out", "in": "sounds/ar_mag_in",
                "seat": "sounds/ar_mag_seat"})
    spec.update(changes)
    return SwapSpec(**spec)


def grip_mag_reload(family, swap: MagSwap, name, length, axis, sounds, reference, empty_tail=None,
                    present=(-0.03, 0.03, 0.03, 12.0, 6.0, -24.0), travel=0.15, empty_hurry=0.78):
    """A magazine in the grip (pistols, the SMG, the laser pistol and SMG): the
    right thumb drops it free, the left hand fetches a fresh one, pushes it up
    the grip and slaps it home."""
    clip = family.clip(name, length)
    s = length / reference
    k = s * (empty_hurry if empty_tail else 1.0)
    q = lambda t: t * k
    gun = family.gun
    down = v_scale(v_norm(axis), -1.0)

    def along(distance, tip=0.0, swing=0.0):
        return dict(offset=v_scale(down, distance), tip=tip, swing=swing)

    g = clip.gun
    g.key(q(0.06), (0.004, -0.008, 0.0, -2.0, -2.0, 3.0), "inout")
    g.key(q(0.20), present, "out")
    g.key(q(0.30), (present[0], present[1] + 0.006, present[2], present[3], present[4] + 2.0, present[5] - 2.0), "snap")
    g.key(q(0.40), (present[0] + 0.01, present[1] - 0.01, present[2], present[3] - 3.0, present[4], present[5] + 6.0),
          "inout")
    g.key(q(0.62), present, "inout")
    g.key(q(0.70), present, "linear")
    g.key(q(0.76), (present[0], present[1] + 0.018, present[2], present[3], present[4] + 5.0, present[5] - 2.0), "snap")
    g.key(q(0.88), (present[0], present[1] + 0.004, present[2], present[3], present[4] + 1.0, present[5]), "out")

    # The old magazine slides out under its own weight.
    track = clip.part(swap.part)
    track.key(q(0.16), Frame(), "linear")
    track.key(q(0.27), track_value(swap.motion(**along(0.05)), swap.pivot), "in")

    hand = clip.left
    hand.key(q(0.04), HandAt(gun.support), "linear")
    hand.key(q(0.32), HandAt(POUCH, "view", arc=(-0.03, -0.03, 0.02)), "inout")
    hand.key(q(0.38), HandAt(POUCH_LOW, "view"), "inout")
    hand.key(q(0.60), swap.palm(**along(travel * 0.45, tip=6.0)), "out")
    hand.key(q(0.70), swap.palm(**along(0.012)), "inout")
    hand.key(q(0.76), swap.palm(), "snap")

    grip = clip.left_grip
    grip.key(q(0.10), OPEN, "inout")
    grip.key(q(0.30), RELAXED, "inout")
    grip.key(q(0.37), HOLD_PART, "inout")
    grip.key(q(0.70), HOLD_PART, "linear")
    grip.key(q(0.75), Grip(0.35, thumb=0.3), "snap")
    right = family.stance.right_grip
    clip.right_grip.key(q(0.12), right, "linear")
    clip.right_grip.key(q(0.16), Grip(right.curl, trigger=right.trigger, thumb=0.15), "snap")
    clip.right_grip.key(q(0.28), right, "inout")

    look = clip.look
    look.key(q(0.22), (1.5, 3.0, -1.5), "inout")
    look.key(q(0.40), (0.0, 1.0, 0.0), "inout")
    look.key(q(0.66), (1.5, 4.0, -2.0), "inout")
    look.key(q(0.78), (1.5, 2.5, -1.5), "snap")

    clip.cue(q(0.02), sound="sounds/cloth_raise", volume=0.4, pitch_jitter=0.05)
    clip.cue(q(0.16), sound=sounds["release"], volume=0.9, pitch_jitter=0.04)
    clip.cue(q(0.18), sound=sounds["out"], volume=0.85, pitch_jitter=0.05)
    clip.cue(q(0.27), drop=swap.part, velocity=(0.1, -1.4, -0.15), spin=(6.0, 0.5, 2.0), life=4.0)
    clip.cue(q(0.27) + 0.40, sound="sounds/mag_hit_ground", volume=0.28, pitch_jitter=0.1, who="self")
    clip.cue(q(0.30), sound="sounds/pouch_mag", volume=0.75, pitch_jitter=0.05)
    clip.cue(q(0.36), show=swap.new_part, bone="PropLeft")
    clip.cue(q(0.66), sound=sounds["in"], volume=0.9, pitch_jitter=0.04)
    clip.cue(q(0.76), sound=sounds["seat"], volume=1.0, pitch_jitter=0.03)
    clip.cue(q(0.76), kick={"pitch": -0.8, "roll": -0.4, "seconds": 0.25})
    clip.cue(q(0.76), detach=swap.new_part, blend=0.05)
    clip.part_follow[swap.new_part] = (grab_after(swap), q(0.32), q(0.76), swap.pivot)

    t0 = q(0.76)
    if empty_tail is None:
        hand.key(t0 + 0.08 * s, HandAt(Frame(v_add(swap.grab.p, v_scale(down, 0.03)), swap.grab.q)), "out")
        hand.key(t0 + 0.34 * s, HandAt(gun.support), "inout")
        grip.key(t0 + 0.30 * s, Grip(0.3), "inout")
        grip.key(t0 + 0.42 * s, family.stance.left_grip, "inout")
        g.key(t0 + 0.42 * s, (0.003, -0.003, 0.0, -1.0, -1.0, 2.0), "inout")
        g.key(length, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "settle")
        look.key(t0 + 0.50 * s, (0.0, 0.0, 0.0), "inout")
        clip.cue(t0 + 0.36 * s, sound="sounds/grip_tap", volume=0.4, pitch_jitter=0.06)
    else:
        empty_tail(clip, t0, s)
    return clip


def slide_rack(family, part, back, palm_on, lift=(0.0, 0.0, 0.0, 0.0, 0.0, 0.0), sounds=None, travel=0.035,
               axis=(1.0, 0.0, 0.0), release_kick=0.7):
    """From empty: the left hand grabs a slide (or a cocking knob) from above,
    pulls it back and lets it fly home."""
    sounds = sounds or {"back": "sounds/pistol_slide_back", "forward": "sounds/pistol_slide_forward"}
    gun = family.gun

    def tail(clip, t0, s):
        g, hand, grip, look = clip.gun, clip.left, clip.left_grip, clip.look
        pulled = Frame(v_add(palm_on.p, v_scale(v_norm(axis), travel)), palm_on.q)
        g.key(t0 + 0.14 * s, lift, "inout")
        g.key(t0 + 0.30 * s, lift, "linear")
        g.key(t0 + 0.38 * s, (lift[0], lift[1] - 0.006, lift[2] + 0.01, lift[3] - 1.0, lift[4] - 2.0, lift[5]), "anticipate")
        g.key(t0 + 0.44 * s, (lift[0], lift[1] + 0.012, lift[2], lift[3], lift[4] + 4.0, lift[5]), "snap")
        g.key(t0 + 0.72 * s, (0.003, -0.003, 0.0, -1.0, -1.0, 2.0), "inout")
        g.key(clip.length, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "settle")
        hand.key(t0 + 0.24 * s, HandAt(palm_on, arc=(0.0, 0.04, 0.0)), "inout")
        hand.key(t0 + 0.38 * s, HandAt(pulled), "anticipate")
        hand.key(t0 + 0.46 * s, HandAt(Frame(v_add(pulled.p, (0.02, 0.02, 0.03)), pulled.q)), "out")
        hand.key(t0 + 0.74 * s, HandAt(gun.support), "inout")
        grip.key(t0 + 0.20 * s, Grip(0.35, spread=0.2), "inout")
        grip.key(t0 + 0.27 * s, Grip(0.8, thumb=0.6), "snap")
        grip.key(t0 + 0.39 * s, Grip(0.8, thumb=0.6), "linear")
        grip.key(t0 + 0.43 * s, Grip(0.1, spread=0.6), "snap")
        grip.key(t0 + 0.68 * s, Grip(0.3), "inout")
        grip.key(t0 + 0.80 * s, family.stance.left_grip, "inout")
        track = clip.part(part)
        track.key(t0 + 0.27 * s, Frame(), "linear")
        track.key(t0 + 0.38 * s, Frame(v_scale(v_norm(axis), travel), IDENTITY), "anticipate")
        track.key(t0 + 0.405 * s, Frame(v_scale(v_norm(axis), travel), IDENTITY), "linear")
        track.key(t0 + 0.43 * s, Frame(), "in")
        look.key(t0 + 0.30 * s, (-1.0, 2.0, 1.0), "inout")
        look.key(t0 + 0.80 * s, (0.0, 0.0, 0.0), "inout")
        clip.cue(t0 + 0.33 * s, sound=sounds["back"], volume=0.95, pitch_jitter=0.03)
        clip.cue(t0 + 0.41 * s, sound=sounds["forward"], volume=1.0, pitch_jitter=0.03)
        clip.cue(t0 + 0.41 * s, kick={"pitch": -release_kick, "yaw": 0.3, "seconds": 0.25})
    return tail


def charge_prime(family, sounds, palm_on, lift=(0.0, 0.0, 0.0, -3.0, 2.0, 8.0)):
    """Laser guns from empty: the left hand slaps the cell's prime and the gun
    whines up to charge."""
    gun = family.gun

    def tail(clip, t0, s):
        g, hand, grip, look = clip.gun, clip.left, clip.left_grip, clip.look
        g.key(t0 + 0.16 * s, lift, "inout")
        g.key(t0 + 0.30 * s, lift, "linear")
        g.key(t0 + 0.34 * s, (lift[0], lift[1] - 0.012, lift[2], lift[3], lift[4] - 3.0, lift[5] + 2.0), "snap")
        g.key(t0 + 0.55 * s, (lift[0], lift[1], lift[2], lift[3], lift[4] + 1.0, lift[5]), "settle")
        g.key(t0 + 0.95 * s, (0.003, -0.003, 0.0, -1.0, -1.0, 2.0), "inout")
        g.key(clip.length, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "settle")
        above = Frame(v_add(palm_on.p, (0.0, 0.0, 0.05)), palm_on.q)
        hand.key(t0 + 0.24 * s, HandAt(above, arc=(0.0, 0.03, 0.0)), "inout")
        hand.key(t0 + 0.33 * s, HandAt(palm_on), "snap")
        hand.key(t0 + 0.44 * s, HandAt(above), "out")
        hand.key(t0 + 0.92 * s, HandAt(gun.support), "inout")
        grip.key(t0 + 0.22 * s, Grip(0.1, spread=0.4), "inout")
        grip.key(t0 + 0.85 * s, Grip(0.3), "inout")
        grip.key(t0 + 0.98 * s, family.stance.left_grip, "inout")
        look.key(t0 + 0.40 * s, (-1.0, 2.0, 1.0), "inout")
        look.key(t0 + 1.0 * s, (0.0, 0.0, 0.0), "inout")
        clip.cue(t0 + 0.33 * s, sound=sounds["prime"], volume=0.9, pitch_jitter=0.03)
        clip.cue(t0 + 0.36 * s, sound=sounds["charge"], volume=0.8, pitch_jitter=0.02)
        clip.cue(t0 + 0.33 * s, kick={"pitch": 0.5, "seconds": 0.2})
    return tail


def pump_action(clip, family, start, pump_axis, travel, sounds=None, hand_drift=(0.0, 0.0, 0.0),
                pump="pump"):
    """A pump stroke after a shot: the left hand runs the forend back and home."""
    sounds = sounds or {"back": "sounds/shotgun_pump_back", "forward": "sounds/shotgun_pump_forward"}
    gun = family.gun
    back = v_scale(v_norm(pump_axis), travel)
    track = clip.part(pump)
    t = start
    track.key(t, Frame(), "linear")
    track.key(t + 0.12, Frame(back, IDENTITY), "inout")
    track.key(t + 0.17, Frame(back, IDENTITY), "linear")
    track.key(t + 0.27, Frame(), "snap")
    support = gun.support
    clip.left.key(t, HandAt(support), "linear")
    clip.left.key(t + 0.12, HandAt(Frame(v_add(v_add(support.p, back), hand_drift), support.q)), "inout")
    clip.left.key(t + 0.17, HandAt(Frame(v_add(v_add(support.p, back), hand_drift), support.q)), "linear")
    clip.left.key(t + 0.27, HandAt(support), "snap")
    clip.left_grip.key(t, family.stance.left_grip, "linear")
    clip.left_grip.key(t + 0.05, HOLD_PART, "inout")
    clip.left_grip.key(t + 0.32, family.stance.left_grip, "inout")
    clip.cue(t + 0.06, sound=sounds["back"], volume=0.95, pitch_jitter=0.03)
    clip.cue(t + 0.24, sound=sounds["forward"], volume=1.0, pitch_jitter=0.03)
    clip.cue(t + 0.24, kick={"pitch": -0.5, "seconds": 0.2})


def round_reload(family, name, length, part, pivot, grab, approach, port_motion, sounds, reference,
                 loading=(-0.03, 0.04, 0.02, 10.0, 6.0, -35.0), push=(0.0, 0.0, 0.0), source=None, stays=False,
                 spent=None):
    """One round at a time (shotgun shells, energy shells, a rocket): the clip
    starts and ends with the hand at the belt and the gun tilted to load, so
    it can run again for the next round without a seam."""
    clip = family.clip(name, length)
    s = length / reference
    q = lambda t: t * s
    gun = family.gun
    # A shell is palmed beside the loading gate. The former shared routine
    # used 0.98-second keys in a 0.58-second clip and snapped 38 cm back to the
    # belt in its final frame. Use fractions of this round's actual duration.
    q = lambda fraction: fraction * length
    source = source or Frame(in_view(-0.16, -0.32, 0.19),
                             clip.gun_frame(0.0).compose(grab).q)
    swap = MagSwap(family, part, pivot, grab)
    clip.gun = Track(loading)
    clip.gun.key(q(0.30), loading, "linear")
    clip.gun.key(q(0.36), (loading[0], loading[1] + 0.012, loading[2] + 0.008, loading[3], loading[4] + 3.0,
                           loading[5] - 2.0), "snap")
    clip.gun.key(q(0.62), loading, "out")
    clip.gun.key(length, loading, "inout")
    clip.left = Track(HandAt(source, "view"))
    clip.left.key(q(0.06), HandAt(source, "view"), "linear")
    clip.left.key(q(0.34), swap.palm(**approach), "inout")
    clip.left.key(q(0.48), swap.palm(**port_motion), "out")
    clip.left.key(q(0.60), swap.palm(**push), "inout")
    clip.left.key(q(0.73), HandAt(Frame(v_add(swap.grab.p, (0.02, -0.02, -0.04)), swap.grab.q)), "out")
    clip.left.key(length, HandAt(source, "view"), "inout")
    clip.left_grip = Track(HOLD_PART)
    clip.left_grip.key(q(0.58), HOLD_PART, "linear")
    clip.left_grip.key(q(0.67), Grip(0.3, thumb=0.2), "out")
    clip.left_grip.key(q(0.85), RELAXED, "inout")
    clip.left_grip.key(length, HOLD_PART, "inout")
    clip.look = Track((1.0, 3.0, -1.5))
    clip.look.key(q(0.36), (1.0, 3.6, -1.5), "snap")
    clip.look.key(length, (1.0, 3.0, -1.5), "inout")
    clip.cue(q(0.01), show=part, bone="PropLeft")
    clip.cue(q(0.08), sound=sounds["pick"], volume=0.55, pitch_jitter=0.08)
    clip.cue(q(0.48), sound=sounds["in"], volume=0.8, pitch_jitter=0.04)
    clip.cue(q(0.60), detach=part, blend=0.025)
    if spent is not None:
        # The round the gun held is gone (fired): hidden until the fresh one is home.
        clip.cue(0.0, hide=spent)
    if not stays:
        clip.cue(q(0.63), hide=part)
    clip.part_follow[part] = (grab_after(swap), 0.0, q(0.60), pivot)
    return clip


def pistol():
    # Extend both arms toward the target. The old sight relief put the pistol
    # against the chest and forced a 59-66 degree bend at the firing wrist.
    # Moving along the sight line preserves ADS alignment and the right-hand
    # attachment instead of compensating by rotating the gun inside the grip.
    gun = replace(GUNS["pistol"], relief=0.125,
                  support=palm((-0.035, 0.090, -0.008), (-1.0, 0.15, 0.05), (0.0, 1.0, 0.0)))
    stance = Stance(hip_offset=(0.06, -0.07, 0.0), hip_turn=(2.0, 3.0, 6.0), torso=(-12.0, 0.0, 0.0),
                    aim_torso=(-10.0, 1.0, 0.0), collar_left=(2.0, 12.0), collar_right=(0.0, 4.0),
                    elbows=(30.0, 30.0), aim_elbows=(25.0, 30.0),
                    left_grip=Grip(0.65, thumb=0.12),
                    right_grip=Grip(0.88, trigger=0.4, thumb=0.10, index_joints=(-25.0, 110.0, 80.0)))
    family = Family("Pistol", gun, stance)
    clips = common_clips(family, "Pistol", LENGTHS, "PistolWalk", shoot=(0.018, 5.0, 2.0),
                         aim_shoot=(0.014, 3.0, 1.0))
    for clip in clips:
        if clip.name in ("PistolShoot", "PistolShoulderShoot"):
            slide_blowback(clip, "slide", 0.032)
    clips.append(pistol_reload(family, empty=False))
    clips.append(pistol_reload(family, empty=True))
    for clip in clips:
        keys = []
        for time, grip, easing in clip.right_grip.keys:
            # Fit the pad to the actual trigger island. A generic partly-open
            # fist put the index fingertip six centimetres ahead of it. Slight
            # MCP extension lets the distal joints curl back into the guard.
            pull = max(0.0, min(1.0, ((grip.trigger or 0.4) - 0.4) / 0.15))
            thumb = grip.curl if grip.thumb is None else grip.thumb
            # Keep the thumb alongside the slide; only the explicitly authored
            # magazine/slide-release gestures dip it farther down the frame.
            fitted = replace(grip, index_joints=(-25.0, 110.0 + 4.0 * pull, 80.0 - 2.0 * pull),
                             thumb=0.10 if thumb > 0.30 else thumb)
            keys.append((time, fitted, easing))
        clip.right_grip.keys = keys
    return family, clips


def pistol_reload(family: Family, empty: bool) -> Clip:
    """Present the pistol's side, guide the magazine, and push its base home.

    The magazine stands in the support palm. Fingers guide its forward edge;
    they open for the seating push instead of closing around the firing hand.
    """
    length = LENGTHS["PistolReload"]
    clip = family.clip("PistolReloadEmpty" if empty else "PistolReload", length)
    scale = length / 1.583
    q = lambda time: time * scale
    gun = family.gun
    down = v_scale(v_norm((-0.29, 0.0, 0.96)), -1.0)
    swap = MagSwap(family, "mag", part_pivot("pistol", "mag"),
                   palm((-0.012, 0.113, -0.047), (-0.7, 1.0, 0.1), (0.0, 0.2, 1.0)))
    present = shown_at(family, (-0.012, 0.113, -0.065), (0.56, 0.69), 0.40, (32.0, 6.0, -32.0))
    clip.gun.key(q(0.24), present, "inout")
    clip.gun.key(q(0.80), present, "linear")
    seated = (*present[:1], present[1] + 0.006, present[2] + 0.001,
              present[3], present[4] + 0.8, present[5])
    clip.gun.key(q(0.90), seated, "inout")
    clip.gun.key(q(0.99), present, "inout")
    clip.gun.key(q(1.47), (0.0,) * 6, "inout")
    clip.gun.key(length, (0.0,) * 6, "linear")
    clip.elbows.key(q(0.30), (-50.0, family.stance.elbows[1]), "inout")
    clip.elbows.hold(q(1.02)).key(q(1.45), family.stance.elbows, "inout")
    clip.collar_right.key(q(0.20), (0.0, 30.0), "inout")
    clip.collar_right.hold(q(1.02)).key(q(1.47), family.stance.collar_right, "inout")

    # The thumb drops the depleted magazine; the support hand is already on
    # its way to the pouch, so the old and new magazines never cross its palm.
    clip.part("mag").key(q(0.16), Frame(), "linear")
    clip.part("mag").key(q(0.27), Frame(v_scale(down, 0.08), IDENTITY), "in")
    hand = clip.left
    # Once released, the hand travels independently of the gun's presentation
    # turn. Keeping this key gun-relative dragged the wrist through that turn.
    hand.key(q(0.05), HandAt(clip.gun_frame(0.0).compose(gun.support), "view"), "linear")
    pickup_rotation = clip.gun_frame(q(0.65)).compose(swap.grab).q
    pickup = Frame(in_view(-0.12, -0.39, 0.34), pickup_rotation)
    hand.key(q(0.33), HandAt(pickup, "view"), "inout")
    hand.key(q(0.42), HandAt(pickup, "view"), "linear")
    hand.key(q(0.63), swap.palm(offset=v_scale(down, 0.120)), "out")
    hand.key(q(0.77), swap.palm(offset=v_scale(down, 0.045)), "inout")
    hand.key(q(0.90), swap.palm(), "inout")
    hand.key(q(0.99), swap.palm(offset=v_scale(down, 0.026)), "out")
    # Lower the palm from the floorplate, then turn around the firing knuckles.
    side = Frame(v_add(gun.support.p, (0.025, -0.035, -0.025)), gun.support.q)
    hand.key(q(1.30), HandAt(side), "inout")
    hand.key(q(1.48), HandAt(gun.support), "inout")
    hand.key(length, HandAt(gun.support), "linear")
    grip = clip.left_grip
    grip.key(q(0.13), RELAXED, "inout")
    grip.key(q(0.33), RELAXED, "linear")
    grip.key(q(0.42), Grip(0.42, thumb=0.40), "inout")
    grip.key(q(0.77), Grip(0.42, thumb=0.40), "linear")
    grip.key(q(0.90), Grip(0.18, thumb=0.18), "inout")
    grip.hold(q(0.99))
    grip.key(q(1.30), Grip(0.45, thumb=0.15), "inout")
    grip.key(q(1.48), family.stance.left_grip, "inout")
    right = family.stance.right_grip
    clip.right_grip.key(q(0.10), right, "linear")
    clip.right_grip.key(q(0.16), Grip(right.curl, trigger=right.trigger, thumb=0.22), "inout")
    clip.right_grip.key(q(0.28), right, "inout")
    clip.part_follow["mag_new"] = (grab_after(swap), q(0.42), q(0.90), swap.pivot)
    clip.cue(q(0.02), sound="sounds/cloth_raise", volume=0.3, pitch_jitter=0.04)
    clip.cue(q(0.16), sound="sounds/pistol_mag_release", volume=0.8, pitch_jitter=0.03)
    clip.cue(q(0.19), sound="sounds/pistol_mag_out", volume=0.65, pitch_jitter=0.03)
    clip.cue(q(0.27), drop="mag", velocity=(0.08, -1.4, -0.15), spin=(3.0, 0.5, 1.0), life=4.0)
    clip.cue(q(0.30), sound="sounds/pouch_mag", volume=0.6, pitch_jitter=0.04)
    clip.cue(q(0.42), show="mag_new", bone="PropLeft")
    clip.cue(q(0.77), sound="sounds/pistol_mag_in", volume=0.8, pitch_jitter=0.03)
    clip.cue(q(0.90), sound="sounds/pistol_mag_seat", volume=0.85, pitch_jitter=0.02)
    clip.cue(q(0.90), detach="mag_new", blend=0.035)
    clip.cue(q(1.48), sound="sounds/grip_tap", volume=0.3, pitch_jitter=0.04)
    if empty:
        slide = clip.part("slide")
        slide.key(0.0, Frame((0.032, 0.0, 0.0), IDENTITY), "linear")
        slide.key(q(1.06), Frame((0.032, 0.0, 0.0), IDENTITY), "linear")
        slide.key(q(1.10), Frame(), "in")
        clip.right_grip.key(q(1.00), right, "linear")
        clip.right_grip.key(q(1.07), Grip(right.curl, trigger=right.trigger, thumb=0.30), "inout")
        clip.right_grip.key(q(1.17), right, "inout")
        clip.cue(q(1.09), sound="sounds/pistol_slide_forward", volume=0.9, pitch_jitter=0.02)
    return clip


def slide_blowback(clip, part, travel):
    """The slide (or bolt) snaps back with the shot and returns."""
    track = clip.part(part)
    track.key(0.0, Frame(), "linear")
    track.key(0.025, Frame((travel, 0.0, 0.0), IDENTITY), "snap")
    track.key(0.075, Frame(), "in")


def submachine_gun():
    # Cradle the cylindrical fore-end from below. The former longitudinal
    # finger axis made a claw around the front sight even when its virtual
    # contact point landed exactly. Cross the barrel with the fingers, keep
    # the thumb alongside it, and curl the distal joints around the far side.
    gun = replace(GUNS["smg"], relief=0.02,
                  support=palm((-0.220, 0.107, 0.086), (-0.8, 1.0, 0.2), (0.0, -0.2, 1.0)))
    stance = Stance(hip_offset=(0.07, -0.09, 0.05), hip_turn=(1.5, 4.0, 10.0), torso=(-50.0, 0.0, 0.0),
                    aim_torso=(-50.0, 0.0, 0.0), collar_left=(10.0, 0.0), elbows=(-90.0, 30.0),
                    aim_elbows=(-90.0, 35.0),
                    left_grip=Grip(0.85, thumb=0.0, handle=1.0, thumb_opposition=10.0))
    family = Family("SMG", gun, stance)
    clips = common_clips(family, "SMG", LENGTHS, "SMGWalk", shoot=(0.016, 2.4, 1.6), aim_shoot=(0.012, 1.4, 0.9))
    swap = MagSwap(family, "mag", part_pivot("smg", "mag"),
                   palm((-0.0435, 0.109, -0.12), (-1.0, 0.0, 0.2), (0.0, 1.0, 0.0)))
    sounds = {"release": "sounds/smg_mag_release", "out": "sounds/smg_mag_out", "in": "sounds/smg_mag_in",
              "seat": "sounds/smg_mag_seat"}
    reload = LENGTHS["SMGReload"]
    present = shown_at(family, (-0.044, 0.109, -0.06), (0.55, 0.8), 0.34, (12.0, 8.0, -26.0))
    clips.append(grip_mag_reload(family, swap, "SMGReload", reload, (0.0, 0.0, 1.0), sounds, 1.983,
                                 present=present, travel=0.2))
    # The cylindrical charging knob is behind the fixed front-sight guards.
    # Reach its side with the wrist in line with the forearm; an overhand
    # frame above the sight made the fingers hook around empty space.
    knob = palm(part_pivot("smg", "knob"), (-1.0, 0.0, 0.2), (0.0, 1.0, 0.4))
    rack = slide_rack(family, "knob", 0.06, knob, lift=present,
                      sounds={"back": "sounds/smg_knob_back", "forward": "sounds/smg_knob_forward"},
                      travel=0.06)

    def rack_from_side(clip: Clip, start: float, scale: float) -> None:
        rack(clip, start, scale)
        clip.left.key(start + 0.24 * scale, HandAt(knob), "inout")
        # Let go toward the support side, clearing the receiver. The generic
        # overhand rack exits upward and across the sight line.
        clear = Frame(v_add(knob.p, (0.06, -0.025, -0.015)), knob.q)
        clip.left.key(start + 0.46 * scale, HandAt(clear), "out")

    clips.append(grip_mag_reload(family, swap, "SMGReloadEmpty", reload, (0.0, 0.0, 1.0), sounds, 1.983,
                                 present=present, travel=0.2, empty_hurry=1.0,
                                 empty_tail=rack_from_side))
    for clip in clips:
        if "Reload" not in clip.name:
            continue
        pickup = Frame(in_view(-0.06, -0.36, 0.24), clip.gun_frame(0.60).compose(swap.grab).q)
        clip.left.keys = [(time, HandAt(pickup, "view", hand.arc), easing)
                          if hand.space == "view" and hand.frame in (POUCH, POUCH_LOW)
                          else (time, hand, easing) for time, hand, easing in clip.left.keys]
        # Unwrap the underhand support before reaching the magazine. The old
        # 0.14-second grab left no time to turn this wrist. Share the existing
        # reload duration between that release, the swap, and a slower return;
        # part attachments and their cues use the same clock as the hands.
        scale = clip.length / 1.983

        def handling_time(time: float) -> float:
            if abs(time - clip.length) < 1e-8:
                return clip.length
            reference = time / scale
            if reference <= 0.18:
                return reference * (0.40 / 0.18) * scale
            if reference <= 0.76:
                return (0.40 + (reference - 0.18) * (0.46 / 0.58)) * scale
            return (reference + 0.10) * scale

        tracks = (clip.gun, clip.left, clip.left_grip, clip.right_grip, clip.look,
                  clip.torso, clip.neck, clip.head, clip.collar_left,
                  clip.collar_right, clip.elbows, *clip.parts.values())
        for track in tracks:
            track.keys = [(handling_time(time), value, easing) for time, value, easing in track.keys]
        for cue in clip.cues:
            cue.time = handling_time(cue.time)
        clip.part_follow = {
            name: (grip, handling_time(start), handling_time(end), pivot)
            for name, (grip, start, end, pivot) in clip.part_follow.items()
        }
        support_at = clip.length - (0.10 if clip.name.endswith("Empty") else 0.25) * scale
        clip.left.keys = [
            (support_at if time > 0.5 * scale and hand.space == "gun" and hand.frame == gun.support
             else time, hand, easing) for time, hand, easing in clip.left.keys
        ]
        clip.left.keys.sort(key=lambda key: key[0])
        clip.left_grip.keys = [
            (min(clip.length - 0.03 * scale, support_at + 0.08 * scale)
             if time > 0.5 * scale and grip == stance.left_grip else time, grip, easing)
            for time, grip, easing in clip.left_grip.keys
        ]
        clip.left_grip.keys.sort(key=lambda key: key[0])
        for cue in clip.cues:
            if cue.action.get("sound") == "sounds/grip_tap":
                cue.time = support_at
    return family, clips


def bolt_rifle():
    gun = GUNS["rifle"]
    stance = Stance(hip_offset=(0.11, -0.075, -0.02), hip_turn=(1.0, 4.0, 11.0), torso=(-30.0, 2.0, 0.0),
                    aim_torso=(-26.0, 2.0, 0.0), collar_left=(3.0, 22.0))
    family = Family("Rifle", gun, stance)
    clips = common_clips(family, "Rifle", LENGTHS, "RifleWalk", shoot=(0.03, 4.5, 2.0), aim_shoot=(0.022, 2.6, 1.0))
    clips.append(rifle_bolt_reload(family, "RifleReload", LENGTHS["RifleReload"]))
    return family, clips


def rifle_bolt_reload(family, name, length):
    """Bolt up and back, a stripper clip of rounds pressed into the magazine,
    the empty clip flicked off as the bolt runs home and turns down."""
    clip = family.clip(name, length)
    s = length / 2.383
    q = lambda t: t * s
    gun = family.gun
    bore = v_norm((-0.944, -0.057, 0.325))             # toward the muzzle
    bolt_pivot = part_pivot("rifle", "bolt")
    # Approach the actual knob from above with the fingers across the action.
    # The old target below the handle sent the left forearm through the right.
    handle = palm((-0.080, 0.133, 0.080), (0.0, 1.0, 0.0), (0.0, 0.0, -1.0))

    def bolt(lift, back):
        return part_motion(bolt_pivot, v_scale(bore, -back), q_axis_angle(bore, -lift))

    def bolt_grasp(transform):
        # A round knob swivels in the grasp as the bolt turns. Forcing the
        # wrist to inherit the bolt's axial roll inverts an overhand grip.
        return Frame(held(transform, handle).p, handle.q)

    present = shown_at(family, (-0.12, 0.1, 0.11), (0.49, 0.69), 0.29, (5.0, -2.0, 45.0))
    g = clip.gun
    g.key(q(0.10), (0.004, -0.008, 0.0, -2.0, -2.0, -3.0), "inout")
    g.key(q(0.32), present, "out")
    g.key(q(0.44), (present[0], present[1] + 0.006, present[2], present[3], present[4] + 2.0, present[5] + 2.0), "inout")
    g.key(q(0.58), (present[0] + 0.008, present[1] - 0.006, present[2], present[3], present[4] - 1.0, present[5]), "snap")
    g.key(q(0.95), (present[0], present[1] - 0.01, present[2], present[3] - 3.0, present[4] - 2.0, present[5] - 6.0),
          "inout")
    g.key(q(1.30), present, "inout")
    g.key(q(1.50), (present[0], present[1] - 0.01, present[2], present[3], present[4] - 2.5, present[5]), "inout")
    g.key(q(1.62), present, "out")
    g.key(q(1.92), (present[0], present[1] + 0.012, present[2] + 0.01, present[3], present[4] + 3.5, present[5]), "snap")
    g.key(q(2.02), (present[0], present[1] - 0.004, present[2], present[3], present[4], present[5] - 1.0), "snap")
    g.key(q(2.22), (0.003, -0.003, 0.0, -1.0, -1.0, 2.0), "inout")
    g.key(length, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0), "settle")

    lifted = bolt(62.0, 0.0)
    opened = bolt(62.0, 0.075)
    hand = clip.left
    # Turn through a neutral side-on palm outside the receiver. Interpolating
    # directly from the cradle to the overhand knob takes the wrist through
    # the pronation seam and leaves the sleeve rotating in the wrong direction.
    hand.key(q(0.13), HandAt(palm((-.20, -.04, .18), (0., 0., 1.), (0., 1., 0.))), "linear")
    hand.key(q(0.24), HandAt(Frame((-.13, .035, .16), handle.q)), "inout")
    hand.key(q(0.34), HandAt(handle), "inout")
    hand.key(q(0.44), HandAt(bolt_grasp(lifted)), "snap")
    hand.key(q(0.58), HandAt(bolt_grasp(opened)), "snap")
    hand.key(q(0.66), HandAt(Frame(v_add(held(opened, handle).p, (-.005, -.07, .04)), handle.q)), "out")
    clip_grab = palm((-0.13, 0.085, 0.16), (-.4, .916, 0.0), (0.0, 0.0, -1.0))
    # A chest pouch keeps the stripper clip aligned for its top-down insertion.
    # Taking this palm orientation to the low belt forced a wrist inversion.
    pickup = Frame(in_view(-.14, -.28, .30), clip.gun_frame(q(1.30)).compose(clip_grab).q)
    hand.key(q(0.92), HandAt(pickup, "view"), "inout")
    hand.key(q(1.00), HandAt(pickup, "view"), "inout")
    lowered = part_motion(part_pivot("rifle", "clip"), (0.0, -0.07, 0.06), IDENTITY)
    hand.key(q(1.30), HandAt(held(lowered, clip_grab), arc=(0.0, -0.05, 0.0)), "out")
    hand.key(q(1.40), HandAt(clip_grab), "inout")
    thumb = Frame(v_add(clip_grab.p, (0.0, 0.0, -0.02)), clip_grab.q)
    hand.key(q(1.62), HandAt(thumb), "inout")
    hand.key(q(1.72), HandAt(Frame(v_add(bolt_grasp(opened).p, (0., -.06, .04)), handle.q)), "inout")
    hand.key(q(1.80), HandAt(bolt_grasp(opened)), "inout")
    hand.key(q(1.92), HandAt(bolt_grasp(lifted)), "snap")
    hand.key(q(2.02), HandAt(handle), "snap")
    hand.key(q(2.12), HandAt(palm((-.24, .03, .16), (-.574, 0., .819), (0., 1., 0.))), "linear")
    hand.key(q(2.30), HandAt(gun.support), "linear")

    grip = clip.left_grip
    grip.key(q(0.10), OPEN, "inout")
    grip.key(q(0.30), Grip(0.3, spread=0.2), "inout")
    grip.key(q(0.36), Grip(0.7, thumb=0.5), "snap")
    grip.key(q(0.60), Grip(0.7, thumb=0.5), "linear")
    grip.key(q(0.66), Grip(0.1, spread=0.6), "snap")
    grip.key(q(0.98), HOLD_PART, "inout")
    grip.key(q(1.40), HOLD_PART, "linear")
    grip.key(q(1.46), Grip(0.55, thumb=0.0), "inout")
    grip.key(q(1.62), Grip(0.6, thumb=0.0), "linear")
    grip.key(q(1.74), Grip(0.3, spread=0.2), "inout")
    grip.key(q(1.80), Grip(0.7, thumb=0.5), "snap")
    grip.key(q(2.04), Grip(0.7, thumb=0.5), "linear")
    grip.key(q(2.10), Grip(0.2), "snap")
    grip.key(q(2.30), family.stance.left_grip, "inout")

    track = clip.part("bolt")
    track.key(q(0.36), Frame(), "linear")
    track.key(q(0.44), track_value(lifted, bolt_pivot), "snap")
    track.key(q(0.58), track_value(opened, bolt_pivot), "snap")
    track.key(q(1.82), track_value(opened, bolt_pivot), "linear")
    track.key(q(1.92), track_value(lifted, bolt_pivot), "snap")
    track.key(q(2.02), Frame(), "snap")

    clip_pivot = part_pivot("rifle", "clip")
    stripper = clip.part("clip")
    stripper.key(q(1.40), Frame(), "linear")
    stripper.key(q(1.62), Frame((0.0, 0.0, -0.035), IDENTITY), "inout")
    clip.part_follow["clip"] = (
        Frame(PALM["Left"], IDENTITY).compose(clip_grab.inverse()).compose(Frame(clip_pivot, IDENTITY)),
        q(0.96), q(1.40), clip_pivot)

    look = clip.look
    look.key(q(0.40), (-1.5, 3.0, 2.0), "inout")
    look.key(q(1.00), (0.0, 1.0, 0.0), "inout")
    look.key(q(1.50), (-1.0, 5.0, 1.5), "inout")
    look.key(q(1.95), (-1.0, 3.0, 1.0), "inout")
    look.key(q(2.30), (0.0, 0.0, 0.0), "inout")

    clip.cue(q(0.02), sound="sounds/cloth_raise", volume=0.4, pitch_jitter=0.05)
    clip.cue(q(0.42), sound="sounds/rifle_bolt_up", volume=0.9, pitch_jitter=0.03)
    clip.cue(q(0.55), sound="sounds/rifle_bolt_back", volume=1.0, pitch_jitter=0.03)
    clip.cue(q(0.56), kick={"pitch": 0.4, "seconds": 0.2})
    clip.cue(q(0.90), sound="sounds/pouch_clip", volume=0.7, pitch_jitter=0.05)
    clip.cue(q(0.96), show="clip", bone="PropLeft")
    clip.cue(q(1.40), detach="clip", blend=0.05)
    clip.cue(q(1.38), sound="sounds/rifle_clip_seat", volume=0.9, pitch_jitter=0.04)
    clip.cue(q(1.48), sound="sounds/rifle_rounds_in", volume=0.95, pitch_jitter=0.03)
    clip.cue(q(1.86), drop="clip", velocity=(0.6, 1.2, -0.2), spin=(3.0, 8.0, 1.0), life=3.0)
    clip.cue(q(1.86), sound="sounds/rifle_bolt_forward", volume=1.0, pitch_jitter=0.03)
    clip.cue(q(2.00), sound="sounds/rifle_bolt_down", volume=1.0, pitch_jitter=0.03)
    clip.cue(q(2.00), kick={"pitch": -0.6, "yaw": 0.3, "seconds": 0.25})
    clip.cue(q(2.25), sound="sounds/grip_tap", volume=0.4, pitch_jitter=0.06)
    return clip


SHOTGUN_AXIS = v_norm((0.9615, -0.1373, 0.2381))      # from the muzzle toward the stock (Modern model)


def pump_shotgun(model="modern/shotgun"):
    """The pump shotgun. Modern has its own hold clips (PumpShotgunIdle, ...);
    Classic borrows the assault rifle's (Gun*), so the Classic model gets
    `Gun*@shotgun` holds (only Classic ever plays them with a shotgun) and
    `PumpShotgun*@classic/shotgun` fire and reload clips."""
    classic = model.startswith("classic")
    gun = GUNS[model]
    stance = Stance(hip_offset=(0.11, -0.075, -0.02), hip_turn=(1.0, 4.0, 11.0), torso=(-30.0, 2.0, 0.0),
                    aim_torso=(-26.0, 2.0, 0.0), collar_left=(3.0, 24.0), elbows=(5.0, 25.0))
    family = Family("PumpShotgun", gun, stance)
    lengths = dict(LENGTHS)
    axis = (1.0, 0.0, 0.0) if classic else SHOTGUN_AXIS
    if classic:
        holds = [c for c in common_clips(family, "Gun", lengths, "GunRun")
                 if c.name in ("GunIdle", "GunRun", "GunShoulder", "GunShoulderIdle", "GunShoulderWalk")]
        for clip in holds:
            clip.name += "@shotgun"
        clips = holds + [c for c in common_clips(family, "PumpShotgun", lengths, "PumpShotgunRun")
                         if c.name in ("PumpShotgunShoot", "PumpShotgunShoulderShoot")]
    else:
        clips = common_clips(family, "PumpShotgun", lengths, "PumpShotgunRun", shoot=(0.05, 8.0, 3.0),
                             aim_shoot=(0.04, 5.0, 2.0))
    for clip in clips:
        if clip.name in ("PumpShotgunShoot", "PumpShotgunShoulderShoot"):
            clip.gun = Track((0.0,) * 6)
            aimed_shot = "Shoulder" in clip.name
            kick = 0.04 if aimed_shot else 0.05
            rise = 5.0 if aimed_shot else 8.0
            clip.gun.key(0.033, (0.0, 0.003, kick, 0.0, rise, 2.5), "snap")
            clip.gun.key(0.20, (0.0, -0.004, 0.006, 0.0, -1.0, 0.5), "out")
            clip.gun.key(0.26, (0.0, -0.01, 0.0, 1.0, -2.0, 3.0), "inout")
            clip.gun.key(0.36, (0.0, -0.008, 0.01, 1.0, -1.0, 2.5), "inout")
            clip.gun.key(0.48, (0.0, -0.002, -0.004, 0.0, 1.0, 0.0), "snap")
            clip.gun.key(clip.length, (0.0,) * 6, "settle")
            pump_action(clip, family, 0.22, axis, 0.075)
            clip.cue(0.34, sound="sounds/shell_eject", volume=0.5, pitch_jitter=0.1)
    shell_pivot = part_pivot_at(model, "shell")
    # A shell is pinched between the index and thumb, not buried in the
    # magazine fist. Fit the actual stock/avatar finger surfaces in wrist
    # space, then derive the virtual palm target from that contact. The
    # insertion push finishes at the existing shell socket.
    shell_in_hand = (0.110, -0.080, 0.045)
    shell_grip = Grip(0.50, thumb=0.55)
    orientation = hand_frame((-0.6, 1.0, 0.2), (0.0, 1.0, 0.3))
    # Modern's loading gate and shell are tilted fourteen degrees relative
    # to model X. Carry that authored mesh orientation into the same pinch.
    shell_rotation = next(part.get("rotation", (0.0, 0.0, 0.0)) for part in PARTS[model]
                          if part["name"] == "shell")
    orientation = q_mul(q_axis_angle(Y, shell_rotation[1]), orientation)
    push = (-0.035, 0.0, 0.0)
    grab = Frame(v_sub(v_add(shell_pivot, q_rotate(orientation, v_sub(PALM["Left"], shell_in_hand))),
                       push), orientation)
    loading = shown_at(family, shell_pivot, (0.52, 0.66), 0.30,
                       (12.0, 8.0, -15.0 if classic else -25.0))
    presentation = family.clip("shell-presentation", 0.583)
    presentation.gun = Track(loading)
    # Keep the shell oriented for insertion while it comes from the pouch.
    # Using the idle gun here forced a rapid wrist turn into the tilted gun.
    source = Frame(in_view(-0.16, -0.32, 0.19), presentation.gun_frame(0.0).compose(grab).q)
    reload_clip = round_reload(
        family, "PumpShotgunReload", 0.583, "shell", shell_pivot, grab,
        approach=dict(offset=(0.04, -0.03, -0.08), tip=-10.0), port_motion=dict(offset=(0.02, 0.0, -0.01)),
        sounds={"pick": "sounds/shell_pick", "in": "sounds/shell_in"}, reference=0.583,
        loading=loading, push=dict(offset=push), source=source)
    reload_clip.elbows = Track((0.0, family.stance.elbows[1]))
    reload_clip.left_grip = Track(shell_grip).key(0.583 * 0.60, shell_grip, "linear") \
        .key(0.583 * 0.73, Grip(0.3, thumb=0.1), "out") \
        .key(0.583 * 0.85, RELAXED, "inout").key(0.583, shell_grip, "inout")
    clips.append(reload_clip)
    if classic:
        for clip in clips:
            if not clip.name.endswith("@shotgun"):
                clip.name += "@classic/shotgun"
    return family, clips


def classic_pump_shotgun():
    return pump_shotgun("classic/shotgun")


def machine_gun():
    gun = GUNS["lmg"]
    stance = Stance(hip_offset=(0.12, -0.09, -0.01), hip_turn=(1.0, 3.0, 9.0), torso=(-30.0, 3.0, 0.0),
                    aim_torso=(-40.0, 2.0, 0.0), collar_left=(3.0, 24.0),
                    elbows=(-50.0, 30.0), aim_elbows=(-50.0, 45.0), left_grip=Grip(.70, thumb=.4))
    family = Family("LMG", gun, stance)
    clips = common_clips(family, "LMG", LENGTHS, "LMGWalk", shoot=(0.02, 2.0, 1.5), aim_shoot=(0.014, 1.2, 0.8))
    clips.append(lmg_reload(family, "LMGReload", LENGTHS["LMGReload"]))
    return family, clips


def lmg_reload(family, name, length):
    """The long one: feed cover up, the spent box off, a fresh box from the
    pouch, the belt laid on the tray, cover closed and the heavy gun regripped."""
    clip = family.clip(name, length)
    s = length / 9.683
    q = lambda t: t * s
    gun = family.gun
    box_pivot = part_pivot_at("lmg", "box")
    cover_pivot = part_pivot_at("lmg", "cover")
    grab = palm((-0.155, 0.16, -0.12), (-1.0, 0.15, 0.15), (0.0, 0.0, 1.0))
    swap = MagSwap(family, "box", box_pivot, grab)
    cover_open = part_motion(cover_pivot, (0.0, 0.0, 0.0), q_axis_angle(Y, -60.0))
    # Fingers hook over the edge from below. A forward-pointing closed grasp
    # rotates the fingertips downward as the cover opens and inverts the wrist.
    latch = palm((-0.06, 0.105, 0.15), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0))
    lid = held(cover_open, latch)
    tray = palm((-0.16, 0.11, 0.145), (0.0, 1.0, 0.0), (0.0, 0.0, -1.0))
    present = shown_at(family, (-0.16, 0.15, -0.02), (0.49, 0.74), 0.36, (10.0, 4.0, -22.0))
    top = shown_at(family, (-0.15, 0.13, 0.15), (0.49, 0.74), 0.36, (6.0, 2.0, 12.0))

    g = clip.gun
    g.key(q(0.40), present, "out")
    g.key(q(0.80), top, "inout")
    g.key(q(1.30), (top[0], top[1] + 0.01, top[2], top[3], top[4] + 3.0, top[5]), "inout")
    g.key(q(1.60), top, "settle")
    g.key(q(2.20), present, "inout")
    g.key(q(2.60), (present[0], present[1] + 0.012, present[2], present[3], present[4] + 3.0, present[5] - 3.0), "out")
    g.key(q(3.40), (present[0] + 0.01, present[1] - 0.02, present[2], present[3] - 5.0, present[4] - 2.0, present[5] + 8.0),
          "inout")
    g.key(q(4.40), present, "inout")
    g.key(q(4.80), (present[0], present[1] + 0.016, present[2], present[3], present[4] + 4.0, present[5] - 2.0), "snap")
    g.key(q(5.10), present, "out")
    g.key(q(5.60), top, "inout")
    g.key(q(6.40), top, "linear")
    g.key(q(6.80), (top[0], top[1] - 0.02, top[2], top[3], top[4] - 5.0, top[5]), "snap")
    g.key(q(7.10), top, "settle")
    g.key(q(8.30), (0.012, -0.012, 0.0, -2.0, -2.0, 5.0), "inout")
    g.key(q(9.20), (0.002, -0.002, 0.0, -.5, -.5, 1.0), "inout")
    g.key(length, (0.0,) * 6, "settle")

    hand = clip.left
    hand.key(q(0.20), HandAt(gun.support), "linear")
    hand.key(q(0.60), HandAt(Frame((-.10, .025, .17), latch.q)), "inout")
    hand.key(q(0.80), HandAt(latch), "inout")
    hand.key(q(0.95), HandAt(latch), "linear")
    hand.key(q(1.40), HandAt(lid), "out")
    hand.key(q(1.65), HandAt(Frame(v_add(lid.p, (0.0, -.08, .015)), lid.q)), "out")
    hand.key(q(2.05), HandAt(Frame((-.155, .025, -.15), grab.q)), "inout")
    hand.key(q(2.30), HandAt(grab), "inout")
    hand.key(q(2.45), swap.palm(offset=(0.0, 0.0, 0.004)), "inout")
    hand.key(q(2.80), swap.palm(offset=(-0.01, 0.02, -0.06), tip=10.0), "inout")
    hand.key(q(3.00), swap.palm(offset=(-0.015, 0.025, -0.075), tip=12.0), "in")
    hand.key(q(3.40), HandAt(POUCH, "view", arc=(-0.04, 0.0, 0.02)), "inout")
    hand.key(q(3.55), HandAt(POUCH_LOW, "view"), "inout")
    hand.key(q(4.30), swap.palm(offset=(-0.01, 0.02, -0.075), tip=12.0), "out")
    hand.key(q(4.60), swap.palm(offset=(0.0, 0.005, -0.02), tip=4.0), "inout")
    hand.key(q(4.80), swap.palm(), "snap")
    hand.key(q(5.10), HandAt(Frame(v_add(grab.p, (0.0, 0.0, -0.03)), grab.q)), "out")
    # Clear the box with the palm facing inward, then turn it down above the
    # receiver. Applying the flat tray orientation below the gun folded the
    # wrist and forced an 81-degree correction while the palm point still fit.
    tray_clear = palm((-.16, -.025, 0.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0))
    hand.key(q(5.28), HandAt(tray_clear), "inout")
    hand.key(q(5.43), HandAt(Frame((-.16, -.025, .20), q_slerp(tray_clear.q, tray.q, .5))), "inout")
    hand.key(q(5.60), HandAt(tray), "inout")
    hand.key(q(6.00), HandAt(Frame(v_add(tray.p, (0.03, 0.0, 0.0)), tray.q)), "inout")
    hand.key(q(6.20), HandAt(Frame(v_add(lid.p, (0.0, -.07, .02)), lid.q)), "inout")
    hand.key(q(6.40), HandAt(lid), "inout")
    hand.key(q(6.80), HandAt(latch), "snap")
    hand.key(q(7.00), HandAt(Frame(v_add(latch.p, (0.0, -.08, .025)), latch.q)), "out")
    # Clear the receiver with the palm inward before turning up under the
    # fore-end. Applying the cradle orientation while still beside the box
    # forces a wrist bend even though the requested palm point is reachable.
    hand.key(q(7.80), HandAt(palm((-.22, .025, .07), (-1.0, 0.0, .2), (0.0, 1.0, 0.0))), "inout")
    hand.key(q(8.70), HandAt(gun.support), "inout")

    grip = clip.left_grip
    grip.key(q(0.30), OPEN, "inout")
    grip.key(q(0.85), Grip(0.6, thumb=0.4), "inout")
    grip.key(q(1.50), Grip(0.6, thumb=0.4), "linear")
    grip.key(q(1.70), OPEN, "inout")
    grip.key(q(2.40), HOLD_PART, "inout")
    grip.key(q(2.95), HOLD_PART, "linear")
    grip.key(q(3.05), Grip(0.05, spread=0.9), "snap")
    grip.key(q(3.40), RELAXED, "inout")
    grip.key(q(3.55), HOLD_PART, "inout")
    grip.key(q(4.85), HOLD_PART, "linear")
    grip.key(q(5.10), Grip(0.3, spread=0.2), "inout")
    grip.key(q(5.43), Grip(0.12, thumb=0.25), "inout")
    grip.key(q(6.05), Grip(0.12, thumb=0.25), "linear")
    grip.key(q(6.38), Grip(0.6, thumb=0.4), "inout")
    grip.key(q(6.80), Grip(0.6, thumb=0.4), "linear")
    grip.key(q(7.20), RELAXED, "inout")
    grip.key(q(8.00), RELAXED, "linear")
    grip.key(q(8.70), family.stance.left_grip, "inout")

    cover = clip.part("cover")
    cover.key(q(0.95), Frame(), "linear")
    cover.key(q(1.40), track_value(cover_open, cover_pivot), "out")
    cover.key(q(6.40), track_value(cover_open, cover_pivot), "linear")
    cover.key(q(6.80), Frame(), "in")
    # Bake the hand along the same hinge trajectory as the cover. Independent
    # interpolation between the end contacts follows a chord through the lid.
    for begin, end in ((.95, 1.40), (6.40, 6.80)):
        for index in range(math.ceil(q(begin) * FRAME_RATE), math.floor(q(end) * FRAME_RATE) + 1):
            time = index / FRAME_RATE
            value = cover.value(time)
            hand.key(time, HandAt(held(part_motion(cover_pivot, value.p, value.q), latch)), "linear")

    look = clip.look
    look.key(q(0.80), (-1.0, 4.0, 1.0), "inout")
    look.key(q(2.40), (2.0, 5.0, -2.0), "inout")
    look.key(q(3.50), (0.0, 1.5, 0.0), "inout")
    look.key(q(4.70), (2.0, 5.0, -2.0), "inout")
    look.key(q(5.80), (-1.0, 5.5, 1.0), "inout")
    look.key(q(6.80), (-1.0, 4.0, 1.0), "snap")
    look.key(q(8.20), (-1.5, 3.0, 2.0), "inout")
    look.key(q(9.20), (0.0, 0.0, 0.0), "inout")
    torso = family.stance.torso
    clip.torso.key(q(1.0), (torso[0] - 3.0, torso[1] + 4.0, torso[2]), "inout")
    clip.torso.key(q(3.4), (torso[0] - 1.0, torso[1] + 5.0, torso[2]), "inout")
    clip.torso.key(q(6.0), (torso[0] - 4.0, torso[1] + 4.0, torso[2]), "inout")
    clip.torso.key(q(9.2), torso, "inout")

    clip.cue(q(0.05), sound="sounds/cloth_raise", volume=0.5, pitch_jitter=0.05)
    clip.cue(q(0.98), sound="sounds/lmg_cover_open", volume=1.0, pitch_jitter=0.03)
    clip.cue(q(2.42), attach="box", bone="PropLeft")
    clip.cue(q(2.50), sound="sounds/lmg_box_out", volume=0.95, pitch_jitter=0.04)
    clip.cue(q(3.02), drop="box", velocity=(-0.6, -1.0, -0.5), spin=(3.0, 1.0, 2.0), life=5.0)
    clip.cue(q(3.45), sound="sounds/mag_hit_ground", volume=0.35, pitch_jitter=0.06, who="self")
    clip.cue(q(3.35), sound="sounds/pouch_box", volume=0.85, pitch_jitter=0.05)
    clip.cue(q(3.60), show="box_new", bone="PropLeft")
    clip.cue(q(4.62), sound="sounds/lmg_box_in", volume=0.95, pitch_jitter=0.04)
    clip.cue(q(4.80), sound="sounds/ar_mag_seat", volume=0.8, pitch=0.8, pitch_jitter=0.03)
    clip.cue(q(4.80), detach="box_new", blend=0.06)
    clip.cue(q(4.80), kick={"pitch": -0.6, "seconds": 0.25})
    clip.cue(q(5.55), sound="sounds/lmg_belt", volume=0.9, pitch_jitter=0.05)
    clip.cue(q(6.78), sound="sounds/lmg_cover_close", volume=1.0, pitch_jitter=0.03)
    clip.cue(q(6.78), kick={"pitch": 1.0, "seconds": 0.3})
    clip.cue(q(7.60), sound="sounds/cloth_lower", volume=0.45, pitch_jitter=0.05)
    clip.cue(q(8.70), sound="sounds/grip_tap", volume=0.45, pitch_jitter=0.05)
    clip.part_follow["box"] = (grab_after(swap), q(2.42), q(3.02), box_pivot)
    clip.part_follow["box_new"] = (grab_after(swap), q(3.55), q(4.80), box_pivot)
    return clip


def rocket_launcher():
    gun = GUNS["rpg"]
    stance = Stance(hip_offset=(0.10, -0.06, 0.02), hip_turn=(1.0, 3.0, 4.0), torso=(-25.0, 2.0, 0.0),
                    aim_torso=(-22.0, 1.0, 0.0), collar_left=(3.0, 20.0), collar_right=(6.0, 0.0),
                    elbows=(10.0, 40.0), aim_elbows=(8.0, 45.0))
    family = Family("RPG", gun, stance)
    clips = []
    idle = family.clip("RPGIdle", LENGTHS["RPGIdle"])
    idle.looping = True
    clips.append(idle)
    walk = family.clip("RPGWalk", LENGTHS["RPGWalk"])
    period = walk.length
    loop_keys(walk.gun, period, lambda t: (wave(t, period, 0.004), -abs(wave(t, period, 0.006, 0.25)), 0.0,
                                           wave(t, period, 0.6), wave(t, period, 0.5, 0.25), wave(t, period, 1.2)))
    clips.append(walk)
    for name, aimed_base in (("RPGShoot", False), ("PumpShotgunShoulderShoot@rpg", True)):
        fire = family.clip(name, 0.983, aimed_base=aimed_base)
        fire.gun = Track((0.0,) * 6)
        fire.gun.key(0.04, (0.0, 0.006, 0.07, 0.0, 9.0, 2.0), "snap")
        fire.gun.key(0.30, (0.0, -0.006, 0.01, 0.0, -1.5, 0.5), "out")
        fire.gun.key(0.983, (0.0,) * 6, "settle")
        fire.cue(0.0, hide="rocket")
        fire.cue(0.0, kick={"pitch": 2.0, "seconds": 0.45})
        clips.append(fire)
    lift = family.clip("GunShoulder@rpg", LENGTHS["GunShoulder"], aimed_base=True)
    lift.gun = Track(family.hip_in_ads()).key(lift.length, (0.0,) * 6, "out")
    lift.torso = Track(stance.torso).key(lift.length, stance.aim_torso, "inout")
    clips.append(lift)
    for name in ("GunShoulderIdle@rpg", "GunShoulderWalk@rpg"):
        still = family.clip(name, LENGTHS[name.split("@")[0]], aimed_base=True)
        still.looping = True
        clips.append(still)
    rocket = part_pivot("rpg", "rocket_new")
    grab = palm((-0.40, 0.1, -0.005), (-0.2, 1.0, 0.3), (0.0, 0.0, 1.0))
    clips.append(rocket_reload(family, rocket, grab))
    return family, clips


def rocket_reload(family: Family, pivot: tuple, grab: Frame) -> Clip:
    """One fast front-load: guide the tail into the tube, seat, twist, regrip.

    Retail allows only 0.567 seconds, so the round starts already palmed below
    the muzzle. A shoulder-pouch fetch cannot honestly fit into that interval.
    """
    clip = family.clip("PumpShotgunReload@rpg", 0.583)
    q = lambda fraction: fraction * clip.length
    swap = MagSwap(family, "rocket_new", pivot, grab)
    loading = shown_at(family, (-0.42, 0.10, 0.06), (0.44, 0.67), 0.30, (27.0, -7.0, -8.0))
    clip.gun.key(q(0.22), loading, "inout").hold(q(0.62))
    clip.gun.key(q(0.75), tuple(v + d for v, d in zip(loading, (0, -.005, -.004, 0, -1, 0))), "out")
    clip.gun.key(clip.length, (0.0,) * 6, "smooth")
    # Start close to the bore, with the tail leading. Hands never cross the eye.
    clip.left = Track(HandAt(Frame(in_view(-.12, -.27, .24), clip.gun_frame(q(.22)).compose(grab).q), "view"))
    clip.left.key(q(0.28), swap.palm(offset=(-0.075, 0.0, 0.0)), "inout")
    clip.left.key(q(0.57), swap.palm(roll=-12.0), "out")
    clip.left.key(q(0.69), swap.palm(), "inout")
    clip.left.key(q(0.80), HandAt(Frame(v_add(grab.p, (0.0, -.04, -.04)), grab.q)), "out")
    clip.left.key(clip.length, HandAt(family.gun.support, arc=(-.025, -.025, 0)), "smooth")
    clip.left_grip = Track(Grip(.65, thumb=.55)).hold(q(.67))
    clip.left_grip.key(q(.79), RELAXED, "out").key(clip.length, family.stance.left_grip)
    clip.part_follow["rocket_new"] = (grab_after(swap), 0.0, q(.69), pivot)
    clip.cue(0.0, hide="rocket")
    clip.cue(0.0, show="rocket_new", bone="PropLeft")
    clip.cue(q(.12), sound="sounds/rpg_rocket_pick", volume=.55, pitch_jitter=.03)
    clip.cue(q(.54), sound="sounds/rpg_rocket_in", volume=.85, pitch_jitter=.02)
    clip.cue(q(.69), detach="rocket_new", blend=.025)
    clip.cue(q(.87), sound="sounds/grip_tap", volume=.30, pitch_jitter=.04)
    return clip


@dataclass(frozen=True)
class EnergyAction:
    """Each receiver's cell handling, independent of firearm magazine logic."""
    roll: float
    yaw: float
    withdraw: tuple
    grab: Frame
    prime: Frame
    release: float
    seat: float
    twist: float
    long_charge: bool = False
    depth: float = 0.29
    pickup: tuple = (-0.18, -0.37, 0.16)
    prime_grip: Grip = field(default_factory=lambda: Grip(.30, trigger=.08, thumb=.2))


def energy_reload(family: Family, name: str, length: float, action: EnergyAction,
                  empty: bool = False) -> Clip:
    """Vent, unlock, extract the cell, dock a replacement, lock and energize."""
    clip = family.clip(name, length)
    q = lambda fraction: fraction * length
    pivot = part_pivot(family.gun.model, "cell")
    swap = MagSwap(family, "cell", pivot, action.grab)
    show = action.release + .16
    seat = action.seat - (.035 if empty else 0)
    loading = shown_at(family, pivot, (.51, .70), action.depth, (action.yaw, 4.0, action.roll))
    clip.gun.key(q(.14), loading, "smooth").hold(q(seat - .08))
    clip.gun.key(q(seat), tuple(v + d for v, d in zip(loading, (0, .006, .003, 0, 1.0, -1))), "out")
    clip.gun.key(q(.83), loading, "smooth").key(length, (0.0,) * 6, "smooth")
    # Align the fingers to the cell, with a measured partial turn to disengage
    # its contacts. The hand then follows the cell, not the other way around.
    hand = clip.left
    hand.hold(q(.035)).key(q(.17), swap.palm(), "smooth")
    hand.key(q(action.release), swap.palm(roll=action.twist), "inout")
    hand.key(q(action.release + .085), swap.palm(offset=action.withdraw, roll=action.twist), "inout")
    pouch = Frame(in_view(*action.pickup), clip.gun_frame(q(seat)).compose(action.grab).q)
    hand.key(q(show), HandAt(pouch, "view", arc=(-.025, -.02, 0)), "inout")
    hand.hold(q(show + .045))
    hand.key(q(seat - .11), swap.palm(offset=v_scale(action.withdraw, .55), roll=action.twist), "inout")
    hand.key(q(seat - .045), swap.palm(roll=action.twist), "out")
    hand.key(q(seat), swap.palm(), "inout")
    # A palm/thumb contact closes the charging circuit. No bolt-rack gesture.
    hand.key(q(.81), HandAt(Frame(action.prime.p, action.grab.q), arc=(-.02, -.025, 0)), "smooth")
    hand.hold(q(.86 if action.long_charge or empty else .82))
    hand.key(length, HandAt(family.gun.support, arc=(-.02, -.015, 0)), "smooth")
    grip = clip.left_grip
    grip.key(q(.09), RELAXED).key(q(.17), HOLD_PART)
    grip.hold(q(action.release + .07)).key(q(action.release + .11), OPEN, "out")
    grip.key(q(show + .025), HOLD_PART).hold(q(seat))
    grip.key(q(.78), action.prime_grip, "out")
    grip.key(length, family.stance.left_grip, "smooth")
    old_end = q(action.release + .095)
    clip.part_follow["cell"] = (grab_after(swap), q(.17), old_end, pivot)
    clip.part_follow["cell_new"] = (grab_after(swap), q(show), q(seat), pivot)
    clip.cue(q(.055), sound="sounds/laser_vent", volume=.42, pitch_jitter=.025)
    clip.cue(q(.17), attach="cell", bone="PropLeft")
    clip.cue(q(action.release), sound="sounds/cell_release", volume=.70, pitch_jitter=.03)
    clip.cue(q(action.release + .04), sound="sounds/cell_eject", volume=.65, pitch_jitter=.03)
    clip.cue(old_end, drop="cell", velocity=(-.35, -.65, -.1), spin=(2., .5, 1.), life=3.)
    clip.cue(q(show), sound="sounds/pouch_cell", volume=.55, pitch_jitter=.04)
    clip.cue(q(show + .015), show="cell_new", bone="PropLeft")
    clip.cue(q(seat - .045), sound="sounds/cell_in", volume=.75, pitch_jitter=.025)
    clip.cue(q(seat), sound="sounds/cell_seat", volume=.8, pitch_jitter=.02)
    clip.cue(q(seat), detach="cell_new", blend=.035)
    clip.cue(q(.80), sound="sounds/cell_prime", volume=.55, pitch_jitter=.02)
    clip.cue(q(.83), sound="sounds/cell_charge_precision" if action.long_charge else "sounds/cell_charge_compact",
             volume=.58 if empty else .48, pitch_jitter=.015)
    if family.gun.model in ("space_ak", "space_rifle"):
        # The cradle and cell grasp use different forearm rolls. Spend the
        # whole existing approach/release interval on that turn: a smooth
        # two-key curve concentrated it into the middle and outran the wrist's
        # 12-degree-per-frame limit, despite both endpoint grips being valid.
        # Keep the original elbow preference throughout actual cell contact.
        clip.elbows.key(q(.17), (-50.0, family.stance.elbows[1]), "linear")
        clip.elbows.hold(q(.86)).key(length, family.stance.elbows, "linear")
        hand.keys = [key for key in hand.keys if abs(key[0] - q(.035)) > 1e-8]
        hand.key(length, HandAt(family.gun.support), "linear")
        # Evaluate the same continuous gun track that prepare_choreography
        # exports. View-space samples keep the hand turn even while the gun
        # rolls underneath it, with the contact and release times untouched.
        clip.gun.flow = True
        for begin, end in ((0.0, q(.17)),
                           (q(.86 if action.long_charge or empty else .82), length)):
            start = clip.left_frame(begin, clip.gun_frame(begin))
            finish = clip.left_frame(end, clip.gun_frame(end))
            for index in range(1, 16):
                blend = index / 16.0
                hand.key(begin + (end - begin) * blend,
                         HandAt(lerp_frame(start, finish, blend), "view"), "linear")
    return clip


def laser_rifle_family(key, model, prefix, walk_name, reload_length, reference, sounds_prefix="cell"):
    gun = GUNS[model]
    family = Family(key, gun, Stance(hip_offset=(.10, -.075, .01), hip_turn=(1., 3., 8.)))
    precision = model == "space_rifle"
    clips = common_clips(family, prefix, LENGTHS, walk_name,
                         shoot=(.010 if precision else .008, 1.35, .3), aim_shoot=(.006, .7, .2))
    pivot = part_pivot(model, "cell")
    # Assault: pull down then twist-lock. Precision: roll the receiver further
    # outward and seat with a deliberate contact check and longer charge hold.
    # These are the centres of the complete cell mesh islands. The previous
    # below/side contact made the fingers grasp air, and its downward-facing
    # wrist forced the solver to rotate the palm away by over 150 degrees.
    centre = (-.172115, .073134, -.004449) if precision else (-.175827, .077877, -.008448)
    grab = palm(centre, (-.966, 0., .259), (0., 1., 0.))
    action = EnergyAction(
        roll=-44. if precision else -24., yaw=8. if precision else 14.,
        withdraw=(.005, -.025 if precision else 0., -.085),
        grab=grab,
        # Keep the fingers around the seated cell for a short thumb contact;
        # reaching up across the receiver used to turn an open hand over it.
        prime=Frame(v_add(centre, (.005, -.005, .012)), grab.q),
        release=.25 if precision else .21, seat=.68 if precision else .64,
        twist=25. if precision else 12., long_charge=precision,
        depth=.40 if precision else .36, pickup=(-.12, -.36, .24),
        prime_grip=Grip(.65, thumb=.4))
    for empty in (False, True):
        clips.append(energy_reload(family, prefix + "Reload" + ("Empty" if empty else ""),
                                   reload_length, action, empty))
    return family, clips


def laser_handgun_family(key, model, prefix, walk_name, reload_length, reference, two_handed_front=False):
    gun = GUNS[model]
    stance = Stance(hip_offset=(.065, -.075, .025), hip_turn=(1., 3., 6.),
                    torso=(-16., 0., 0.), aim_torso=(-14., 1., 0.),
                    collar_left=(2., 17.), collar_right=(0., 4.),
                    elbows=(20., 30.), aim_elbows=(15., 35.),
                    left_grip=Grip(.70 if two_handed_front else .80, thumb=.4))
    family = Family(key, gun, stance)
    clips = common_clips(family, prefix, LENGTHS, walk_name, shoot=(.008, 1.8, .5), aim_shoot=(.005, .9, .25))
    pivot = part_pivot(model, "cell")
    action = EnergyAction(
        roll=-35. if two_handed_front else -16., yaw=12. if two_handed_front else 6.,
        withdraw=(.025, -.025 if two_handed_front else 0., -.07),
        grab=palm(v_add(pivot, (.002, -.008, -.025)), (-.9, .2, .35), (0., .35, 1.)),
        prime=palm(v_add(pivot, (-.02 if two_handed_front else .01, -.025, .07)),
                   (-.95, .15, .1), (0., 1., .0)),
        release=.22 if two_handed_front else .18, seat=.66 if two_handed_front else .63,
        twist=18. if two_handed_front else -10.)
    for empty in (False, True):
        clips.append(energy_reload(family, prefix + "Reload" + ("Empty" if empty else ""),
                                   reload_length, action, empty))
    return family, clips


def laser_assault():
    return laser_rifle_family("LaserGun", "space_ak", "LaserGun", "LaserGunRun", LENGTHS["LaserGunReload"], 3.0)


def laser_rifle():
    return laser_rifle_family("LaserRifle", "space_rifle", "LaserRifle", "LaserRifleRun",
                              LENGTHS["LaserRifleReload"], 3.0)


def laser_pistol():
    gun = replace(GUNS["space_pistol"], relief=0.16,
                  support=palm((-0.045, 0.095, -0.03), (-1.0, 0.2, 0.18), (0.0, 1.0, 0.0)))
    family = Family("LaserPistol", gun, Stance(
        hip_offset=(.065, -.075, .025), hip_turn=(1., 3., 6.),
        torso=(-16., 0., 0.), aim_torso=(-14., 1., 0.),
        collar_left=(2., 17.), collar_right=(0., 4.),
        elbows=(20., 30.), aim_elbows=(15., 35.), left_grip=Grip(.80, thumb=.4)))
    clips = common_clips(family, "LaserPistol", LENGTHS, "LaserPistolRun",
                         shoot=(.008, 1.8, .5), aim_shoot=(.005, .9, .25))
    pivot = part_pivot("space_pistol", "cell")
    action = EnergyAction(
        roll=-12., yaw=6., withdraw=(.025, 0., -.07),
        # The small cell's centre is measured from its entire connected mesh;
        # the old grip was below it and treated it like a long magazine.
        grab=palm((-.0089912, .0965865, -.0598834), (-1., 0., .2), (0., 1., 0.)),
        prime=palm((-.009, .087, -.045), (-.95, .15, .1), (0., 1., 0.)),
        release=.18, seat=.63, twist=-10., depth=.39, pickup=(-.08, -.36, .24))
    for empty in (False, True):
        clips.append(energy_reload(family, "LaserPistolReload" + ("Empty" if empty else ""),
                                   LENGTHS["LaserPistolReload"], action, empty))
    return family, clips


def laser_smg():
    return laser_handgun_family("LaserSMG", "space_smg", "LaserSMG", "LaserSMGRun", LENGTHS["LaserSMGReload"], 1.6,
                                two_handed_front=True)


LASER_SHOTGUN_AXIS = (1.0, 0.0, 0.0)


def laser_shotgun():
    gun = GUNS["space_shotgun"]
    stance = Stance(hip_offset=(0.11, -0.075, -0.02), hip_turn=(1.0, 4.0, 11.0), torso=(-30.0, 2.0, 0.0),
                    aim_torso=(-26.0, 2.0, 0.0), collar_left=(3.0, 22.0))
    family = Family("LaserShotgun", gun, stance)
    clips = []
    # Its hold borrows the laser rifle's clips: these variants fit its forend.
    for name in ("LaserGunIdle", "LaserGunRun", "LaserGunShoulder", "LaserGunShoulderIdle", "LaserGunShoulderWalk"):
        shared = [c for c in common_clips(family, "LaserGun", LENGTHS, "LaserGunRun") if c.name == name][0]
        shared.name = name + "@space_shotgun"
        clips.append(shared)
    for name, aimed_base in (("LaserShotgunShoot", False), ("LaserShotgunShoulderShoot", True)):
        fire = family.clip(name, LENGTHS[name], aimed_base=aimed_base)
        fire.gun = Track((0.0,) * 6)
        fire.gun.key(0.033, (0.0, 0.003, 0.035, 0.0, 5.0 if not aimed_base else 3.0, 2.0), "snap")
        fire.gun.key(0.20, (0.0, -0.004, 0.004, 0.0, -1.0, 0.5), "out")
        fire.gun.key(0.36, (0.0, -0.008, 0.008, 1.0, -1.0, 2.5), "inout")
        fire.gun.key(0.48, (0.0, -0.002, -0.004, 0.0, 1.0, 0.0), "snap")
        fire.gun.key(fire.length, (0.0,) * 6, "settle")
        # The energy shotgun cycles a cooling sleeve automatically; the
        # support hand stays planted instead of miming a conventional pump.
        sleeve = fire.part("pump")
        sleeve.key(.12, Frame(), "linear").key(.24, Frame((.018, 0., 0.)), "out")
        sleeve.hold(.42).key(.68, Frame(), "smooth")
        fire.cue(.20, sound="sounds/laser_vent", volume=.45, pitch_jitter=.03)
        clips.append(fire)
    shell = part_pivot("space_shotgun", "shell")
    grab = palm(v_add(shell, (0.0, -0.03, -0.03)), (-0.95, 0.0, 0.3), (0.0, 0.55, 0.85))
    clips.append(energy_shell_reload(family, shell, grab))
    return family, clips


def energy_shell_reload(family: Family, pivot: tuple, grab: Frame) -> Clip:
    """Vent the chamber and dock one capacitor cartridge per retail round."""
    clip = family.clip("LaserShotgunReload", LENGTHS["LaserShotgunReload"])
    q = lambda fraction: fraction * clip.length
    swap = MagSwap(family, "shell", pivot, grab)
    loading = shown_at(family, pivot, (.50, .70), .28, (9., -3., -42.))
    clip.gun = Track(loading)
    source = Frame(in_view(-.13, -.31, .18), clip.gun_frame(0).compose(grab).q)
    clip.left = Track(HandAt(source, "view")).hold(q(.10))
    clip.left.key(q(.38), swap.palm(offset=(.025, -.025, -.055), roll=-25.), "smooth")
    clip.left.key(q(.55), swap.palm(roll=-25.), "out")
    clip.left.key(q(.65), swap.palm(), "inout")
    clip.left.key(q(.78), swap.palm(offset=(0., -.035, -.025)), "out")
    clip.left.key(clip.length, HandAt(source, "view"), "smooth")
    clip.left_grip = Track(Grip(.6, trigger=.4, thumb=.5)).hold(q(.63))
    clip.left_grip.key(q(.74), RELAXED).key(clip.length, Grip(.6, trigger=.4, thumb=.5))
    sleeve = clip.part("pump")
    sleeve.key(q(.18), Frame((.02, 0., 0.)), "smooth").hold(q(.65))
    sleeve.key(q(.84), Frame(), "smooth")
    clip.part_follow["shell"] = (grab_after(swap), 0., q(.65), pivot)
    clip.cue(0., show="shell", bone="PropLeft")
    clip.cue(q(.12), sound="sounds/laser_vent", volume=.35, pitch_jitter=.03)
    clip.cue(q(.54), sound="sounds/laser_shell_in", volume=.75, pitch_jitter=.03)
    clip.cue(q(.65), detach="shell", blend=.025)
    clip.cue(q(.68), hide="shell")
    clip.cue(q(.82), sound="sounds/cell_seat", volume=.4, pitch_jitter=.025)
    return clip


def close_weapon(model: str, position: tuple, turn: tuple) -> Family:
    """A neutral upper body for the one-arm idle masks of tools/grenades."""
    gun = Gun(model, (0., .10, 0.), (0., .10, .25), (0., 1., 0.),
              pivot=(0., .10, .015), butt=(0., .10, .015),
              support=palm((.04, .10, .04), (-.7, .2, .2), (0., 1., 0.)))
    family = Family("Grenade" if model == "grenade" else "Pick", gun,
                    Stance(torso=(0., 0., 0.), aim_torso=(0., 0., 0.),
                           collar_left=(0., 0.), collar_right=(0., 0.),
                           elbows=(10., 10.), left_grip=RELAXED,
                           right_grip=Grip(.88, trigger=.82, thumb=.75),
                           prop_right=q_axis_angle(Y, -90.)))
    rotation = view_turn(*turn)
    base = Frame(v_sub(position, q_rotate(rotation, gun.pivot)), rotation)
    # Close weapons have no sights; retain the normal Clip/family machinery.
    family.hip = lambda: base
    return family


def knife() -> tuple[Family, list[Clip]]:
    """Forward knife guard, short anticipation, point-first thrust, withdrawal.

    Model-qualified clips leave pickaxes and other tools' swings alone.
    Their right-arm mask also leaves the off hand's locomotion untouched.
    """
    family = close_weapon("knife", in_view(.20, -.15, .32), (240., -5., -60.))
    # The installed knife's handle runs across the palm toward the thumb,
    # just as the firearm handles do. The grenade's quarter-turn puts this
    # hilt along the fingers and sends the blade through their knuckles.
    family.stance.prop_right = IDENTITY
    family.stance.right_grip = Grip(.94, trigger=.94, thumb=.10)
    clips = []
    for name, length in (("PickIdle@knife", .317), ("PickWalk@knife", .983)):
        clip = family.clip(name, length)
        clip.looping = True
        if "Walk" in name:
            loop_keys(clip.gun, length, lambda t: (wave(t, length, .0025), wave(t, length, .004),
                                                  0., wave(t, length, .4), wave(t, length, .5), 0.))
        clips.append(clip)
    hit = family.clip("PickUse@knife", .317)
    # The first movement is into the strike: gameplay resolves a melee use
    # immediately, so the wind-up is intentionally only one animation sample.
    hit.gun.key(.033, (.006, -.004, .008, 0., 0., 0.), "out")
    # Move the fist along the blade, keeping the point in front throughout.
    # This axis is the installed blade's guard-to-tip direction in the ready
    # pose; a broad swing would expose its thin edge and twist the wrist.
    hit.gun.key(.100, (-.100, .069, -.104, 0., 0., 0.), "in")
    hit.gun.hold(.133)
    hit.gun.key(.233, (-.028, .018, -.030, 0., 0., 0.), "out")
    hit.gun.key(hit.length, (0.,) * 6, "smooth")
    # Keep the complete hilt grip through contact and withdrawal. Reopening
    # the thumb during a 317 ms stab looked like a loose pinch on the blade.
    hit.cue(.033, sound="sounds/knife_swish", volume=.65, pitch_jitter=.05)
    hit.cue(.23, sound="sounds/cloth_lower", volume=.22, pitch_jitter=.04)
    clips.append(hit)
    return family, clips


def grenade() -> tuple[Family, list[Clip]]:
    """Two-hand pin preparation, a held ready pose, and an overhand release."""
    family = close_weapon("grenade", in_view(.17, -.19, .30), (205., -12., 18.))
    family.stance.right_grip = Grip(.50, trigger=.45, thumb=.45)
    support = Frame(in_view(-.17, -.26, .18), hand_frame((.1, .2, -.9), (.6, .5, .0)))
    clips = []
    for name, length in (("GrenadeIdle", .317), ("GrenadeWalk", .983)):
        clip = family.clip(name, length)
        clip.left = Track(HandAt(support, "view"))
        clip.looping = True
        if "Walk" in name:
            loop_keys(clip.gun, length, lambda t: (wave(t, length, .003), wave(t, length, .004),
                                                   0., 0., wave(t, length, .6), wave(t, length, .7)))
        clips.append(clip)
    cook = family.clip("Grenade_Cook", .983)
    cook.left = Track(HandAt(support, "view"))
    pin = palm((.020, .100, .045), (-.7, .1, .55), (0., 1., 0.))
    ready = (.015, .035, .015, -8., 8., -12.)
    cook.gun.key(.20, (-.04, .025, .015, 8., 6., -8.), "smooth")
    cook.gun.hold(.51).key(.85, ready, "smooth").hold(cook.length)
    cook.left.key(.25, HandAt(pin, arc=(0., -.025, 0.)), "smooth")
    cook.left.hold(.36).key(.57, HandAt(Frame(v_add(pin.p, (.085, -.025, .015)), pin.q)), "out")
    cook.left.key(.85, HandAt(support, "view", arc=(-.025, -.025, 0.)), "smooth")
    cook.left_grip = Track(RELAXED).key(.29, Grip(.5, trigger=.55, thumb=.6), "out")
    cook.left_grip.hold(.57).key(.84, RELAXED, "smooth")
    cook.cue(.34, sound="sounds/grenade_pin", volume=.65, pitch_jitter=.025)
    cook.cue(.55, sound="sounds/cloth_raise", volume=.3, pitch_jitter=.04)
    clips.append(cook)
    throw = family.clip("Grenade_Throw", .317)
    throw.gun = Track(ready).key(.10, (.07, .08, .025, -16., 22., -10.), "inout")
    throw.gun.key(.267, (-.065, .06, -.055, 20., -24., 8.), "in")
    throw.gun.key(throw.length, (-.070, .05, -.058, 24., -32., 10.), "out")
    throw.left = Track(HandAt(support, "view"))
    throw.left_grip = Track(RELAXED)
    throw.right_grip.hold(.25).key(throw.length, Grip(.08, thumb=.1, spread=.35), "out")
    throw.cue(.17, sound="sounds/grenade_throw", volume=.5, pitch_jitter=.04)
    clips.append(throw)
    reset = family.clip("Grenade_Reset", .317)
    reset.gun = Track(throw.gun.value(throw.length)).key(.317, (0.,) * 6, "smooth")
    reset.left = Track(HandAt(support, "view"))
    reset.right_grip = Track(Grip(.08, thumb=.1, spread=.35)).key(.317, family.stance.right_grip, "smooth")
    clips.append(reset)
    return family, clips


def grab_after(swap: MagSwap):
    """The part's rest frame seen from the left wrist when the palm closes on it."""
    wrist = swap.grab.compose(Frame(v_scale(PALM["Left"], -1.0), IDENTITY))
    return wrist.inverse().compose(Frame(swap.pivot, IDENTITY))


# The length each clip is authored at: the game's own length (check-anim
# --clips), the shorter one where the two generations differ (a shorter clip
# holds its last pose, which every clip here ends on). Reload clips are
# stretched over the real reload time by the engine whatever their length.
LENGTHS = {
    "GunIdle": 0.05, "GunRun": 0.983, "GunShoot": 0.117, "GunReload": 2.983, "GunShoulder": 0.117,
    "GunShoulderIdle": 0.117, "GunShoulderShoot": 0.083, "GunShoulderWalk": 0.983,
    "PistolIdle": 0.983, "PistolWalk": 0.983, "PistolShoot": 0.15, "PistolReload": 1.583, "PistolShoulder": 0.15,
    "PistolShoulderIdle": 0.15, "PistolShoulderShoot": 0.15, "PistolShoulderWalk": 0.983,
    "SMGIdle": 0.117, "SMGWalk": 0.983, "SMGShoot": 0.117, "SMGReload": 1.983, "SMGShoulder": 0.117,
    "SMGShoulderIdle": 0.117, "SMGShoulderShoot": 0.117, "SMGShoulderWalk": 0.983,
    "RifleIdle": 0.117, "RifleWalk": 0.983, "RifleShoot": 0.117, "RifleReload": 2.383, "RifleShoulder": 0.117,
    "RifleShoulderIdle": 0.117, "RifleShoulderShoot": 0.117, "RifleShoulderWalk": 0.983,
    "PumpShotgunIdle": 0.117, "PumpShotgunRun": 0.983, "PumpShotgunShoot": 0.983, "PumpShotgunReload": 0.583,
    "PumpShotgunShoulder": 0.233, "PumpShotgunShoulderIdle": 0.117, "PumpShotgunShoulderShoot": 0.983,
    "PumpShotgunShoulderWalk": 1.483,
    "LMGIdle": 0.117, "LMGWalk": 0.983, "LMGShoot": 0.117, "LMGReload": 9.683, "LMGShoulder": 0.217,
    "LMGShoulderIdle": 0.117, "LMGShoulderShoot": 0.117, "LMGShoulderWalk": 1.483,
    "RPGIdle": 0.083, "RPGWalk": 0.983, "RPGShoot": 0.983,
    "LaserGunIdle": 0.05, "LaserGunRun": 0.983, "LaserGunShoot": 0.15, "LaserGunReload": 3.0,
    "LaserGunShoulder": 0.15, "LaserGunShoulderIdle": 0.117, "LaserGunShoulderShoot": 0.1,
    "LaserGunShoulderWalk": 0.983,
    "LaserRifleIdle": 0.05, "LaserRifleRun": 0.983, "LaserRifleShoot": 0.15, "LaserRifleReload": 3.0,
    "LaserRifleShoulder": 0.15, "LaserRifleShoulderIdle": 0.117, "LaserRifleShoulderShoot": 0.1,
    "LaserRifleShoulderWalk": 0.983,
    "LaserPistolIdle": 3.333, "LaserPistolRun": 0.983, "LaserPistolShoot": 0.15, "LaserPistolReload": 1.6,
    "LaserPistolShoulder": 0.15, "LaserPistolShoulderIdle": 0.15, "LaserPistolShoulderShoot": 0.15,
    "LaserPistolShoulderWalk": 0.983,
    "LaserSMGIdle": 3.333, "LaserSMGRun": 0.983, "LaserSMGShoot": 0.15, "LaserSMGReload": 1.6,
    "LaserSMGShoulder": 0.15, "LaserSMGShoulderIdle": 0.15, "LaserSMGShoulderShoot": 0.15,
    "LaserSMGShoulderWalk": 0.983,
    "LaserShotgunShoot": 0.983, "LaserShotgunShoulderShoot": 0.983, "LaserShotgunReload": 0.983,
}


# ---------------------------------------------------------------------------
# Parts cut out of the guns (gunfx.parts), in each model's own space
# ---------------------------------------------------------------------------
# Boxes select triangles by their centre; `islands` takes whole connected
# pieces of the mesh whose middle lies in a box (the pistol's and SMG's
# magazines run up inside the grip, so they come out whole). `pivot` is the
# point the clips turn a part about. Fresh magazines are copies, hidden until
# the hand brings them.

def around(centre, reach=0.01):
    return (v_sub(centre, (reach, reach, reach)), v_add(centre, (reach, reach, reach)))


PARTS = {
    "modern/ak": [
        # The magazine is welded to the receiver in this mesh. Include its
        # entire rear wall and neck, but leave the handguard underside behind.
        # The resulting eight boundary edges are all at the magazine mouth.
        {"name": "mag", "box": ((-0.34, 0.07, -0.16), (-0.145, 0.18, 0.0408)),
         "exclude": [((-0.34, 0.07, 0.025), (-0.215, 0.18, 0.05))], "pivot": (-0.228, 0.135, 0.02)},
        {"name": "mag_new", "copy": "mag", "hidden": True, "pivot": (-0.228, 0.135, 0.02)},
        # The small forward hook is the charging handle; the long receiver
        # side lever is the safety selector and must remain stationary.
        {"name": "bolt", "islands": [around((-0.244915, 0.149574, 0.082922), .003)],
         "pivot": (-0.244915, 0.149574, 0.082922)},
    ],
    "classic/ak": [
        {"name": "mag", "box": ((-0.26, 0.08, -0.10), (-0.115, 0.18, 0.053)),
         "exclude": [((-0.26, 0.08, 0.04), (-0.17, 0.18, 0.06))], "pivot": (-0.205, 0.13, 0.035)},
        {"name": "mag_new", "copy": "mag", "hidden": True, "pivot": (-0.205, 0.13, 0.035)},
        # This low-detail receiver has no charging hook to detach. An original
        # small steel handle supplies the missing contact without cutting it.
        {"name": "bolt", "model": "models/ak_charging_handle", "pivot": (-.216, .136, .078)},
    ],
    "pistol": [
        # Reserve the complete stationary hammer before cutting the slide.
        # Its old box exclusion cut three hammer faces off with the slide.
        {"name": "hammer", "islands": [around((0.01380, 0.11118, 0.09245), .003)],
         "pivot": (0.014, 0.111, 0.078)},
        {"name": "slide", "box": ((-0.26, 0.08, 0.086), (0.05, 0.14, 0.13)),
         "pivot": (-0.1, 0.112, 0.1)},
        {"name": "mag", "islands": [around((-0.0015, 0.113, 0.001))], "pivot": (0.0, 0.113, -0.066)},
        {"name": "mag_new", "copy": "mag", "hidden": True, "pivot": (0.0, 0.113, -0.066)},
    ],
    "smg": [
        {"name": "mag", "islands": [around((-0.0435, 0.109, -0.036))], "pivot": (-0.0435, 0.109, -0.15)},
        {"name": "mag_new", "copy": "mag", "hidden": True, "pivot": (-0.0435, 0.109, -0.15)},
        # Round charging knob, behind the fixed U-shaped front-sight guards.
        {"name": "knob", "islands": [around((-0.13528, 0.11511, 0.11781), .004)],
         "pivot": (-0.13528, 0.11511, 0.11781)},
    ],
    "rifle": [
        {"name": "bolt", "islands": [around((-0.108, 0.099, 0.106)), around((-0.0805, 0.1215, 0.0865)),
                                       around((-0.04954, 0.10157, 0.08750), .004)],
         "pivot": (-0.108, 0.099, 0.106)},
        {"name": "clip", "model": "models/stripper_clip", "rotation": (0.0, 19.0, 0.0), "hidden": True,
         "pivot": (-0.13, 0.099, 0.13)},
    ],
    "lmg": [
        # Both complete box shells; a centroid box slices their rim and takes
        # nearby handguard/trigger triangles with it.
        {"name": "box", "islands": [around((-0.13803, 0.18817, -0.03133), .008),
                                      around((-0.15098, 0.08764, -0.03176), .008)],
         "pivot": (-0.21, 0.15, 0.035)},
        {"name": "box_new", "copy": "box", "hidden": True, "pivot": (-0.21, 0.15, 0.035)},
        # Rear aperture, adjustment hardware and sight base are ten separate
        # islands mounted on the lid. The front sight remains on the barrel.
        {"name": "cover", "islands": [around((-0.085, 0.114, 0.142), .008),
                                        ((-0.055, 0.065, 0.15), (0.015, 0.125, 0.19))],
         "pivot": (-0.221, 0.114, 0.142)},
    ],
    "modern/shotgun": [
        # Only the fore-end: the small island above it is the fixed rear sight.
        {"name": "pump", "islands": [around((-0.3785, 0.1605, -0.0095))],
         "pivot": (-0.378, 0.16, -0.01)},
        {"name": "shell", "model": "models/shell", "rotation": (0.0, -14.0, 0.0), "hidden": True,
         "pivot": (-0.15, 0.125, -0.02)},
    ],
    "classic/shotgun": [
        {"name": "pump", "islands": [around((-0.3475, 0.123, 0.065))],
         "pivot": (-0.3475, 0.123, 0.065)},
        {"name": "shell", "model": "models/shell", "hidden": True, "pivot": (-0.12, 0.125, 0.04)},
    ],
    "space_ak": [
        {"name": "cell", "islands": [around((-0.1755, 0.078, -0.0085))], "pivot": (-0.1755, 0.078, 0.03)},
        {"name": "cell_new", "copy": "cell", "hidden": True, "pivot": (-0.1755, 0.078, 0.03)},
    ],
    "space_rifle": [
        {"name": "cell", "islands": [around((-0.172, 0.073, -0.0045))], "pivot": (-0.172, 0.073, 0.025)},
        {"name": "cell_new", "copy": "cell", "hidden": True, "pivot": (-0.172, 0.073, 0.025)},
    ],
    "space_pistol": [
        {"name": "cell", "islands": [around((-0.0085, 0.097, -0.06))], "pivot": (-0.0085, 0.097, -0.06)},
        {"name": "cell_new", "copy": "cell", "hidden": True, "pivot": (-0.0085, 0.097, -0.06)},
    ],
    "space_smg": [
        {"name": "cell", "islands": [around((-0.008, 0.0985, -0.061))], "pivot": (-0.008, 0.0985, -0.061)},
        {"name": "cell_new", "copy": "cell", "hidden": True, "pivot": (-0.008, 0.0985, -0.061)},
    ],
    "space_shotgun": [
        {"name": "pump", "islands": [around((-0.246, 0.082, 0.0215))], "pivot": (-0.246, 0.082, 0.0215)},
        {"name": "shell", "model": "models/energy_shell", "hidden": True, "pivot": (-0.2, 0.08, 0.0)},
    ],
    "modern/rpg": [
        {"name": "rocket", "mesh": 1, "pivot": (-0.48, 0.1015, 0.0355)},
        {"name": "rocket_new", "copy": "rocket", "hidden": True, "pivot": (-0.48, 0.1015, 0.0355)},
    ],
    "classic/rpg": [
        {"name": "rocket_new", "model": "models/rocket", "hidden": True, "pivot": (-0.48, 0.1015, 0.0355)},
    ],
}


def part_pivot_at(key, name):
    """A part's pivot from one exact PARTS entry ("classic/ak", "pistol", ...)."""
    for part in PARTS[key]:
        if part["name"] == name:
            return part["pivot"]
    raise KeyError((key, name))


def part_pivot(model, name):
    for key in (model, "modern/" + model, "classic/" + model):
        for part in PARTS.get(key, []):
            if part["name"] == name:
                return part["pivot"]
    raise KeyError((model, name))


# ---------------------------------------------------------------------------
# The client script: parts, cues and the procedural motion (Luau)
# ---------------------------------------------------------------------------

# The game's own reload sounds each clip replaces (GunInventoryItemClass reload cues).
RETAIL_RELOAD_CUES = {
    "GunReload": ["AssaultReload"], "RifleReload": ["AssaultReload"], "LaserGunReload": ["AssaultReload"],
    "LaserRifleReload": ["AssaultReload"], "PistolReload": ["Reload"], "SMGReload": ["Reload"],
    "LMGReload": ["Reload"], "LaserPistolReload": ["Reload"], "LaserSMGReload": ["Reload"],
    "PumpShotgunReload": ["ShotGunReload"], "LaserShotgunReload": ["ShotGunReload"],
}

# Procedural first-person motion per family (1 = the engine's tuned default):
# pistols light and snappy, rifles steadier, the machine gun and the launcher
# heavy and slow to swing.
MOTION = {
    "Pistol": {"sway": .50, "bob": .40, "roll": .40, "recoil": .30, "weight": .7},
    "LaserPistol": {"sway": .48, "bob": .38, "roll": .35, "recoil": .18, "weight": .8},
    "SMG": {"sway": .55, "bob": .45, "roll": .45, "recoil": .28, "weight": .9},
    "LaserSMG": {"sway": .52, "bob": .42, "roll": .40, "recoil": .20, "weight": 1.0},
    "Gun": {"sway": .60, "bob": .45, "roll": .45, "recoil": .30, "weight": 1.0},
    "LaserGun": {"sway": .55, "bob": .42, "roll": .40, "recoil": .20, "weight": 1.1},
    "Rifle": {"sway": .62, "bob": .40, "roll": .40, "recoil": .42, "weight": 1.2},
    "LaserRifle": {"sway": .60, "bob": .38, "roll": .35, "recoil": .25, "weight": 1.3},
    "PumpShotgun": {"sway": .62, "bob": .48, "roll": .45, "recoil": .45, "weight": 1.2},
    "LaserShotgun": {"sway": .58, "bob": .42, "roll": .40, "recoil": .30, "weight": 1.25},
    "LMG": {"sway": .70, "bob": .40, "roll": .35, "recoil": .25, "sprint": .6, "weight": 1.65},
    "RPG": {"sway": .65, "bob": .40, "roll": .35, "recoil": .20, "sprint": .55, "weight": 1.6},
    "Pick": {"sway": .35, "bob": .35, "roll": .3, "recoil": 0., "weight": .8},
    "Grenade": {"sway": .4, "bob": .35, "roll": .3, "recoil": 0., "weight": .75},
}


def lua_number(value):
    text = f"{value:.4f}".rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def lua_value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return lua_number(value)
    if isinstance(value, str):
        return json.dumps(value)
    if isinstance(value, tuple) and len(value) == 3 and all(isinstance(c, (int, float)) for c in value):
        return f"v({', '.join(lua_number(c) for c in value)})"
    if isinstance(value, dict):
        return "{ " + ", ".join(f"{key} = {lua_value(item)}" for key, item in value.items()) + " }"
    if isinstance(value, (list, tuple)):
        return "{ " + ", ".join(lua_value(item) for item in value) + " }"
    raise TypeError(value)


def lua_part(part):
    out = {"name": part["name"]}
    if "box" in part:
        out["box"] = {"min": part["box"][0], "max": part["box"][1]}
    if "islands" in part:
        out["boxes"] = [{"min": lo, "max": hi} for lo, hi in part["islands"]]
        out["islands"] = True
    if "exclude" in part:
        out["exclude"] = [{"min": lo, "max": hi} for lo, hi in part["exclude"]]
    for key in ("mesh", "copy", "model", "rotation", "scale", "pivot", "hidden"):
        if key in part:
            out[key] = part[key]
    return lua_value(out)


SCRIPT_HEAD = """-- Tactical Animations: the gun parts the clips move, the cues they fire and the
-- procedural weapon motion. GENERATED by tools/ModSdk/make_tactical_animations.py
-- from the same timelines as assets/animations/*.gltf: change the generator and
-- run it again rather than editing this file. Presentation only: nothing here
-- reaches the server, the game's timing or its aim.

if gunfx == nil then
\tlog.warn("Tactical Animations: this game has no gunfx library; only the arm animations play")
\treturn
end

local v = vector.create

mod.config.define("motion", {
\ttype = "boolean", default = true,
\tlabel = ui.text("setting.motion"), description = ui.text("setting.motion_hint"),
})
mod.config.define("motion_strength", {
\ttype = "number", default = 1, min = 0.25, max = 1.5, step = 0.25,
\tlabel = ui.text("setting.motion_strength"), description = ui.text("setting.motion_strength_hint"),
})

-- Parts cut out of the guns of your own installed game (model space: barrel -X, up +Z).
"""

SCRIPT_TAIL = """
local function apply_motion()
\tif not mod.config.get("motion") then
\t\tgunfx.motion(nil)
\t\treturn
\tend
\tlocal k = mod.config.get("motion_strength")
\tlocal families = {}
\tfor name, values in FAMILIES do
\t\tlocal scaled = {}
\t\tfor key, value in values do
\t\t\tscaled[key] = if key == "weight" then value else value * k
\t\tend
\t\tfamilies[name] = scaled
\tend
\tgunfx.motion({
\t\tsway = k, bob = k, roll = k, sprint = k, jump = 0.55 * k, breathe = 0.4 * k, recoil = k, switch = 0.65 * k,
\t\taim_ease = 1, aim_damping = 0.3, weight = 1,
\t\tfamilies = families,
\t})
end

apply_motion()
events.on("config.changed", function(event)
\tif event.mod == mod.id then
\t\tapply_motion()
\tend
end)
"""


def write_script(path, families):
    lines = [SCRIPT_HEAD.rstrip("\n")]
    for model, parts in PARTS.items():
        lines.append(f"gunfx.parts({json.dumps(model)}, {{")
        for part in parts:
            lines.append(f"\t{lua_part(part)},")
        lines.append("})")
    lines.append("")
    lines.append("-- Cues along each clip: times are seconds of the clip as authored (a reload is")
    lines.append("-- stretched over the gun's real reload time, its cues with it).")
    for family, clips in families:
        for clip in clips:
            if not clip.cues:
                continue
            base = clip.name.split("@")[0].removesuffix("Empty")
            options = {"remote_volume": 0.6, "remote_distance": 24}
            if base in RETAIL_RELOAD_CUES:
                options["mute"] = RETAIL_RELOAD_CUES[base]
            lines.append(f"gunfx.events({json.dumps(clip.name)}, {{")
            for cue in sorted(clip.cues, key=lambda c: c.time):
                entry = {"time": round(cue.time, 3)}
                entry.update(cue.action)
                lines.append(f"\t{lua_value(entry)},")
            lines.append(f"}}, {lua_value(options)})")
    lines.append("")
    lines.append("-- Weapon sway, bob, breathing, recoil and the eased aim, weighted per family.")
    lines.append("local FAMILIES = " + lua_value(MOTION))
    lines.append(SCRIPT_TAIL)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# The pack's own small meshes: rounds and a rocket (low poly, flat colours)
# ---------------------------------------------------------------------------
# Built along X with the business end toward -X (the guns' muzzle side), in
# metres, origin at the middle. Each piece is a prism of `sides` faces.

def prism(center, length, radius, sides=8, radius_end=None, axis_x=True, offset=(0.0, 0.0, 0.0)):
    """A cylinder (or a cone when radius_end differs) along X from
    center.x - length/2 (radius) to center.x + length/2 (radius_end)."""
    radius_end = radius if radius_end is None else radius_end
    cx, cy, cz = center
    positions, normals, indices = [], [], []
    x0, x1 = cx - length / 2.0, cx + length / 2.0
    ring = []
    for k in range(sides):
        a = 2.0 * math.pi * (k + 0.5) / sides
        ring.append((math.cos(a), math.sin(a)))
    slope = (radius - radius_end) / length if length else 0.0
    for k in range(sides):
        (ca, sa), (cb, sb) = ring[k], ring[(k + 1) % sides]
        cm, sm = math.cos(2.0 * math.pi * (k + 1) / sides), math.sin(2.0 * math.pi * (k + 1) / sides)
        normal = v_norm((slope, cm, sm))
        base = len(positions)
        for x, r, c, sn in ((x0, radius, ca, sa), (x0, radius, cb, sb), (x1, radius_end, cb, sb), (x1, radius_end, ca, sa)):
            positions.append((x, cy + r * c, cz + r * sn))
            normals.append(normal)
        indices += [base, base + 2, base + 1, base, base + 3, base + 2]
    for x, r, sign in ((x0, radius, -1.0), (x1, radius_end, 1.0)):
        if r <= 1e-6:
            continue
        centre = len(positions)
        positions.append((x, cy, cz))
        normals.append((sign, 0.0, 0.0))
        for k in range(sides):
            c, sn = ring[k]
            positions.append((x, cy + r * c, cz + r * sn))
            normals.append((sign, 0.0, 0.0))
        for k in range(sides):
            a, b = centre + 1 + k, centre + 1 + (k + 1) % sides
            indices += [centre, a, b] if sign > 0 else [centre, b, a]
    return positions, normals, indices


def slab(center, size):
    cx, cy, cz = center
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    faces = [((1, 0, 0), [(hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz), (hx, -hy, hz)]),
             ((-1, 0, 0), [(-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz), (-hx, -hy, -hz)]),
             ((0, 1, 0), [(-hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz)]),
             ((0, -1, 0), [(-hx, -hy, hz), (-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz)]),
             ((0, 0, 1), [(hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz), (-hx, -hy, hz)]),
             ((0, 0, -1), [(-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz), (hx, -hy, -hz)])]
    positions, normals, indices = [], [], []
    for normal, corners in faces:
        base = len(positions)
        for x, y, z in corners:
            positions.append((cx + x, cy + y, cz + z))
            normals.append(normal)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, indices


def write_prop(path, pieces, note):
    """pieces: [(mesh (positions, normals, indices), rgb 0-255, metallic)]."""
    buffer = bytearray()
    views, accessors, materials, primitives = [], [], [], []

    def add(data, target=None):
        while len(buffer) % 4:
            buffer.append(0)
        view = {"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)}
        if target:
            view["target"] = target
        buffer.extend(data)
        views.append(view)
        return len(views) - 1

    for (positions, normals, indices), color, metallic in pieces:
        materials.append({"pbrMetallicRoughness": {"baseColorFactor": [c / 255.0 for c in color] + [1.0],
                                                   "metallicFactor": metallic, "roughnessFactor": 0.55}})
        pv = add(b"".join(struct.pack("<3f", *(round(c, 5) for c in v)) for v in positions), 34962)
        nv = add(b"".join(struct.pack("<3f", *(round(c, 5) for c in v)) for v in normals), 34962)
        iv = add(b"".join(struct.pack("<H", i) for i in indices), 34963)
        lo = [min(v[k] for v in positions) for k in range(3)]
        hi = [max(v[k] for v in positions) for k in range(3)]
        accessors.append({"bufferView": pv, "componentType": 5126, "count": len(positions), "type": "VEC3",
                          "min": lo, "max": hi})
        accessors.append({"bufferView": nv, "componentType": 5126, "count": len(normals), "type": "VEC3"})
        accessors.append({"bufferView": iv, "componentType": 5123, "count": len(indices), "type": "SCALAR"})
        primitives.append({"attributes": {"POSITION": len(accessors) - 3, "NORMAL": len(accessors) - 2},
                           "indices": len(accessors) - 1, "material": len(materials) - 1})
    document = {
        "asset": {"version": "2.0", "generator": "OpenMinerZ tools/ModSdk/make_tactical_animations.py",
                  "copyright": "Original model, OpenMinerZ examples", "extras": {"openminerz": note}},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": path.stem, "mesh": 0}],
        "meshes": [{"name": path.stem, "primitives": primitives}], "materials": materials,
        "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(buffer),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")


BRASS_RGB = (196, 156, 72)
STEEL_RGB = (92, 96, 104)


def build_props(folder):
    models = folder / "assets" / "models"
    # Original geometry for Classic's otherwise unmodelled charging handle.
    # Its short shank starts inside the receiver; the wider end is graspable.
    write_prop(models / "ak_charging_handle.gltf", [
        (slab((.001, -.004, 0.), (.007, .016, .006)), STEEL_RGB, .7),
        (slab((-.002, .006, 0.), (.012, .008, .008)), STEEL_RGB, .7),
    ], "An original steel charging handle for the Classic assault rifle.")
    # A 12-gauge shell: red hull, brass head (toward +X: it goes in crimp first).
    write_prop(models / "shell.gltf", [
        (prism((-0.008, 0.0, 0.0), 0.056, 0.0105, 10), (178, 34, 30), 0.0),
        (prism((0.026, 0.0, 0.0), 0.012, 0.0112, 10), BRASS_RGB, 0.8),
    ], "A shotgun shell for the Tactical Animations reload.")
    # An energy shell: a glowing cyan core between two dark metal caps.
    write_prop(models / "energy_shell.gltf", [
        (prism((0.0, 0.0, 0.0), 0.036, 0.0095, 8), (70, 230, 255), 0.0),
        (prism((-0.022, 0.0, 0.0), 0.008, 0.011, 8), (60, 64, 72), 0.7),
        (prism((0.022, 0.0, 0.0), 0.008, 0.011, 8), (60, 64, 72), 0.7),
    ], "An energy shell for the laser shotgun.")
    # A stripper clip: five rifle rounds stacked, tips forward, in a steel clip.
    pieces = []
    for k in range(5):
        z = -0.0125 * k
        pieces.append((prism((0.006, 0.0, z), 0.05, 0.0058, 6), BRASS_RGB, 0.8))
        pieces.append((prism((-0.03, 0.0, z), 0.022, 0.0055, 6, radius_end=0.0055), (170, 104, 64), 0.6))
        pieces.append((prism((-0.047, 0.0, z), 0.012, 0.0001, 6, radius_end=0.0055), (170, 104, 64), 0.6))
    pieces.append((slab((0.033, 0.0, -0.025), (0.006, 0.016, 0.066)), STEEL_RGB, 0.7))
    write_prop(models / "stripper_clip.gltf", pieces, "A stripper clip of five rounds for the bolt-action rifle.")
    # A rocket for the launcher (the Classic launcher has none of its own).
    write_prop(models / "rocket.gltf", [
        (prism((-0.25, 0.0, 0.0), 0.10, 0.0001, 10, radius_end=0.034), (86, 98, 62), 0.2),
        (prism((-0.16, 0.0, 0.0), 0.08, 0.034, 10, radius_end=0.042), (86, 98, 62), 0.2),
        (prism((-0.10, 0.0, 0.0), 0.04, 0.042, 10, radius_end=0.022), (74, 84, 54), 0.2),
        (prism((0.08, 0.0, 0.0), 0.32, 0.019, 8), (58, 60, 64), 0.6),
        (slab((0.21, 0.0, 0.0), (0.07, 0.07, 0.004)), (58, 60, 64), 0.6),
        (slab((0.21, 0.0, 0.0), (0.07, 0.004, 0.07)), (58, 60, 64), 0.6),
    ], "A rocket for the rocket launcher's reload.")


# ---------------------------------------------------------------------------
# The pack
# ---------------------------------------------------------------------------

FAMILY_BUILDERS = [assault_rifle, classic_assault_rifle, pistol, submachine_gun, bolt_rifle, pump_shotgun,
                   classic_pump_shotgun, machine_gun, rocket_launcher,
                   laser_assault, laser_rifle, laser_pistol, laser_smg, laser_shotgun, knife, grenade]


def prepare_choreography(clip: Clip) -> None:
    """Keep the cell/magazine's grasp through the pouch, and restrain head bob.

    A universal downward palm at the belt used to spin the wrist nearly half
    a turn in a single 30 Hz sample. The new magazine is already oriented in
    the hand for its insertion; fetching it is a translation with a small arc.
    """
    tactical_authoring.apply_clip(clip, sys.modules[__name__])
    # Recoil retains its fast impulse. Handling tracks carry velocity through
    # intermediate keys instead of stopping at every waypoint.
    if "Reload" in clip.name or clip.family.key in ("Pick", "Grenade"):
        for track in (clip.gun, clip.left, clip.torso, clip.collar_left, clip.collar_right, clip.elbows):
            track.flow = True
    keys = []
    for index, (time, hand, easing) in enumerate(clip.left.keys):
        if hand.space == "view" and hand.frame in (POUCH, POUCH_LOW):
            contacts = [key for key in clip.left.keys[index + 1:] if key[1].space == "gun"]
            if not contacts:
                contacts = [key for key in reversed(clip.left.keys[:index]) if key[1].space == "gun"]
            if contacts:
                contact_time, contact, _ = contacts[0]
                orientation = clip.gun_frame(contact_time).compose(contact.frame).q
                hand = HandAt(Frame(hand.frame.p, orientation), "view", hand.arc)
        keys.append((time, hand, "smooth" if easing in ("anticipate", "back") else easing))
    clip.left.keys = keys
    clip.look.keys = [(time, tuple(component * .18 for component in value), easing)
                      for time, value, easing in clip.look.keys]
    for cue in clip.cues:
        if "kick" in cue.action:
            cue.action["kick"] = {key: value if key == "seconds" else value * .25
                                  for key, value in cue.action["kick"].items()}
    # Retire the oscillator whines/hisses. Cells use their physical latch,
    # extraction and seating recordings; a charge hold can be quiet.
    clip.cues = [cue for cue in clip.cues if not
                 (cue.action.get("sound", "").startswith("sounds/cell_charge") or
                  cue.action.get("sound") == "sounds/laser_vent")]


def build_animations(skeleton, folder):
    written = []
    for builder in FAMILY_BUILDERS:
        family, clips = builder()
        for clip in clips:
            prepare_choreography(clip)
        solved = [(clip, evaluate(skeleton, clip)) for clip in clips]
        parts = sorted({name for clip in clips for name in list(clip.parts) + list(clip.part_follow)})
        path = folder / "assets" / "animations" / f"{builder.__name__}.glb"
        size = write_clips(path, skeleton, solved, parts,
                           f"Tactical Animations: the {family.key} family's clips.")
        written.append((family, clips))
        print(f"wrote {path} ({len(clips)} clips, {size // 1024} KiB)")
    return written


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bones, _lengths, _generation = read_rig(sys.argv[1])
    skeleton = Skeleton(bones)
    folder = Path(sys.argv[2]) if len(sys.argv) > 2 else \
        Path(__file__).resolve().parents[2] / "examples" / "mods" / "tactical_animations"
    families = build_animations(skeleton, folder)
    write_script(folder / "client" / "tactical.luau", families)
    build_props(folder)
    import tactical_foley
    sounds = sorted({cue.action["sound"].removeprefix("sounds/") for _, clips in families for clip in clips
                     for cue in clip.cues if "sound" in cue.action})
    tactical_foley.write_sounds(folder, sounds)
    print(f"wrote {len(sounds)} sounds and {len(list((folder / 'assets' / 'models').glob('*.gltf')))} models")


if __name__ == "__main__":
    main()
