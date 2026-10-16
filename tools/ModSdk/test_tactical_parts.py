"""Regressions for complete moving geometry and native/editor cut agreement."""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import unittest
from unittest.mock import patch

import numpy as np

import make_tactical_animations as animation
import rig_lab
from tactical_mesh_audit import CONTENT, read_model, select_parts


class SelectorParityTests(unittest.TestCase):
    def test_weld_boundary_uses_native_rounding(self) -> None:
        """Just below a half-cell welds to zero; a tie welds away from zero."""
        for sign in (1, -1):
            tie = np.float32(.00005) * sign
            below = np.nextafter(tie, np.float32(0), dtype=np.float32)
            for value, should_join in ((below, True), (tie, False)):
                vertices = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0],
                                     [value, 0, 0], [0, 0, 2], [0, 2, 0]], dtype=np.float32)
                centres = rig_lab.connected_centres(vertices, np.array([[0, 1, 2], [3, 4, 5]]))
                self.assertEqual(bool(np.array_equal(centres[0], centres[1])), should_join)

    def test_island_exclusion_uses_island_centre(self) -> None:
        vertices = np.array([[0., 0., 0.], [2., 0., 0.], [0., 2., 0.], [2., 2., 0.]])
        triangles = np.array([[0, 1, 2], [1, 2, 3]])
        centres = vertices[triangles].mean(axis=1)
        islands = rig_lab.connected_centres(vertices, triangles)
        spec = {"islands": [((0, 0, -1), (2, 2, 1))],
                "exclude": [((.9, .9, -1), (1.1, 1.1, 1))]}
        self.assertEqual(rig_lab.selected_triangles(spec, 0, {0, 1}, centres, islands), [])
        spec["exclude"] = [((0, 0, -1), (.8, .8, 1))]
        self.assertEqual(rig_lab.selected_triangles(spec, 0, {0, 1}, centres, islands), [0, 1])

    def test_mesh_is_a_restriction_not_an_alternative(self) -> None:
        centres = np.array([[0., 0., 0.], [2., 0., 0.]])
        spec = {"mesh": 1, "box": ((-1, -1, -1), (1, 1, 1))}
        self.assertEqual(rig_lab.selected_triangles(spec, 0, {0, 1}, centres, list(centres)), [])
        self.assertEqual(rig_lab.selected_triangles(spec, 1, {0, 1}, centres, list(centres)), [0])


class AuthoredContactTests(unittest.TestCase):
    @unittest.skipUnless(Path("out/rig-lab/rig.glb").exists(), "Needs locally extracted avatar rig")
    def test_rifle_bolt_reach_keeps_forearms_clear_and_wrist_feasible(self) -> None:
        """The old low handle grasp crossed forearms and forced a 64-degree twist."""
        animation.tactical_authoring.load()
        skeleton = animation.Skeleton(animation.read_rig(Path("out/rig-lab/rig.glb"))[0])
        _, clips = animation.bolt_rifle()
        clip = next(item for item in clips if item.name == "RifleReload")
        animation.prepare_choreography(clip)
        checked = 0
        for time, solved, _ in animation.evaluate(skeleton, clip):
            target = solved.view.compose(clip.left_frame(time, clip.gun_frame(time)))
            actual = solved.frames["WristLeft"]
            self.assertLess(animation.q_angle(animation.q_mul(actual.q, animation.q_inv(target.q))), 25.)
            if not (.34 <= time <= .59 or 1.8 <= time <= 2.03):
                continue
            # Dense samples of the solved forearm axes, not authored key values.
            # The 12 cm threshold includes ample room for both skinned sleeves.
            blend = np.linspace(0., 1., 101)[:, None]
            left_elbow = np.array(solved.frames["ElbowLeft"].p)
            right_elbow = np.array(solved.frames["ElbowRight"].p)
            left = left_elbow + (np.array(actual.p) - left_elbow) * blend
            right = right_elbow + (np.array(solved.frames["WristRight"].p) - right_elbow) * blend
            self.assertGreater(float(np.linalg.norm(left[:, None] - right[None, :], axis=2).min()), .12)
            checked += 1
        self.assertGreater(checked, 10)

    @unittest.skipUnless(Path("out/rig-lab/rig.glb").exists(), "Needs locally extracted avatar rig")
    def test_lmg_turns_palm_above_receiver_after_box_seating(self) -> None:
        """A palm-down pose below the box forced an 81-degree wrist correction."""
        skeleton = animation.Skeleton(animation.read_rig(Path("out/rig-lab/rig.glb"))[0])
        _, clips = animation.machine_gun()
        clip = next(item for item in clips if item.name == "LMGReload")
        animation.prepare_choreography(clip)
        checked = 0
        for time, solved, _ in animation.evaluate(skeleton, clip):
            if not 5.1 <= time <= 5.6:
                continue
            desired = solved.view.compose(clip.pose(time).left)
            actual = solved.frames["WristLeft"]
            self.assertLess(animation.q_angle(animation.q_mul(actual.q, animation.q_inv(desired.q))), 15.)
            self.assertLess(animation.v_len(animation.v_sub(
                actual.apply(animation.PALM["Left"]), desired.p)), .001)
            checked += 1
        self.assertGreater(checked, 12)

    def test_classic_charging_handle_is_original_geometry(self) -> None:
        """The Classic receiver stays intact; its new handle has a real asset."""
        spec = next(part for part in animation.PARTS["classic/ak"] if part["name"] == "bolt")
        self.assertNotIn("box", spec)
        self.assertNotIn("islands", spec)
        with patch.object(animation, "write_prop") as writer:
            animation.build_props(Path("unused"))
        path, pieces, _ = next(call.args for call in writer.call_args_list
                               if call.args[0].stem == "ak_charging_handle")
        self.assertEqual(spec["model"], "models/" + path.stem)
        vertices = np.array([point for mesh, _, _ in pieces for point in mesh[0]]) + spec["pivot"]
        self.assertLess(float(vertices[:, 1].min()), .127)  # Shank enters the receiver.
        self.assertGreater(float(vertices[:, 1].max()), .143)  # Knob protrudes for the grasp.
        self.assertLess(float(np.ptp(vertices[:, 2])), .012)

    @unittest.skipUnless(Path("out/rig-lab/rig.glb").exists(), "Needs locally extracted avatar rig")
    def test_lmg_hand_tracks_cover_without_wrist_inversion(self) -> None:
        """Position alone passed while the solver rotated the hand by 137 degrees."""
        animation.tactical_authoring.load()
        skeleton = animation.Skeleton(animation.read_rig(Path("out/rig-lab/rig.glb"))[0])
        _, clips = animation.machine_gun()
        clip = next(item for item in clips if item.name == "LMGReload")
        animation.prepare_choreography(clip)
        pivot = animation.part_pivot_at("lmg", "cover")
        rest_contact = clip.gun_frame(.95).inverse().compose(clip.left_frame(.95, clip.gun_frame(.95)))
        checked = 0
        for time, solved, parts in animation.evaluate(skeleton, clip):
            if not (.95 <= time <= 1.4 or 6.4 <= time <= 6.8):
                continue
            value = parts["cover"]
            lid = animation.part_motion(pivot, value.p, value.q)
            target = solved.gun.compose(lid).compose(rest_contact)
            actual = solved.frames["WristLeft"].compose(animation.Frame(animation.PALM["Left"]))
            self.assertLess(animation.v_len(animation.v_sub(actual.p, target.p)), .001)
            self.assertLess(animation.q_angle(animation.q_mul(actual.q, animation.q_inv(target.q))), 20.)
            checked += 1
        self.assertGreater(checked, 20)


