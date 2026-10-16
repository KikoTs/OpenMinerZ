"""Inspect installed weapon geometry using the native GunFX island rules.

No game geometry is copied into the mod. Reports and optional images go under
``out`` and use the locally imported OMSM models.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import struct
from typing import Any

import numpy as np

import make_tactical_animations as animation

ROOT = Path(__file__).resolve().parents[2]
CONTENT = ROOT / "out/build/native-release/content-packs"


@dataclass
class Mesh:
    """Model-space triangles and their native welded connected components."""

    positions: np.ndarray
    triangles: np.ndarray
    components: np.ndarray
    bounds: list[tuple[np.ndarray, np.ndarray]]


def connected_components(positions: np.ndarray, triangles: np.ndarray) -> tuple[np.ndarray, list]:
    """Match C++ llround(float32(position * 1e4)), including ties away from zero."""
    quantized = np.asarray(positions, dtype=np.float32) * np.float32(1e4)
    keys = np.copysign(np.floor(np.abs(quantized.astype(np.float64)) + .5), quantized).astype(np.int64)
    parent = list(range(len(positions)))

    def root(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    welded: dict[tuple[int, ...], int] = {}
    for i, key in enumerate(keys):
        previous = welded.setdefault(tuple(int(value) for value in key), i)
        parent[root(i)] = root(previous)
    for a, b, c in triangles:
        parent[root(int(b))] = root(int(a))
        parent[root(int(c))] = root(int(a))
    numbered: dict[int, int] = {}
    indices = np.array([numbered.setdefault(root(int(tri[0])), len(numbered)) for tri in triangles])
    bounds = []
    for component in range(len(numbered)):
        vertices = positions[triangles[indices == component]].reshape(-1, 3)
        bounds.append((vertices.min(axis=0), vertices.max(axis=0)))
    return indices, bounds


def read_model(model: str) -> list[Mesh]:
    """Read only geometry from an imported model, applying each mesh transform."""
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
        raise ValueError("Unsupported imported model format")
    meshes = []
    for _ in range(count):
        matrix = np.array(read("16f"), dtype=np.float32).reshape(4, 4)
        if version >= 3:
            read("B")
        read("3f")
        width, height = read("ii")
        offset += width * height * 4
        positions = []
        for _ in range(read("i")[0]):
            position = read("3f")
            read("3f2f")
            if version >= 2:
                read("4B")
            positions.append((np.array([*position, 1], dtype=np.float32) @ matrix)[:3])
        triangles = np.array(read(f"{read('i')[0]}I")).reshape(-1, 3)
        vertices = np.array(positions, dtype=np.float32)
        components, bounds = connected_components(vertices, triangles)
        meshes.append(Mesh(vertices, triangles, components, bounds))
    if offset != len(data):
        raise ValueError(f"Unparsed model bytes: {len(data) - offset}")
    return meshes


def select_parts(meshes: list[Mesh], specs: list[dict[str, Any]]) -> list[dict[str, np.ndarray]]:
    """Apply the native first-owner cut semantics to each mesh."""
    result = []
    for mesh_index, mesh in enumerate(meshes):
        remaining = np.ones(len(mesh.triangles), dtype=bool)
        selected: dict[str, np.ndarray] = {}
        centres = mesh.positions[mesh.triangles].mean(axis=1)
        island_centres = np.array([(low + high) * .5 for low, high in mesh.bounds])[mesh.components]
        for spec in specs:
            if "copy" in spec:
                selected[spec["name"]] = selected[spec["copy"]].copy()
                continue
            if "model" in spec:
                continue
            match = remaining.copy()
            if "mesh" in spec and spec["mesh"] != mesh_index:
                match[:] = False
            boxes = spec.get("islands", [spec["box"]] if "box" in spec else [])
            points = island_centres if "islands" in spec else centres
            if boxes:
                inside = np.zeros(len(mesh.triangles), dtype=bool)
                for low, high in boxes:
                    inside |= np.all((points >= low) & (points <= high), axis=1)
                for low, high in spec.get("exclude", []):
                    inside &= ~np.all((points >= low) & (points <= high), axis=1)
                match &= inside
            selected[spec["name"]] = np.flatnonzero(match)
            remaining[match] = False
        selected["body"] = np.flatnonzero(remaining)
        result.append(selected)
    return result


def report(model: str, meshes: list[Mesh], selected: list[dict[str, np.ndarray]]) -> dict:
    """List every component's size, bounds, and partial cuts."""
    result: dict[str, Any] = {"model": model, "meshes": []}
    for mesh, parts in zip(meshes, selected):
        entries = []
        for i, (low, high) in enumerate(mesh.bounds):
            counts = {name: int(np.sum(mesh.components[indices] == i)) for name, indices in parts.items()}
            counts = {name: count for name, count in counts.items() if count}
            entries.append({"island": i, "triangles": int(np.sum(mesh.components == i)),
                            "min": low.tolist(), "max": high.tolist(),
                            "centre": ((low + high) * .5).tolist(), "ownership": counts})
        result["meshes"].append(entries)
    return result


