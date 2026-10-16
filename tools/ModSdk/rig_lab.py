"""Local rig/contact editor. Run with --prepare once, then open the printed URL.

Owned model/skin exports stay under out/rig-lab. The server binds only loopback,
serves explicit roots and accepts same-origin JSON writes to the authoring profile.
"""
from __future__ import annotations

import argparse
import base64
import copy
from contextlib import contextmanager
from dataclasses import asdict
from datetime import datetime, timezone
import functools
import hashlib
import json
import math
import mimetypes
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlsplit
from urllib.request import urlopen
from urllib.error import URLError

import numpy as np
from PIL import Image
import make_tactical_animations as animation
import tactical_authoring as authoring

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "out/rig-lab"
UI = Path(__file__).with_suffix("")
PACK = ROOT / "examples/mods/tactical_animations"
SDK = ROOT / "out/build/native-release/Release/openminerz_mod_sdk.exe"
CONTENT = ROOT / "out/build/native-release/content-packs"
PLAYABLE = ROOT / "out/build/native-release/Release/examples/mods/tactical_animations"
HISTORY = WORK / "history"
EXPORT_STATUS = WORK / "export-status.json"
LOCK = threading.RLock()
SERVICE = "openminerz-rig-lab"
SOURCE_REVISION = hashlib.sha256(Path(__file__).read_bytes() + Path(authoring.__file__).read_bytes()).hexdigest()
HISTORY_ID = re.compile(r"(?:\d{8}T\d{12}Z-[0-9a-f]{12}|before-editor-upgrade-\d{8}-\d{6})\Z")
ITEMS = {"modern/ak": 50, "classic/ak": 50, "pistol": 51, "smg": 54, "rifle": 53,
         "modern/shotgun": 52, "classic/shotgun": 52, "lmg": 119, "rpg": 103,
         "space_ak": 82, "space_pistol": 83, "space_shotgun": 84,
         "space_rifle": 85, "space_smg": 86, "knife": 49, "grenade": 110}


def prepare(avatar: Path | None) -> None:
    """Use the native decoders to export this installation's skin and rig."""
    WORK.mkdir(parents=True, exist_ok=True)
    command = [str(SDK), "export-rig-lab", "--rig", str(CONTENT / "modern/DerivedData/character.omrc"),
               "--out", str(WORK / "scene")]
    if avatar:
        command += ["--avatar", str(avatar)]
    subprocess.run(command, check=True)
    subprocess.run([str(SDK), "export-rig", "--rig", str(CONTENT / "modern/DerivedData/character.omrc"),
                    "--out", str(WORK / "rig.glb")], check=True)


def inside(point: np.ndarray, box: list) -> bool:
    return bool(np.all(point >= box[0]) and np.all(point <= box[1]))


