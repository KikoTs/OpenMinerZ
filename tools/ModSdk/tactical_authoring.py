"""Validated rig profiles shared by interactive preview and animation export.

Model support edits are deltas in gun coordinates: position is additive and
rotation pre-multiplies the original contact orientation without orbiting its
position. Only authored support contacts inherit them; explicit clip keys win.
"""
from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import json
import math
from pathlib import Path
from typing import Any

PROFILE_PATH = Path(__file__).with_name("tactical_authoring.json")
ACTIVE: dict[str, Any] = {}

# A reset must not depend on the last loaded/saved user document. Keep this
# calibration as immutable text and return a fresh document for every caller.
_DEFAULT_PROFILE = """{
  "schema": 1,
  "bones": {
    "CollarLeft": "CollarLeft", "ShoulderLeft": "ShoulderLeft",
    "ElbowLeft": "ElbowLeft", "WristLeft": "WristLeft", "PropLeft": "PropLeft",
    "CollarRight": "CollarRight", "ShoulderRight": "ShoulderRight",
    "ElbowRight": "ElbowRight", "WristRight": "WristRight", "PropRight": "PropRight",
    "Head": "Head", "BackUpper": "BackUpper"
  },
  "palms": {"Left": [0.096, -0.055, 0.004], "Right": [-0.062, -0.042, 0.004]},
  "finger_curl": {"Index": [55, 78, 40], "Middle": [62, 82, 44],
                  "Ring": [67, 86, 46], "Small": [71, 90, 48]},
  "models": {}, "clips": {}
}"""
_EASING = frozenset(("linear", "in", "out", "inout", "smooth", "snap", "back",
                     "anticipate", "settle", "hold"))
_GRIP_BOUNDS = {"curl": (0.0, 1.0), "thumb": (0.0, 1.0), "handle": (0.0, 1.0),
                "spread": (0.0, 1.0), "thumb_opposition": (-45.0, 45.0)}


def defaults() -> dict[str, Any]:
    """Return independent built-in calibration with no model or clip edits."""
    return json.loads(_DEFAULT_PROFILE)


def _object(value: Any, path: str, allowed: set[str] | None = None) -> dict:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{path} must be an object with string keys")
    if allowed is not None and set(value) - allowed:
        raise ValueError(f"{path}: unknown field {sorted(set(value) - allowed)[0]}")
    return value


def _name(value: Any, path: str) -> None:
    if not isinstance(value, str) or not value or value != value.strip() or len(value) > 128:
        raise ValueError(f"{path} must be a nonempty name of at most 128 characters")
    if any(ord(character) < 32 for character in value):
        raise ValueError(f"{path} cannot contain control characters")


def _number(value: Any, path: str, lower: float, upper: float) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{path} must be a number")
    if not lower <= value <= upper or not math.isfinite(value):
        raise ValueError(f"{path} must be finite and between {lower:g} and {upper:g}")


def _vector(value: Any, count: int, path: str, lower: float, upper: float) -> None:
    if not isinstance(value, list) or len(value) != count:
        raise ValueError(f"{path} needs {count} coordinates")
    for index, component in enumerate(value):
        _number(component, f"{path}[{index}]", lower, upper)


def _quaternion(value: Any, path: str) -> None:
    _vector(value, 4, path, -1.0, 1.0)
    if abs(sum(component * component for component in value) - 1.0) > .01:
        raise ValueError(f"{path} must be a normalized quaternion")


def _keys(value: Any, path: str) -> list:
    if not isinstance(value, list) or len(value) > 4096:
        raise ValueError(f"{path} must be a list of at most 4096 keys")
    previous_times: list[float] = []
    previous_sources: list[float] = []
    for index, key in enumerate(value):
        location = f"{path}[{index}]"
        _object(key, location)
        if "time" not in key:
            raise ValueError(f"{location}.time is required")
        _number(key["time"], location + ".time", 0.0, 120.0)
        if any(abs(key["time"] - previous) <= 1e-8 for previous in previous_times):
            raise ValueError(f"{path} contains duplicate key times")
        previous_times.append(key["time"])
        if "sourceTime" in key:
            _number(key["sourceTime"], location + ".sourceTime", 0.0, 120.0)
            if any(abs(key["sourceTime"] - source) <= 1e-8 for source in previous_sources):
                raise ValueError(f"{path} contains duplicate source times")
            previous_sources.append(key["sourceTime"])
        easing = key.get("easing", "smooth")
        if not isinstance(easing, str) or easing not in _EASING:
            raise ValueError(f"{location}: unknown easing")
    return value