def plot(model: str, meshes: list[Mesh], selected: list[dict[str, np.ndarray]], path: Path) -> None:
    """Render orthographic mesh diagnostics; island numbers identify source geometry."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection

    fig, axes = plt.subplots(2, 2, figsize=(16, 10))
    views = [(0, 2, "Side: barrel left, up"), (0, 1, "Top: barrel left, right side up")]
    for row, (horizontal, vertical, title) in enumerate(views):
        for column in range(2):
            ax = axes[row, column]
            for mesh, parts in zip(meshes, selected):
                if column == 0:
                    colors = plt.cm.tab20(mesh.components % 20 / 20)
                else:
                    colors = np.tile([.3, .33, .36, 1.], (len(mesh.triangles), 1))
                    for number, (name, indices) in enumerate(parts.items()):
                        if name == "body" or name.endswith("_new"):
                            continue
                        colors[indices] = plt.cm.Set1(number / 9)
                polys = mesh.positions[mesh.triangles][:, :, [horizontal, vertical]]
                ax.add_collection(PolyCollection(polys, facecolors=colors, edgecolors="black", linewidths=.25))
                if column == 0:
                    for i, (low, high) in enumerate(mesh.bounds):
                        centre = (low + high) * .5
                        ax.text(centre[horizontal], centre[vertical], str(i), fontsize=8, color="black",
                                bbox={"facecolor": "white", "alpha": .75, "pad": 1})
            ax.autoscale()
            ax.set_aspect("equal")
            ax.set_title(title + (" — islands" if column == 0 else " — moving parts"))
            ax.grid(alpha=.2)
    fig.suptitle(model)
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=150)
    plt.close(fig)


def plot_exploded(model: str, meshes: list[Mesh], selected: list[dict[str, np.ndarray]],
                  specs: list[dict[str, Any]], path: Path) -> None:
    """Show the selected surfaces displaced, making incomplete walls conspicuous."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection

    fig, axes = plt.subplots(1, 2, figsize=(14, 7))
    by_name = {spec["name"]: spec for spec in specs}
    palette = {"body": "#535c66", "mag": "#e69f00", "box": "#e69f00",
               "cover": "#009e73", "slide": "#56b4e9", "hammer": "#535c66"}
    for axis, explode in zip(axes, (False, True)):
        for mesh, parts in zip(meshes, selected):
            for name, indices in parts.items():
                if name.endswith("_new") or not len(indices):
                    continue
                vertices = mesh.positions.copy()
                if explode and name in ("mag", "box"):
                    vertices[:, 2] -= .22
                elif explode and name == "slide":
                    vertices[:, 0] += .032
                elif explode and name == "cover":
                    pivot = np.array(by_name[name]["pivot"])
                    angle = np.deg2rad(-25)
                    rotation = np.array([[np.cos(angle), 0, np.sin(angle)], [0, 1, 0],
                                         [-np.sin(angle), 0, np.cos(angle)]])
                    vertices = (vertices - pivot) @ rotation.T + pivot
                triangles = vertices[mesh.triangles[indices]][:, :, [0, 2]]
                axis.add_collection(PolyCollection(triangles, facecolors=palette.get(name, "#cc79a7"),
                                                   edgecolors="#222222", linewidths=.2))
        axis.autoscale()
        axis.set_aspect("equal")
        axis.grid(alpha=.15)
        axis.set_title("Installed geometry" if not explode else "Magazine removed / action open")
        axis.set_xlabel("Model X (m)")
        axis.set_ylabel("Model Z (m)")
    fig.suptitle(model + " — selected geometry, no replacement weapon mesh")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model")
    parser.add_argument("--out", type=Path, default=ROOT / "out/rig-lab/mesh-audit")
    args = parser.parse_args()
    meshes = read_model(args.model)
    specs = animation.PARTS.get(args.model, animation.PARTS.get(args.model.split("/")[-1], []))
    selected = select_parts(meshes, specs)
    name = args.model.replace("/", "-")
    args.out.mkdir(parents=True, exist_ok=True)
    result = report(args.model, meshes, selected)
    (args.out / f"{name}.json").write_text(json.dumps(result, indent=2) + "\n")
    plot(args.model, meshes, selected, args.out / f"{name}.png")
    plot_exploded(args.model, meshes, selected, specs, args.out / f"{name}-exploded.png")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
