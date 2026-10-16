"""CPU skin inspection of this installation's native Rig Lab export.

The scene contains owned assets and stays under out/rig-lab. These helpers use
the stock and Xbox avatar mappings separately, matching the exported native
attachment/body/avatar chains rather than treating every skin as one skeleton.
"""
from __future__ import annotations

import numpy as np

import make_tactical_animations as animation


def matrix(position, rotation, scale=(1., 1., 1.)) -> np.ndarray:
    result = np.eye(4)
    result[:3, :3] = np.array([animation.q_rotate(rotation, axis)
                               for axis in (animation.X, animation.Y, animation.Z)]).T @ np.diag(scale)
    result[:3, 3] = position
    return result


def skin_matrices(scene: dict, solved, kind: str) -> np.ndarray:
    """Native bone world matrices multiplied by each skin's inverse bind."""
    rotations = [solved.rotations.get(bone["name"], animation.IDENTITY) for bone in scene["bones"]]
    attachment = []
    for index, bone in enumerate(scene["bones"]):
        local = matrix(bone["position"], rotations[index])
        attachment.append(local if bone["parent"] < 0 else attachment[bone["parent"]] @ local)
    body = [None] * len(scene["body"])
    root = np.array(scene["bodyRoot"]).reshape(4, 4).T
    for index in scene["bodyOrder"]:
        bone = scene["body"][index]
        local = matrix(bone["position"], rotations[bone["source"]])
        body[index] = (root @ attachment[bone["source"]] if bone["parent"] < 0
                       else body[bone["parent"]] @ local)
    if kind == "stock":
        bones, matrices = scene["body"], body
    elif kind == "avatar":
        mapped = {bone["source"]: index for index, bone in reversed(list(enumerate(scene["body"])))}
        matrices, below_wrist = [], []
        for index, bone in enumerate(scene["avatarBones"]):
            parent = scene["bones"][index]["parent"]
            below_wrist.append(parent >= 0 and (scene["bones"][parent]["name"] in ("WristLeft", "WristRight")
                                                or below_wrist[parent]))
            if index in mapped and not below_wrist[index]:
                matrices.append(body[mapped[index]] @ np.diag([*bone["cumulativeScale"], 1.]))
            elif parent < 0:
                matrices.append(attachment[index])
            else:
                matrices.append(matrices[parent] @ matrix(bone["position"], rotations[index], bone["scale"]))
        bones = scene["avatarBones"]
    else:
        raise ValueError(f"Unknown skin {kind!r}")
    return np.array([world @ np.array(bone["inverseBind"]).reshape(4, 4).T
                     for world, bone in zip(matrices, bones)])


def hand_points(scene: dict, solved, kind: str, finger: str | None = None, side: str = "Left") -> np.ndarray:
    """Actual skinned hand vertices, or distal finger vertices, in gun space."""
    bones = scene["body"] if kind == "stock" else scene["avatarBones"]
    names = [scene["bones"][bone["source"] if kind == "stock" else index]["name"]
             for index, bone in enumerate(bones)]
    if finger:
        influence = np.array([name == f"Finger{finger}3{side}" for name in names])
        threshold = .3
    else:
        influence = np.array([name.endswith(side) and ("Wrist" in name or "Finger" in name) for name in names])
        threshold = .01
    matrices = skin_matrices(scene, solved, kind)
    # Native weapons attach to the solved PropRight socket. Do not let an
    # unreachable intended gun frame make a displaced mesh pass the audit.
    gun = solved.frames["PropRight"]
    inverse_gun = np.linalg.inv(matrix(gun.p, gun.q))
    clouds = []
    for surface in scene["surfaces"]:
        if surface["skin"] != kind:
            continue
        joints = np.array(surface["joints"])
        weights = np.array(surface["weights"])
        positions = np.array(surface["position"])
        mask = np.sum(np.where(influence[joints], weights, 0.), axis=1) > threshold
        if not np.any(mask):
            continue
        vertices = np.einsum("vj,vjab,vb->va", weights[mask], matrices[joints[mask]],
                             np.c_[positions[mask], np.ones(mask.sum())])
        clouds.append((vertices @ inverse_gun.T)[:, :3])
    if not clouds:
        raise ValueError(f"No {kind} {side} {finger or 'hand'} skin vertices")
    return np.unique(np.round(np.vstack(clouds), 6), axis=0)


def surface_distances(points: np.ndarray, triangles: np.ndarray) -> np.ndarray:
    """Exact point-to-triangle distances, including edge and vertex regions."""
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    ab, ac = b - a, c - a
    normal = np.cross(ab, ac)
    lengths = np.linalg.norm(normal, axis=1)
    normal /= np.maximum(lengths[:, None], 1e-12)
    d00, d01, d11 = np.sum(ab * ab, axis=1), np.sum(ab * ac, axis=1), np.sum(ac * ac, axis=1)
    denominator = d00 * d11 - d01 * d01
    result = []
    for point in points:
        plane = np.sum((point - a) * normal, axis=1)
        projected = point - plane[:, None] * normal - a
        d20, d21 = np.sum(projected * ab, axis=1), np.sum(projected * ac, axis=1)
        v = np.divide(d11 * d20 - d01 * d21, denominator, out=np.zeros(len(a)), where=abs(denominator) > 1e-15)
        w = np.divide(d00 * d21 - d01 * d20, denominator, out=np.zeros(len(a)), where=abs(denominator) > 1e-15)
        inside = (v >= 0) & (w >= 0) & (v + w <= 1) & (lengths > 1e-10)
        distance = np.where(inside, abs(plane), np.inf)
        for start, end in ((a, b), (b, c), (c, a)):
            edge = end - start
            blend = np.clip(np.sum((point - start) * edge, axis=1)
                            / np.maximum(np.sum(edge * edge, axis=1), 1e-20), 0., 1.)
            distance = np.minimum(distance, np.linalg.norm(point - start - blend[:, None] * edge, axis=1))
        result.append(distance.min())
    return np.array(result)