def connected_centres(vertices: np.ndarray, triangles: np.ndarray) -> list[np.ndarray]:
    """Match the native 0.1 mm weld, including llround ties away from zero."""
    parent = list(range(len(vertices)))
    def root(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    positions: dict[tuple, int] = {}
    scaled = np.asarray(vertices, dtype=np.float32) * np.float32(1e4)
    quantized = np.copysign(np.floor(np.abs(scaled.astype(np.float64)) + .5), scaled).astype(np.int64)
    for i, position in enumerate(quantized):
        key = tuple(position)
        if key in positions:
            parent[root(i)] = root(positions[key])
        else:
            positions[key] = i
    for a, b, c in triangles:
        parent[root(int(b))] = root(int(a))
        parent[root(int(c))] = root(int(a))
    bounds: dict[int, tuple[np.ndarray, np.ndarray]] = {}
    # Unreferenced vertices do not contribute to native island bounds.
    for tri in triangles:
        key = root(int(tri[0]))
        for index in tri:
            position = vertices[index]
            if key not in bounds: bounds[key] = (position.copy(), position.copy())
            else: bounds[key] = (np.minimum(bounds[key][0], position), np.maximum(bounds[key][1], position))
    return [(bounds[root(int(tri[0]))][0] + bounds[root(int(tri[0]))][1]) * .5 for tri in triangles]


def selected_triangles(spec: dict, mesh: int, remaining: set[int],
                       centres: np.ndarray, islands: list[np.ndarray]) -> list[int]:
    """Use native mesh restriction, selection-point and exclusion semantics."""
    if "mesh" in spec and spec["mesh"] != mesh:
        return []
    boxes = spec.get("islands", [spec["box"]] if "box" in spec else [])
    points = islands if "islands" in spec else centres
    # A selector without boxes takes the whole allowed mesh. Native excludes
    # are applied only while checking a box, to that same selection point.
    return [index for index in sorted(remaining)
            if not boxes or (any(inside(points[index], box) for box in boxes)
                             and not any(inside(points[index], box) for box in spec.get("exclude", [])))]


@functools.lru_cache(maxsize=24)
def weapon(model: str) -> dict[str, Any]:
    """Decode our imported OMSM format and display the pack's authored part cuts."""
    if model not in ITEMS: raise ValueError("Unknown weapon model")
    generation, name = model.split("/", 1) if "/" in model else ("modern", model)
    data = (CONTENT / generation / "DerivedData" / f"{name}.omsm").read_bytes()
    offset = 0
    def read(fmt: str) -> tuple:
        nonlocal offset
        result = struct.unpack_from("<" + fmt, data, offset)
        offset += struct.calcsize("<" + fmt)
        return result
    magic, version, count = read("Iii")
    if magic != 0x4D534D4F or not 1 <= version <= 3:
        raise ValueError("Unsupported imported weapon format")
    specs = animation.PARTS.get(model, animation.PARTS.get(generation + "/" + name, []))
    surfaces = []
    for mesh in range(count):
        matrix = np.array(read("16f"), dtype=np.float32).reshape(4, 4)
        if version >= 3: read("B")
        diffuse = read("3f")
        width, height = read("ii")
        rgba = data[offset:offset + width * height * 4]
        offset += len(rgba)
        texture_path = WORK / "scene" / f"weapon-{generation}-{name}-{mesh}.png"
        if width and height:
            Image.frombytes("RGBA", (width, height), rgba).convert("RGB").save(texture_path)
        n, = read("i")
        positions, normals, uv = [], [], []
        for _ in range(n):
            p, normal, tex = read("3f"), read("3f"), read("2f")
            if version >= 2: read("4B")
            positions.append((np.array([*p, 1], dtype=np.float32) @ matrix)[:3].tolist())
            normals.append((np.array([*normal, 0], dtype=np.float32) @ matrix)[:3].tolist())
            uv.append(tex)
        indices = np.array(read(f"{read('i')[0]}I")).reshape(-1, 3)
        vertices = np.array(positions, dtype=np.float32)
        centres = vertices[indices].mean(axis=1)
        islands = connected_centres(vertices, indices)
        remaining = set(range(len(indices)))
        selections: dict[str, list[int]] = {}
        for spec in specs:
            if "copy" in spec:
                selected = selections.get(spec["copy"], [])
            elif "model" in spec:
                continue
            else:
                selected = selected_triangles(spec, mesh, remaining, centres, islands)
                remaining.difference_update(selected)
            selections[spec["name"]] = selected
        for part, selection in [("body", sorted(remaining)), *selections.items()]:
            if not selection: continue
            surfaces.append({"part": part, "position": positions, "normal": normals, "uv": uv,
                             "indices": indices[selection].reshape(-1).tolist(), "color": diffuse,
                             "texture": "/scene/" + texture_path.name if width else None})
    # Original pack props use embedded glTF buffers; read without any external files.
    for spec in specs:
        if "model" not in spec: continue
        document = json.loads((PACK / "assets" / (spec["model"] + ".gltf")).read_text())
        blob = base64.b64decode(document["buffers"][0]["uri"].split(",")[1])
        def accessor(index: int) -> list:
            acc = document["accessors"][index]
            view = document["bufferViews"][acc["bufferView"]]
            kinds = {5126: "f", 5125: "I", 5123: "H"}
            width = {"SCALAR": 1, "VEC3": 3, "VEC2": 2}[acc["type"]]
            vals = struct.unpack_from("<" + str(acc["count"] * width) + kinds[acc["componentType"]],
                                      blob, view.get("byteOffset", 0) + acc.get("byteOffset", 0))
            return list(vals) if width == 1 else [vals[i:i+width] for i in range(0, len(vals), width)]
        for mesh in document["meshes"]:
            for primitive in mesh["primitives"]:
                attrs = primitive["attributes"]
                material = document["materials"][primitive.get("material", 0)]
                surfaces.append({"part": spec["name"], "position": accessor(attrs["POSITION"]),
                                 "normal": accessor(attrs["NORMAL"]), "indices": accessor(primitive["indices"]),
                                 "color": material["pbrMetallicRoughness"]["baseColorFactor"][:3],
                                 "own": True, "texture": None})
    return {"surfaces": surfaces, "parts": specs}


def serialized(function):
    @functools.wraps(function)
    def locked(*args, **kwargs):
        with LOCK:
            return function(*args, **kwargs)
    return locked


class ProfileConflict(ValueError):
    """A save was based on an older on-disk profile."""
    def __init__(self, current: dict, revision: str):
        super().__init__("The saved profile changed. Reload or compare it before saving your draft.")
        self.current, self.revision = current, revision


def profile_state() -> tuple[dict, str]:
    data = authoring.PROFILE_PATH.read_bytes()
    return json.loads(data), hashlib.sha256(data).hexdigest()


@functools.lru_cache(maxsize=2)
def _rig_bones(modified: int, size: int) -> dict:
    return animation.read_rig(WORK / "rig.glb")[0]


def rig_bones() -> dict:
    stat = (WORK / "rig.glb").stat()
    return _rig_bones(stat.st_mtime_ns, stat.st_size)


@contextmanager
def draft_profile(profile: dict):
    """Transient solver configuration; never writes the profile or any assets."""
    previous = copy.deepcopy(authoring.ACTIVE or profile_state()[0])
    normalized = authoring.validate(profile, set(rig_bones()))
    if normalized is None:  # Older authoring modules returned only validation.
        normalized = copy.deepcopy(profile)
    try:
        authoring.configure(animation, normalized)
        yield normalized
    finally:
        authoring.configure(animation, previous)


def find_clip(name: str):
    if not isinstance(name, str):
        raise ValueError("A clip name is required")
    skeleton = animation.Skeleton(rig_bones())
    for builder in animation.FAMILY_BUILDERS:
        family, clips = builder()
        clip = next((item for item in clips if item.name == name), None)
        if clip is not None:
            animation.prepare_choreography(clip)
            return skeleton, family, clip
    raise ValueError("Unknown clip")


def validate_draft(profile: dict) -> dict:
    """Check references and key times as well as the profile's JSON shape."""
    with draft_profile(profile) as normalized:
        animation.Skeleton(rig_bones())
        known = set()
        for builder in animation.FAMILY_BUILDERS:
            _, clips = builder()
            for clip in clips:
                known.add(clip.name)
                authoring.apply_clip(clip, animation)
        if set(normalized.get("clips", {})) - known:
            raise ValueError("Unknown edited clip")
        return copy.deepcopy(normalized)


def history_entry(identifier: str) -> dict:
    if not isinstance(identifier, str) or not HISTORY_ID.fullmatch(identifier):
        raise ValueError("Invalid history identifier")
    path = HISTORY / (identifier + ".json")
    if path.resolve().parent != HISTORY.resolve() or path.is_symlink():
        raise ValueError("Invalid history path")
    data = path.read_bytes()
    document = json.loads(data)
    if "profile" in document:
        return {"id": identifier, "created": document["created"], "reason": document.get("reason", "save"),
                "revision": document["revision"], "profile": document["profile"]}
    return {"id": identifier, "created": datetime.fromtimestamp(path.stat().st_mtime, timezone.utc).isoformat(),
            "reason": "Before editor upgrade", "revision": hashlib.sha256(data).hexdigest(), "profile": document}


def history_list() -> list[dict]:
    entries = []
    if HISTORY.exists():
        for path in HISTORY.glob("*.json"):
            if not HISTORY_ID.fullmatch(path.stem):
                continue
            try:
                entry = history_entry(path.stem)
                entries.append({key: value for key, value in entry.items() if key != "profile"})
            except (ValueError, KeyError, OSError):
                continue
    return sorted(entries, key=lambda entry: entry["created"], reverse=True)


def snapshot(profile: dict, revision: str, reason: str) -> str:
    HISTORY.mkdir(parents=True, exist_ok=True)
    now = datetime.now(timezone.utc)
    identifier = now.strftime("%Y%m%dT%H%M%S%fZ-") + uuid.uuid4().hex[:12]
    document = {"id": identifier, "created": now.isoformat(), "reason": reason,
                "revision": revision, "profile": profile}
    # Exclusive creation preserves every previous snapshot, even on retries.
    with (HISTORY / (identifier + ".json")).open("x", encoding="utf-8") as output:
        json.dump(document, output, allow_nan=False, indent=2)
        output.write("\n")
    return identifier


@serialized
def save_profile(profile: dict, expected_revision: str | None = None, reason: str = "Before save") -> dict:
    current, revision = profile_state()
    if expected_revision is not None and expected_revision != revision:
        raise ProfileConflict(current, revision)
    normalized = validate_draft(profile)
    latest, latest_revision = profile_state()
    if latest_revision != revision:
        raise ProfileConflict(latest, latest_revision)
    snapshot(current, revision, reason)
    authoring.save(normalized, set(rig_bones()))
    _clip_data_cached.cache_clear()
    return {"saved": True, "profile": profile_state()[0], "revision": profile_state()[1], "history": history_list()}


def tree_hashes(folder: Path) -> dict[str, str]:
    if not folder.is_dir():
        return {}
    return {path.relative_to(folder).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(folder.rglob("*")) if path.is_file()}


def export_status() -> dict:
    source, playable = tree_hashes(PACK), tree_hashes(PLAYABLE)
    try:
        recorded = json.loads(EXPORT_STATUS.read_text())
    except (OSError, ValueError):
        recorded = {}
    revision = profile_state()[1]
    copies_match = bool(source) and source == playable
    recorded_match = bool(recorded.get("packHash")) and recorded["packHash"] == hashlib.sha256(
        json.dumps(source, sort_keys=True).encode()).hexdigest()
    return {**recorded, "sourceFiles": len(source), "playableFiles": len(playable),
            "sourceMatchesPlayable": copies_match, "recordedPackMatches": recorded_match,
            "profileRevision": recorded.get("profileRevision"), "savedRevision": revision,
            "outOfDate": recorded.get("profileRevision") != revision or not copies_match or not recorded_match,
            "playablePath": str(PLAYABLE)}


@serialized
def catalog() -> dict[str, Any]:
    profile, revision = profile_state()
    with draft_profile(profile):
        animation.Skeleton(rig_bones())
        clips = []
        for builder in animation.FAMILY_BUILDERS:
            family, family_clips = builder()
            clips.extend({"name": clip.name, "length": clip.length, "model": family.gun.model,
                          "family": builder.__name__} for clip in family_clips)
    return {"clips": clips, "profile": profile, "defaults": authoring.defaults(),
            "revision": revision, "history": history_list(), "export": export_status()}


def clip_metadata(skeleton, family, clip) -> dict:
    edits = authoring.ACTIVE.get("clips", {}).get(clip.name, {})
    def key_origin(track: str, time: float) -> dict:
        override = next((key for key in edits.get(track, []) if abs(key["time"] - time) <= 1e-8), None)
        return {"sourceTime": override["sourceTime"]} if override is not None and "sourceTime" in override else {}
    return {"name": clip.name, "length": clip.length, "model": family.gun.model, "item": ITEMS[family.gun.model],
            "bones": skeleton.names,
            "support": {"position": family.gun.support.p, "rotation": family.gun.support.q},
            "supportKeys": authoring.linked_support_keys(clip) if hasattr(authoring, "linked_support_keys") else [],
            "attachmentRotation": family.stance.prop_right,
            "keys": [{"time": t, "space": hand.space, "position": hand.frame.p, "rotation": hand.frame.q,
                      "arc": hand.arc, "easing": easing, **key_origin("left", t)} for t, hand, easing in clip.left.keys],
            "gunKeys": [{"time": t, "value": value, "easing": easing, **key_origin("gun", t)}
                        for t, value, easing in clip.gun.keys],
            "gripKeys": {side: [{"time": t, **asdict(value), "easing": easing}
                                for t, value, easing in getattr(clip, side + "_grip").keys]
                         for side in ("left", "right")},
            "cues": [{"time": cue.time, **cue.action} for cue in clip.cues]}


def frame_data(skeleton, family, clip, time, solved, parts) -> dict:
    specs = animation.PARTS.get(family.gun.model, animation.PARTS.get("modern/" + family.gun.model, []))
    visibility = {part["name"]: not part.get("hidden", False) for part in specs}
    for cue in sorted(clip.cues, key=lambda cue: cue.time):
        if cue.time > time:
            break
        for action, value in cue.action.items():
            if action == "show": visibility[value] = True
            elif action in ("hide", "drop"): visibility[value] = False
    target = solved.view.compose(clip.pose(time).left)
    wrist = solved.frames["WristLeft"]
    actual = wrist.apply(animation.PALM["Left"])
    linked = (authoring.support_is_active(clip, time) if hasattr(authoring, "support_is_active") else False)
    return {"time": time, "rotations": [solved.rotations.get(bone, animation.IDENTITY) for bone in skeleton.names],
            "camera": [*solved.camera.p, *solved.camera.q], "view": [*solved.view.p, *solved.view.q],
            "gun": [*solved.gun.p, *solved.gun.q], "target": target.p, "targetRotation": target.q,
            "wristPosition": wrist.p, "wristRotation": wrist.q, "palm": actual,
            "error": animation.v_len(animation.v_sub(target.p, actual)),
            "wristCorrection": animation.q_angle(animation.q_mul(wrist.q, animation.q_inv(target.q))),
            "supportsLinked": linked, "parts": {name: [*value.p, *value.q] for name, value in parts.items()},
            "visible": visibility}


def solve_single(skeleton, clip, requested_time: float):
    """Isolated interactive pose, with carried props calibrated to real wrists.

    This intentionally avoids baking the preceding clip while dragging. A
    settled/full request uses evaluate(), including its temporal elbow/wrist
    continuity. The response identifies this interactive sampling mode.
    """
    if isinstance(requested_time, bool) or not isinstance(requested_time, (int, float)) or not math.isfinite(requested_time):
        raise ValueError("Preview time must be a finite number")
    sample_time = max(0.0, min(clip.length, float(requested_time)))
    solved_cache = {}
    def solve(time):
        if time not in solved_cache:
            solved_cache[time] = animation.solve_pose(skeleton, clip.pose(time))
        return solved_cache[time]
    solved = solve(sample_time)
    parts = {name: track.value(sample_time) for name, track in clip.parts.items()}
    for name, (_, start, end, pivot) in clip.part_follow.items():
        fresh = name.endswith("_new") or name in ("shell", "clip")
        sample_start = 0.0 if fresh else math.floor(start * animation.FRAME_RATE + 1e-8) / animation.FRAME_RATE
        sample_end = math.ceil(end * animation.FRAME_RATE - 1e-8) / animation.FRAME_RATE
        if not sample_start <= sample_time <= sample_end:
            continue
        contact = end if fresh else start
        # An interactive frame is solved at its exact time, not interpolated
        # between exported 30 Hz bone samples. Calibrate its grip the same way
        # so dragging at the seating key cannot introduce a small prop offset.
        contact_frames = solve(contact).frames
        grip = contact_frames["WristLeft"].inverse().compose(contact_frames["PropRight"].compose(
            animation.Frame(pivot, animation.IDENTITY)))
        held = solved.gun.inverse().compose(solved.frames["WristLeft"].compose(grip))
        parts[name] = animation.Frame(animation.v_sub(held.p, pivot), held.q)
    return sample_time, solved, parts


def preview(request: dict) -> dict:
    if not isinstance(request, dict) or "profile" not in request:
        raise ValueError("Preview requires a draft profile")
    started = time.perf_counter()
    with LOCK, draft_profile(request["profile"]):
        skeleton, family, clip = find_clip(request.get("name"))
        result = clip_metadata(skeleton, family, clip)
        if "time" in request:
            sampled = solve_single(skeleton, clip, request["time"])
            result["frame"] = frame_data(skeleton, family, clip, *sampled)
            result["supportsLinked"] = result["frame"]["supportsLinked"]
            result["previewMode"] = "pose"
        else:
            result["frames"] = [frame_data(skeleton, family, clip, *sampled)
                                for sampled in animation.evaluate(skeleton, clip)]
            result["previewMode"] = "clip"
        result["requestId"] = request.get("requestId")
        result["elapsedMs"] = (time.perf_counter() - started) * 1000
        return result


@functools.lru_cache(maxsize=32)
def _clip_data_cached(name: str, revision: str, profile_json: str) -> dict[str, Any]:
    return preview({"name": name, "profile": json.loads(profile_json)})


@serialized
def clip_data(name: str) -> dict[str, Any]:
    profile, revision = profile_state()
    return _clip_data_cached(name, revision, json.dumps(profile, sort_keys=True))


def remove_owned(path: Path, parent: Path) -> None:
    """Only remove a generated staging/backup directory inside its fixed root."""
    resolved, allowed = path.resolve(), parent.resolve()
    if resolved == allowed or not resolved.is_relative_to(allowed) or path.is_symlink():
        raise ValueError("Refusing to remove a directory outside the staging root")
    if path.exists():
        shutil.rmtree(path)


def publish_pack(staged: Path, destinations: tuple[Path, ...], on_commit=None) -> dict[str, str]:
    """Prepare complete copies first, then swap; roll back every target on error."""
    expected = tree_hashes(staged)
    if not expected:
        raise ValueError("Generated pack is empty")
    token = uuid.uuid4().hex
    rollback_root = staged.parent / ("rollback-" + token)
    copies, backups, installed = {}, {}, []
    try:
        for target in destinations:
            target.parent.mkdir(parents=True, exist_ok=True)
            incoming = target.with_name(target.name + ".riglab-new-" + token)
            copies[target] = incoming
            shutil.copytree(staged, incoming)
            if tree_hashes(incoming) != expected:
                raise OSError("Staged pack verification failed")
        rollback_root.mkdir()
        for index, target in enumerate(destinations):
            # Keep rollback copies outside the two scanned Add-Ons folders.
            # A scanner holding an old file must not leave a duplicate pack
            # visible to the game if delayed cleanup is needed on Windows.
            backup = rollback_root / str(index)
            if target.exists():
                target.rename(backup)
                backups[target] = backup
            copies[target].rename(target)
            installed.append(target)
        if on_commit is not None:
            on_commit(expected)
    except Exception:
        for target in reversed(destinations):
            if target in installed:
                remove_owned(target, target.parent)
            if target in backups:
                backups[target].rename(target)
        raise
    finally:
        for incoming in copies.values():
            if incoming.exists():
                remove_owned(incoming, incoming.parent)
        if rollback_root.exists() and not any(rollback_root.iterdir()):
            rollback_root.rmdir()
    try:
        remove_owned(rollback_root, staged.parent)
    except OSError:
        # A scanner can briefly hold an old file on Windows. The complete old
        # copy stays outside Add-Ons, and the committed export remains valid.
        pass
    return expected


@serialized
def build_pack(request: dict) -> dict:
    """Explicit UI export; failed generation/validation never changes either pack."""
    if not isinstance(request, dict):
        raise ValueError("Build request must be an object")
    if "profile" in request:
        save_profile(request["profile"], request.get("revision"), "Before build")
    else:
        current, revision = profile_state()
        if request.get("revision") is not None and request["revision"] != revision:
            raise ProfileConflict(current, revision)
        validate_draft(current)
    revision = profile_state()[1]
    staging_root = WORK / "builds"
    staging_root.mkdir(parents=True, exist_ok=True)
    staged = staging_root / uuid.uuid4().hex
    log = ""
    try:
        shutil.copytree(PACK, staged)
        commands = ([sys.executable, str(UI.parent / "make_tactical_animations.py"), str(WORK / "rig.glb"), str(staged)],
                    [str(SDK), "validate", str(staged), "--strict"])
        for command in commands:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600)
            log += result.stdout + result.stderr
            if result.returncode:
                return {"ok": False, "log": log, "revision": revision, "export": export_status()}
        if profile_state()[1] != revision:
            raise ValueError("The saved profile changed during export. Neither pack was replaced; rebuild the latest draft.")
        def record_export(hashes):
            record = {"profileRevision": revision, "builtAt": datetime.now(timezone.utc).isoformat(),
                      "files": len(hashes), "packHash": hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()}
            EXPORT_STATUS.parent.mkdir(parents=True, exist_ok=True)
            temporary = EXPORT_STATUS.with_suffix(".json.tmp")
            temporary.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            temporary.replace(EXPORT_STATUS)
        publish_pack(staged, (PACK, PLAYABLE), record_export)
        weapon.cache_clear()
        _clip_data_cached.cache_clear()
        return {"ok": True, "log": log, "revision": revision, "history": history_list(), "export": export_status()}
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return {"ok": False, "log": log + "\n" + str(error), "revision": revision, "export": export_status()}
    finally:
        remove_owned(staged, staging_root)