def validate(profile: dict[str, Any], names: set[str] | None = None) -> dict[str, Any]:
    """Return an independent validated JSON document, without changing input.

    Unknown fields, booleans used as numbers, nonfinite values, excessive
    offsets, duplicate times and nonunit rotations are rejected. Clip/model
    existence and each clip's actual duration are checked by the backend or
    apply_clip, because this structural validator does not build animations.
    """
    _object(profile, "profile", {"schema", "bones", "palms", "finger_curl", "models", "clips"})
    if type(profile.get("schema")) is not int or profile["schema"] != 1:
        raise ValueError("Rig profile schema must be 1")
    mapping = _object(profile.get("bones", {}), "bones")
    for role, source in mapping.items():
        _name(role, "bone role")
        _name(source, f"bones.{role}")
    if len(set(mapping.values())) != len(mapping):
        raise ValueError("Two bone roles cannot map to the same source bone")
    for role, source in mapping.items():
        if names is not None and source not in names:
            raise ValueError(f"Missing source bone: {source}")
        if ("Left" in role and "Right" in source) or ("Right" in role and "Left" in source):
            raise ValueError(f"Crossed limb mapping: {role} -> {source}")
    if names is not None:
        reverse = {source: role for role, source in mapping.items()}
        effective = [reverse.get(name, name) for name in names]
        if len(set(effective)) != len(effective):
            raise ValueError("Mapping would overwrite an existing unmapped bone role")
    for side, palm in _object(profile.get("palms", {}), "palms", {"Left", "Right"}).items():
        _vector(palm, 3, f"palms.{side}", -.2, .2)
    for finger, angles in _object(profile.get("finger_curl", {}), "finger_curl",
                                   {"Index", "Middle", "Ring", "Small"}).items():
        _vector(angles, 3, f"finger_curl.{finger}", 0.0, 110.0)
    models = _object(profile.get("models", {}), "models")
    if len(models) > 64:
        raise ValueError("A profile may edit at most 64 models")
    for model, edits in models.items():
        _name(model, "model")
        path = f"models.{model}"
        _object(edits, path, {"support", "attachmentRotation"})
        if "attachmentRotation" in edits:
            _quaternion(edits["attachmentRotation"], path + ".attachmentRotation")
        if "support" not in edits:
            continue
        support = _object(edits["support"], path + ".support", {"position", "rotation", "grip"})
        if "position" in support:
            _vector(support["position"], 3, path + ".support.position", -.3, .3)
        if "rotation" in support:
            _quaternion(support["rotation"], path + ".support.rotation")
        for parameter, value in _object(support.get("grip", {}), path + ".support.grip",
                                         set(_GRIP_BOUNDS)).items():
            _number(value, path + ".support.grip." + parameter, *_GRIP_BOUNDS[parameter])
    clips = _object(profile.get("clips", {}), "clips")
    if len(clips) > 512:
        raise ValueError("A profile may edit at most 512 clips")
    for clip, edits in clips.items():
        _name(clip, "clip")
        path = f"clips.{clip}"
        _object(edits, path, {"left", "gun"})
        for key in _keys(edits.get("left", []), path + ".left"):
            _object(key, path + ".left key", {"time", "sourceTime", "space", "position", "rotation", "arc", "easing"})
            if key.get("space") not in ("gun", "view"):
                raise ValueError(f"{path}.left: space must be gun or view")
            _vector(key.get("position"), 3, path + ".left.position", -3.0, 3.0)
            _quaternion(key.get("rotation"), path + ".left.rotation")
            if "arc" in key:
                _vector(key["arc"], 3, path + ".left.arc", -.3, .3)
        for key in _keys(edits.get("gun", []), path + ".gun"):
            _object(key, path + ".gun key", {"time", "sourceTime", "value", "easing"})
            value = key.get("value")
            _vector(value, 6, path + ".gun.value", -360.0, 360.0)
            for component in value[:3]:
                _number(component, path + ".gun.position", -2.0, 2.0)
    document = deepcopy(profile)
    rotations = []
    for edits in document.get("models", {}).values():
        if "attachmentRotation" in edits:
            rotations.append(edits["attachmentRotation"])
        if "rotation" in edits.get("support", {}):
            rotations.append(edits["support"]["rotation"])
    rotations.extend(key["rotation"] for edits in document.get("clips", {}).values()
                     for key in edits.get("left", []))
    for rotation in rotations:
        norm = math.sqrt(sum(component * component for component in rotation))
        # Preserve existing unit keys bit-for-bit; normalize accepted JSON
        # rounding error before the solver uses conjugation as an inverse.
        if abs(norm - 1.0) > 1e-12:
            rotation[:] = [component / norm for component in rotation]
    return document