@unittest.skipUnless((CONTENT / "modern/DerivedData/ak.omsm").exists(), "Needs locally imported weapons")
class InstalledWeaponGeometryTests(unittest.TestCase):
    def test_ak_magazines_have_no_open_wall_or_split_rear_edge(self) -> None:
        """Every cut boundary must be at the mouth, never down a magazine wall."""
        for model, count, mouth_min_z, mouth_max_z, front_limit in (
            ("modern/ak", 164, .037, .046, -.214),
            ("classic/ak", 142, .051, .055, -.168),
        ):
            with self.subTest(model=model):
                meshes = read_model(model)
                parts = select_parts(meshes, animation.PARTS[model])
                self.assertEqual(len(parts[0]["mag"]), count)
                self.assertTrue(np.array_equal(parts[0]["mag"], parts[0]["mag_new"]))
                mesh = meshes[0]
                edges: Counter = Counter()
                for triangle in mesh.positions[mesh.triangles[parts[0]["mag"]]]:
                    points = [tuple(np.round(vertex, 4)) for vertex in triangle]
                    for i in range(3):
                        edges[tuple(sorted((points[i], points[(i + 1) % 3])))] += 1
                self.assertTrue(all(number <= 2 for number in edges.values()))
                boundary = [edge for edge, number in edges.items() if number == 1]
                self.assertTrue(boundary)
                points = np.array(boundary).reshape(-1, 3)
                self.assertGreaterEqual(float(points[:, 2].min()), mouth_min_z)
                self.assertLessEqual(float(points[:, 2].max()), mouth_max_z)
                self.assertGreaterEqual(float(points[:, 0].min()), front_limit)
                # Exactly one closed mouth loop; no loose shards or isolated face holes.
                neighbours: dict[tuple, set] = {}
                for a, b in boundary:
                    neighbours.setdefault(a, set()).add(b)
                    neighbours.setdefault(b, set()).add(a)
                self.assertTrue(all(len(adjacent) == 2 for adjacent in neighbours.values()))
                visited, pending = set(), [next(iter(neighbours))]
                while pending:
                    at = pending.pop()
                    if at in visited:
                        continue
                    visited.add(at)
                    pending.extend(neighbours[at] - visited)
                self.assertEqual(visited, set(neighbours))

    def test_lmg_box_and_rear_sight_are_complete(self) -> None:
        meshes = read_model("lmg")
        parts = select_parts(meshes, animation.PARTS["lmg"])[0]
        components = meshes[0].components
        self.assertEqual(len(parts["box"]), 198)
        self.assertEqual(set(components[parts["box"]]), {23, 24})
        self.assertEqual(len(parts["cover"]), 506)
        self.assertEqual(set(components[parts["cover"]]), {2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 26})
        for name in ("box", "cover"):
            for component in set(components[parts[name]]):
                self.assertEqual(int(np.sum(components[parts[name]] == component)),
                                 int(np.sum(components == component)))
        self.assertTrue(set(np.flatnonzero(components == 19)).issubset(parts["body"]))

    def test_pistol_slide_leaves_hammer_and_lower_frame_intact(self) -> None:
        meshes = read_model("pistol")
        parts = select_parts(meshes, animation.PARTS["pistol"])
        self.assertEqual(sum(len(part["hammer"]) for part in parts), 60)
        self.assertEqual(sum(len(part["slide"]) for part in parts), 326)
        self.assertEqual(sum(len(part["mag"]) for part in parts), 96)
        self.assertEqual(set(meshes[0].components[parts[0]["hammer"]]), {6})
        self.assertNotIn(6, meshes[0].components[parts[0]["slide"]])
        for mesh, selection in zip(meshes, parts):
            positions = mesh.positions[mesh.triangles[selection["slide"]]]
            if len(positions):
                self.assertGreater(float(positions[:, :, 2].min()), .086)

    def test_mechanical_parts_do_not_take_fixed_sights(self) -> None:
        # Component identity was established from the installed geometry,
        # including an isolated 3D render of the knob, sight guards and pump.
        # Counts alone would happily accept a complete but unrelated island.
        cases = [
            ("modern/ak", "modern/ak", "bolt", {13}, {1, 3, 7}, 76),
            ("modern/smg", "smg", "knob", {5}, {6}, 43),
            ("classic/smg", "smg", "knob", {5}, {6}, 43),
            ("modern/rifle", "rifle", "bolt", {0, 7, 20}, {1, 8, 9, 10, 11, 12, 13, 19}, 222),
            ("classic/rifle", "rifle", "bolt", {0, 7, 20}, {1, 8, 9, 10, 11, 12, 13, 19}, 222),
            ("modern/shotgun", "modern/shotgun", "pump", {2}, {4, 5}, 92),
            ("classic/shotgun", "classic/shotgun", "pump", {4}, {1, 6}, 92),
        ]
        for model, key, name, moving, stationary, count in cases:
            with self.subTest(model=model):
                meshes = read_model(model)
                parts = select_parts(meshes, animation.PARTS[key])[0]
                components = meshes[0].components
                self.assertEqual(set(components[parts[name]]), moving)
                self.assertEqual(len(parts[name]), count)
                for component in moving:
                    self.assertTrue(set(np.flatnonzero(components == component)).issubset(parts[name]))
                for component in stationary:
                    self.assertTrue(set(np.flatnonzero(components == component)).issubset(parts["body"]))

    def test_laser_and_rocket_parts_keep_all_source_surfaces(self) -> None:
        for generation in ("modern", "classic"):
            for name, part, components, count in (
                ("space_ak", "cell", (3, 3), 54), ("space_rifle", "cell", (5, 4), 54),
                ("space_pistol", "cell", (2, 2), 54), ("space_smg", "cell", (1, 0), 54),
                ("space_shotgun", "pump", (1, 1), 218),
            ):
                with self.subTest(model=generation + "/" + name):
                    meshes = read_model(generation + "/" + name)
                    parts = select_parts(meshes, animation.PARTS[name])[0]
                    component = components[0 if generation == "modern" else 1]
                    original = np.flatnonzero(meshes[0].components == component)
                    self.assertTrue(np.array_equal(parts[part], original))
                    self.assertEqual(len(original), count)
        meshes = read_model("modern/rpg")
        parts = select_parts(meshes, animation.PARTS["modern/rpg"])
        self.assertEqual(len(parts[0]["rocket"]), 0)  # Launcher tube and both sights stay fixed.
        self.assertEqual(len(parts[1]["rocket"]), 292)
        self.assertEqual(set(meshes[1].components[parts[1]["rocket"]]), {0, 1, 2})
        self.assertEqual(parts[1]["rocket"].tolist(), list(range(len(meshes[1].triangles))))

    def test_preview_selects_same_installed_triangles(self) -> None:
        for model, specs in animation.PARTS.items():
            if model == "classic/rpg":
                continue
            with self.subTest(model=model):
                meshes = read_model(model)
                expected = select_parts(meshes, specs)
                for mesh_index, mesh in enumerate(meshes):
                    remaining = set(range(len(mesh.triangles)))
                    centres = mesh.positions[mesh.triangles].mean(axis=1)
                    islands = rig_lab.connected_centres(mesh.positions, mesh.triangles)
                    for spec in specs:
                        if "copy" in spec or "model" in spec:
                            continue
                        selection = rig_lab.selected_triangles(spec, mesh_index, remaining, centres, islands)
                        self.assertEqual(selection, expected[mesh_index][spec["name"]].tolist())
                        remaining.difference_update(selection)


if __name__ == "__main__":
    unittest.main()
