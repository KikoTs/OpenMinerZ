"""Validate SMG support against the installed stock and avatar skin geometry.

Run after the native rig/scene export (requires NumPy):
    py -3 tools/ModSdk/test_tactical_smg_skin.py out/rig-lab/rig.glb out/rig-lab/scene/rig.json

The scene stays local. No source skin vertices or animation keys are embedded
in this test. A correct virtual PALM point is insufficient: the actual hand
must touch the fore-end and leave the line between the sights unobstructed.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

import make_tactical_animations as animation
import rig_lab
import tactical_authoring


def matrix(position: tuple, rotation: tuple, scale: tuple = (1, 1, 1)) -> np.ndarray:
    result = np.eye(4)
    result[:3, :3] = np.array([
        animation.q_rotate(rotation, axis) for axis in (animation.X, animation.Y, animation.Z)
    ]).T @ np.diag(scale)
    result[:3, 3] = position
    return result


class NativeSkin:
    """CPU counterpart of the exported native body and avatar bone mapping."""

    def __init__(self, scene: dict):
        self.scene = scene
        self.names = [bone["name"] for bone in scene["bones"]]
        self.mapping = {bone["source"]: i for i, bone in reversed(list(enumerate(scene["body"])))}
        self.surfaces = []
        for surface in scene["surfaces"]:
            kind = surface["skin"]
            joints = np.array(surface["joints"])
            weights = np.array(surface["weights"])
            sources = (np.arange(len(self.names)) if kind == "avatar"
                       else np.array([bone["source"] for bone in scene["body"]]))
            source_joints = sources[joints]
            dominant = source_joints[np.arange(len(joints)), weights.argmax(axis=1)]
            used = np.unique(surface["indices"])
            used = used[np.array([self.names[index].endswith("Left") and
                                 ("Wrist" in self.names[index] or "Finger" in self.names[index])
                                 for index in dominant[used]])]
            if len(used):
                positions = np.array(surface["position"])[used]
                self.surfaces.append((kind, joints[used], weights[used],
                                      np.c_[positions, np.ones(len(used))], dominant[used]))

    def worlds(self, rotations: list) -> tuple:
        scene = self.scene
        attachment, avatar, below_wrist = [], [], []
        body = [None] * len(scene["body"])
        for i, bone in enumerate(scene["bones"]):
            local = matrix(bone["position"], rotations[i])
            attachment.append(local if bone["parent"] < 0 else attachment[bone["parent"]] @ local)
        for i in scene["bodyOrder"]:
            bone = scene["body"][i]
            local = matrix(bone["position"], rotations[bone["source"]])
            body[i] = (np.array(scene["bodyRoot"]).reshape(4, 4).T @ attachment[bone["source"]]
                       if bone["parent"] < 0 else body[bone["parent"]] @ local)
        for i, bone in enumerate(scene["avatarBones"]):
            parent = scene["bones"][i]["parent"]
            below_wrist.append(parent >= 0 and (self.names[parent] in ("WristLeft", "WristRight")
                                               or below_wrist[parent]))
            if i in self.mapping and not below_wrist[i]:
                avatar.append(body[self.mapping[i]] @ np.diag([*bone["cumulativeScale"], 1]))
            elif parent < 0:
                avatar.append(attachment[i])
            else:
                avatar.append(avatar[parent] @ matrix(bone["position"], rotations[i], bone["scale"]))
        return body, avatar

    def parity_error(self) -> float:
        error = 0.0
        for reference in self.scene["parity"]:
            for kind, actual in zip(("body", "avatar"), self.worlds(reference["rotation"])):
                expected = np.array(reference[kind]).reshape(-1, 4, 4).transpose(0, 2, 1)
                error = max(error, float(np.max(np.abs(np.array(actual) - expected))))
        return error

    def vertices(self, solved: animation.Solved) -> dict:
        rotations = [solved.rotations.get(name, animation.IDENTITY) for name in self.names]
        gun_inverse = np.linalg.inv(matrix(solved.gun.p, solved.gun.q))
        skin_matrices = {}
        for kind, worlds, bones in zip(("stock", "avatar"), self.worlds(rotations),
                                      (self.scene["body"], self.scene["avatarBones"])):
            skin_matrices[kind] = np.array([
                gun_inverse @ world @ np.array(bone["inverseBind"]).reshape(4, 4).T
                for world, bone in zip(worlds, bones)
            ])
        result = {kind: [] for kind in skin_matrices}
        for kind, joints, weights, positions, dominant in self.surfaces:
            vertices = np.einsum("vj,vjab,vb->va", weights, skin_matrices[kind][joints], positions)
            result[kind].append((vertices[:, :3], dominant))
        return {kind: (np.concatenate([item[0] for item in values]),
                       np.concatenate([item[1] for item in values])) for kind, values in result.items()}


def audit(rig_path: Path, scene_path: Path) -> dict:
    tactical_authoring.load()
    skeleton = animation.Skeleton(animation.read_rig(rig_path)[0])
    skin = NativeSkin(json.loads(scene_path.read_text(encoding="utf-8")))
    failures = []
    parity = skin.parity_error()
    if parity > 0.00001:
        failures.append("CPU skin matrices disagree with the native parity export")
    family, clips = animation.submachine_gun()
    # Measure the installed cylindrical fore-end, not a renderer's visual
    # approximation. Its long side triangles span the two end rings.
    shroud = []
    for surface in rig_lab.weapon("smg")["surfaces"]:
        if surface["part"] != "body":
            continue
        vertices = np.array(surface["position"])[np.unique(surface["indices"])]
        shroud.extend(vertices[(vertices[:, 0] > -0.287) & (vertices[:, 0] < -0.190)])
    shroud = np.array(shroud)
    centre = (shroud[:, 1:].min(axis=0) + shroud[:, 1:].max(axis=0)) / 2
    radius = float((shroud[:, 1:].max(axis=0) - shroud[:, 1:].min(axis=0)).mean() / 2)
    metrics = {kind: {"sight_clearance_m": float("inf"), "surface_gap_m": 0.0}
               for kind in ("stock", "avatar")}
    reach, step, wrist_bend, held_orientation_loss = 0.0, 0.0, 0.0, 0.0
    for clip in clips:
        animation.prepare_choreography(clip)
        frames = animation.evaluate(skeleton, clip)
        previous = None
        for time, solved, _ in frames:
            for arm in ("Left", "Right"):
                rotation = solved.rotations["Wrist" + arm]
                swing = animation.q_mul(rotation, animation.q_inv(animation.q_twist(rotation, animation.X)))
                wrist_bend = max(wrist_bend, animation.q_angle(swing))
            if previous is not None:
                step = max(step, *(animation.q_angle(animation.q_mul(
                    solved.rotations[bone], animation.q_inv(previous.rotations[bone])))
                    for bone in ("ShoulderLeft", "ElbowLeft", "WristLeft")))
            previous = solved
            if "Reload" in clip.name:
                continue
            local_wrist = solved.gun.inverse().compose(solved.frames["WristLeft"])
            reach = max(reach, animation.v_len(animation.v_sub(
                local_wrist.apply(animation.PALM["Left"]), family.gun.support.p)))
            held_orientation_loss = max(held_orientation_loss, animation.q_angle(animation.q_mul(
                local_wrist.q, animation.q_inv(family.gun.support.q))))
            for kind, (vertices, dominant) in skin.vertices(solved).items():
                # Only skin in the 24 mm sight corridor can obscure the sight
                # line; fingers around the far side are allowed to rise there.
                corridor = np.abs(vertices[:, 1] - family.gun.sight_eye[1]) < 0.012
                corridor &= (vertices[:, 0] > -0.32) & (vertices[:, 0] < -0.15)
                if corridor.any():
                    clearance = family.gun.sight_eye[2] - float(vertices[corridor, 2].max())
                    metrics[kind]["sight_clearance_m"] = min(metrics[kind]["sight_clearance_m"], clearance)
                for group in ("Wrist", "FingerIndex", "FingerMiddle", "FingerRing", "FingerThumb3"):
                    belongs = np.array([skin.names[index].startswith(group) for index in dominant])
                    points = vertices[belongs & (vertices[:, 0] > -0.286) & (vertices[:, 0] < -0.190)]
                    if not len(points):
                        failures.append(f"{kind}: {group} never reaches the fore-end")
                        continue
                    gap = float(np.min(np.abs(np.linalg.norm(points[:, 1:] - centre, axis=1) - radius)))
                    metrics[kind]["surface_gap_m"] = max(metrics[kind]["surface_gap_m"], gap)
    if reach > 0.003:
        failures.append("SMG support cannot reach its fore-end")
    if step > 25.0:
        failures.append("SMG support wrist or elbow turns abruptly")
    if wrist_bend > 55.1:
        failures.append("SMG wrist exceeds the anatomical bend limit")
    if held_orientation_loss > 8.0:
        failures.append("SMG wrist solve turns the held palm away from the support")
    for kind, values in metrics.items():
        if values["sight_clearance_m"] < 0.003:
            failures.append(f"{kind}: support skin crowds or obstructs the sight line")
        if values["surface_gap_m"] > 0.008:
            failures.append(f"{kind}: palm/fingers float away from the fore-end")
    return {"native_parity_error": parity, "held_reach_error_m": reach,
            "maximum_arm_step_degrees": step, "maximum_wrist_bend_degrees": wrist_bend,
            "held_orientation_loss_degrees": held_orientation_loss,
            "skins": metrics, "failures": sorted(set(failures))}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rig", type=Path)
    parser.add_argument("scene", type=Path)
    arguments = parser.parse_args()
    result = audit(arguments.rig, arguments.scene)
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result["failures"]))


if __name__ == "__main__":
    main()