class Handler(BaseHTTPRequestHandler):
    """Explicit read routes; writes are restricted to same-origin editor actions."""
    server_version = "OpenMinerZRigLab/2"
    def send_json(self, value: Any, status: int = 200) -> None:
        payload = json.dumps(value, allow_nan=False, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:
        if self.headers.get("Host") not in (f"127.0.0.1:{self.server.server_port}",f"localhost:{self.server.server_port}"):
            return self.send_error(403)
        try:
            url = urlsplit(self.path)
            query = parse_qs(url.query)
            if url.path == "/api/identity":
                # Reopening the editor must also work while an export holds
                # the solver lock. This route has no mutable solver/file state.
                return self.send_json({"service": SERVICE, "apiVersion": 2, "project": str(ROOT),
                                       "sourceRevision": SOURCE_REVISION})
            with LOCK:
                if url.path == "/api/catalog": return self.send_json(catalog())
                if url.path == "/api/clip": return self.send_json(clip_data(query["name"][0]))
                if url.path == "/api/weapon": return self.send_json(weapon(query["model"][0]))
                if url.path == "/api/history": return self.send_json(history_entry(query["id"][0]))
                if url.path == "/api/status":
                    return self.send_json({"service": SERVICE, "apiVersion": 2,
                                           "project": str(ROOT), "sourceRevision": SOURCE_REVISION,
                                           "export": export_status()})
            roots = {"/scene/": WORK / "scene", "/sounds/": PACK / "assets/sounds", "/": UI}
            prefix = next(p for p in roots if url.path.startswith(p))
            root = roots[prefix].resolve()
            relative = unquote(url.path[len(prefix):]) or "index.html"
            path = (root / relative).resolve()
            if not path.is_relative_to(root) or not path.is_file():
                return self.send_error(404)
            payload = path.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", mimetypes.guess_type(path)[0] or "application/octet-stream")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)
        except (ValueError, KeyError, TypeError, IndexError, OSError) as error:
            self.send_json({"error": str(error)}, 400)

    def do_POST(self) -> None:
        if self.headers.get("Host") not in (f"127.0.0.1:{self.server.server_port}",f"localhost:{self.server.server_port}"):
            return self.send_error(403)
        expected = f"http://{self.headers.get('Host')}"
        if self.headers.get("Origin") != expected or self.headers.get("Content-Type", "").split(";")[0] != "application/json":
            return self.send_json({"error": "Same-origin JSON editor request required"}, 403)
        try: length = int(self.headers.get("Content-Length", "0"))
        except ValueError: return self.send_json({"error":"Invalid request size"},400)
        if not 0 < length <= 1024 * 1024:
            return self.send_json({"error": "Invalid request size"}, 400)
        try:
            request = json.loads(self.rfile.read(length))
            with LOCK:
                if self.path == "/api/preview":
                    return self.send_json(preview(request))
                if self.path == "/api/profile":
                    if not isinstance(request, dict):
                        raise ValueError("Profile request must be an object")
                    return self.send_json(save_profile(request.get("profile", request), request.get("revision")))
                if self.path == "/api/build":
                    return self.send_json(build_pack(request))
            self.send_error(404)
        except ProfileConflict as error:
            self.send_json({"error": str(error), "revision": error.revision, "profile": error.current}, 409)
        except (ValueError, KeyError, TypeError, IndexError, OSError) as error:
            self.send_json({"error": str(error)}, 400)


def existing_server(port: int) -> bool:
    """Recognize our server before reusing a port; never stop another process."""
    try:
        with urlopen(f"http://127.0.0.1:{port}/api/identity", timeout=2) as response:
            result = json.load(response)
        return result.get("service") == SERVICE and result.get("project") == str(ROOT)
    except (OSError, URLError, ValueError):
        return False


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--avatar", type=Path)
    parser.add_argument("--port", type=int, default=8731)
    parser.add_argument("--open", action="store_true", help="Open Rig Lab in your browser")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("Port must be between 1 and 65535")
    url = f"http://127.0.0.1:{args.port}"
    if existing_server(args.port):
        print(f"Reusing Rig Lab: {url}", flush=True)
        if args.open: webbrowser.open(url)
        return
    if args.avatar is None:
        local_avatar=Path(os.environ.get("ProgramFiles(x86)","C:/Program Files (x86)"))/"Steam/steamapps/common/CastleMiner Z/!Mods/XboxAvatar/avatar.ocavatar"
        if local_avatar.is_file(): args.avatar=local_avatar
    if args.prepare: prepare(args.avatar)
    if not (WORK / "scene/rig.json").exists() or not (WORK / "rig.glb").exists(): parser.error("Run --prepare first")
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError:
        # A second launch may have won the bind after the initial probe.
        if existing_server(args.port):
            print(f"Reusing Rig Lab: {url}", flush=True)
            if args.open: webbrowser.open(url)
            return
        parser.error(f"Port {args.port} is occupied. Choose another --port; no process was stopped.")
    print(f"Rig Lab: {url}", flush=True)
    if args.open: webbrowser.open(url)
    with server:
        server.serve_forever()


if __name__ == "__main__":
    main()