def load(path: Path | None = None) -> dict[str, Any]:
    """Read a profile without changing its file or the built-in defaults."""
    global ACTIVE
    ACTIVE = validate(json.loads((Path(path) if path is not None else PROFILE_PATH).read_text(encoding="utf-8")))
    return deepcopy(ACTIVE)


def configure(animation: Any, profile: dict[str, Any] | None = None) -> None:
    """Reset solver calibration, then apply the selected or active profile.

    Explicit profiles replace ACTIVE with an independent copy. Call again
    with the previous profile in a preview context's finally block to restore
    it. Omitting a palm/finger entry resets that entry to the solver's source
    default, so a deleted draft edit cannot leak into subsequent previews.
    """
    global ACTIVE
    selected = validate(profile) if profile is not None else validate(ACTIVE) if ACTIVE else load()
    if not hasattr(animation, "_authoring_base_calibration"):
        animation._authoring_base_calibration = (deepcopy(animation.PALM), deepcopy(animation._CURL))
    palms, curls = animation._authoring_base_calibration
    animation.PALM.clear()
    animation.PALM.update(deepcopy(palms))
    animation._CURL.clear()
    animation._CURL.update(deepcopy(curls))
    animation.PALM.update({side: tuple(palm) for side, palm in selected.get("palms", {}).items()})
    animation._CURL.update({finger: tuple(angles) for finger, angles in selected.get("finger_curl", {}).items()})
    ACTIVE = deepcopy(selected)


def _same_frame(left: Any, right: Any, animation: Any) -> bool:
    return (animation.v_len(animation.v_sub(left.p, right.p)) < 1e-5
            and abs(sum(a * b for a, b in zip(left.q, right.q))) > 1.0 - 1e-9)


def _support_match(clip: Any, time: float, hand: Any, animation: Any) -> dict | None:
    """Recognize original support frames, including view anchors/pump travel."""
    if hand.space not in ("gun", "view"):
        return None
    gun = clip.gun_frame(time)
    local = hand.frame if hand.space == "gun" else gun.inverse().compose(hand.frame)
    original = clip.family.gun.support
    if _same_frame(local, original, animation):
        return {"time": time, "space": hand.space, "part": None}
    # A moving fore-end keeps the same contact in its part space. Require an
    # exact geometric match; nearby magazine/bolt/cover contacts are not holds.
    specs = animation.PARTS.get(clip.family.gun.model, [])
    for name, track in clip.parts.items():
        spec = next((part for part in specs if part["name"] == name), None)
        if spec is None or "pivot" not in spec:
            continue
        value = track.value(time)
        motion = animation.part_motion(spec["pivot"], value.p, value.q)
        if _same_frame(local, motion.compose(original), animation):
            return {"time": time, "space": hand.space, "part": name}
    return None


def _support_state(keys: list, flags: dict[float, dict], time: float) -> bool:
    if not keys:
        return False
    for key_time, _, _ in keys:
        if abs(time - key_time) <= 1e-8:
            return key_time in flags
    if time < keys[0][0]:
        return keys[0][0] in flags
    if time > keys[-1][0]:
        return keys[-1][0] in flags
    after = next(index for index, key in enumerate(keys) if key[0] > time)
    return keys[after - 1][0] in flags and keys[after][0] in flags


def linked_support_keys(clip: Any) -> list[dict[str, Any]]:
    """Return semantically linked key metadata after apply_clip/prepare.

    Each item contains time, space, part (None for fixed support), and explicit
    (True if an explicit clip key superseded this linked default).
    """
    return deepcopy(getattr(clip, "_authoring_support_keys", []))


def support_is_active(clip: Any, time: float) -> bool:
    """Whether this time is on a support contact, excluding explicit overrides."""
    metadata = getattr(clip, "_authoring_support_keys", [])
    flags = {key["time"]: key for key in metadata if not key["explicit"]}
    return _support_state(clip.left.keys, flags, time)


def _remove_retimed_keys(track: Any, keys: list[dict], clip: Any) -> None:
    """Remove all original locations first, allowing two keys to swap times."""
    sources = []
    for key in keys:
        _number(key["time"], f"{clip.name}: key time", 0.0, clip.length)
        if "sourceTime" not in key:
            continue
        source = key["sourceTime"]
        _number(source, f"{clip.name}: sourceTime", 0.0, clip.length)
        if any(abs(source - previous) <= 1e-8 for previous in sources):
            raise ValueError(f"{clip.name}: duplicate source times")
        sources.append(source)
    moved = [key["sourceTime"] for key in keys
             if "sourceTime" in key and abs(key["sourceTime"] - key["time"]) > 1e-8]
    track.keys = [key for key in track.keys if not any(abs(key[0] - source) <= 1e-8 for source in moved)]


def apply_clip(clip: Any, animation: Any) -> None:
    """Apply linked model defaults followed by explicit per-clip overrides.

    Authored keys are captured once, making repeated draft application and
    reset idempotent. The gun model, mechanical contacts, cues, part tracks and
    gameplay duration are untouched. Attachment rotation is a model-local
    delta on the original PropRight orientation, independent of weapon motion.
    An explicit key's optional sourceTime removes its original location before
    inserting the replacement; dropping that override restores the original.
    """
    if not hasattr(clip, "_authoring_tracks"):
        clip._authoring_tracks = {name: deepcopy(getattr(clip, name)) for name in ("left", "gun", "left_grip")}
    else:
        for name, track in clip._authoring_tracks.items():
            setattr(clip, name, deepcopy(track))
    family = clip.family
    if not hasattr(family, "_authoring_prop_right"):
        family._authoring_prop_right = family.stance.prop_right
    edits = ACTIVE.get("clips", {}).get(clip.name, {})
    model = ACTIVE.get("models", {}).get(family.gun.model, {})
    attachment = model.get("attachmentRotation", animation.IDENTITY)
    family.stance = replace(family.stance,
        prop_right=animation.q_mul(tuple(attachment), family._authoring_prop_right))
    matches = {time: match for time, hand, _ in clip.left.keys
               if (match := _support_match(clip, time, hand, animation)) is not None}
    original_keys = deepcopy(clip.left.keys)
    _remove_retimed_keys(clip.gun, edits.get("gun", []), clip)
    for key in edits.get("gun", []):
        if key["time"] > clip.length or len(key["value"]) != 6:
            raise ValueError(f"{clip.name}: invalid weapon key")
        clip.gun.key(key["time"], tuple(key["value"]), key.get("easing", "smooth"))
    support = model.get("support", {})
    if support:
        base = family.gun.support
        adjusted = animation.Frame(animation.v_add(base.p, tuple(support.get("position", (0, 0, 0)))),
            animation.q_mul(tuple(support.get("rotation", animation.IDENTITY)), base.q))
        for time, hand, easing in original_keys:
            match = matches.get(time)
            if match is None:
                continue
            frame = adjusted
            if match["part"] is not None:
                name = match["part"]
                spec = next(part for part in animation.PARTS[family.gun.model] if part["name"] == name)
                value = clip.parts[name].value(time)
                frame = animation.part_motion(spec["pivot"], value.p, value.q).compose(frame)
            if hand.space == "view":
                frame = clip.gun_frame(time).compose(frame)
            clip.left.key(time, animation.HandAt(frame, hand.space, hand.arc), easing)
        if support.get("grip"):
            clip.left_grip.keys = [(time, replace(grip, **support["grip"])
                                   if _support_state(original_keys, matches, time) else grip, easing)
                                  for time, grip, easing in clip.left_grip.keys]
    _remove_retimed_keys(clip.left, edits.get("left", []), clip)
    for key in edits.get("left", []):
        if key["time"] > clip.length:
            raise ValueError(f"{clip.name}: contact is past the clip end")
        clip.left.key(key["time"], animation.HandAt(
            animation.Frame(tuple(key["position"]), tuple(key["rotation"])),
            key["space"], tuple(key.get("arc", [0, 0, 0]))), key.get("easing", "smooth"))
    overrides = edits.get("left", [])
    metadata = []
    for time, match in matches.items():
        retimed = next((key for key in overrides if "sourceTime" in key
                        and abs(key["sourceTime"] - time) <= 1e-8
                        and abs(key["time"] - time) > 1e-8), None)
        if retimed is not None:
            metadata.append({**match, "time": retimed["time"], "sourceTime": time,
                             "space": retimed["space"], "explicit": True})
        elif any(abs(time - key[0]) <= 1e-8 for key in clip.left.keys):
            metadata.append({**match, "explicit": any(abs(time - key["time"]) <= 1e-8 for key in overrides)})
    by_time = {}
    for key in metadata:
        if "sourceTime" in key or key["time"] not in by_time:
            by_time[key["time"]] = key
    clip._authoring_support_keys = list(by_time.values())


def save(profile: dict[str, Any], names: set[str], path: Path | None = None) -> None:
    """Validate and atomically replace the requested profile; defaults stay fixed."""
    document = validate(profile, names)
    destination = Path(path) if path is not None else PROFILE_PATH
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.write_text(json.dumps(document, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(destination)
    load(destination)
