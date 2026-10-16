"""Pistol regressions found by inspecting the held weapon and empty reload."""
import json
from pathlib import Path
import unittest

import make_tactical_animations as animation


class PistolChoreographyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rig = Path(__file__).resolve().parents[2] / "out/rig-lab/rig.glb"
        if not rig.exists():
            raise unittest.SkipTest("Run Rig Lab --prepare to export the installed rig")
        cls.skeleton = animation.Skeleton(animation.read_rig(rig)[0])

    def test_wrists_follow_contacts_without_extreme_bends(self):
        """A good palm error alone must not hide a folded wrist or rotated hand."""
        # The SMG's underhand wrap is checked against both actual skins in
        # test_tactical_smg_skin.py. Exact virtual-palm orientation alone let
        # its former fingers obscure the sight line while this test passed.
        clips = [clip for builder in (animation.pistol, animation.laser_pistol)
                 for clip in builder()[1]]
        for clip in clips:
            animation.prepare_choreography(clip)
            for time, solved, _ in animation.evaluate(self.skeleton, clip):
                with self.subTest(clip=clip.name, time=time):
                    for side in ("Left", "Right"):
                        rotation = solved.rotations["Wrist" + side]
                        twist = animation.q_twist(rotation, animation.X)
                        swing = animation.q_mul(rotation, animation.q_inv(twist))
                        self.assertLess(animation.q_angle(swing), 50.0)
                    requested = solved.view.compose(clip.pose(time).left)
                    actual = solved.frames["WristLeft"].compose(
                        animation.Frame(animation.PALM["Left"], animation.IDENTITY))
                    correction = animation.q_mul(actual.q, animation.q_inv(requested.q))
                    self.assertLess(animation.q_angle(correction), 0.1)
                    self.assertLess(animation.v_len(animation.v_sub(actual.p, requested.p)), 0.0005)
                    self.assertLess(animation.v_len(animation.v_sub(
                        solved.frames["PropRight"].p, solved.gun.p)), 0.0005)

    def test_empty_slide_closes_after_magazine_seats(self):
        _, clips = animation.pistol()
        clip = next(clip for clip in clips if clip.name == "PistolReloadEmpty")
        animation.prepare_choreography(clip)
        seat = next(cue.time for cue in clip.cues if cue.action.get("detach") == "mag_new")
        release = next(cue.time for cue in clip.cues
                       if cue.action.get("sound") == "sounds/pistol_slide_forward")
        self.assertGreater(release, seat)
        self.assertGreater(clip.parts["slide"].value(seat).p[0], 0.025)
        self.assertEqual(clip.parts["slide"].value(clip.length).p, (0.0, 0.0, 0.0))

    def test_seating_push_uses_the_actual_palm_below_the_floorplate(self):
        """Both skins push the base with fingers crossing its far side.

        The rejected pose had a perfect virtual-palm contact but curled the
        actual fingers longitudinally up beside the grip. Check exported skin
        vertices against the installed magazine instead of that contact marker.
        """
        scene_path = Path(__file__).resolve().parents[2] / "out/rig-lab/scene/rig.json"
        if not scene_path.exists():
            self.skipTest("Export the native rig scene to check both hand skins")
        try:
            import numpy as np
            import rig_lab
            from test_tactical_smg_skin import NativeSkin
        except ImportError:
            self.skipTest("Install NumPy to check the native skin surfaces")
        skin = NativeSkin(json.loads(scene_path.read_text(encoding="utf-8")))
        self.assertLess(skin.parity_error(), 0.00001)
        magazine = np.concatenate([
            np.array(surface["position"])[np.unique(surface["indices"])]
            for surface in rig_lab.weapon("pistol")["surfaces"] if surface["part"] == "mag"
        ])
        bottom = float(magazine[:, 2].min())
        floorplate = magazine[magazine[:, 2] < bottom + 0.010]
        lower, upper = floorplate.min(axis=0), floorplate.max(axis=0)
        for clip in animation.pistol()[1]:
            if "Reload" not in clip.name:
                continue
            animation.prepare_choreography(clip)
            frames = animation.evaluate(self.skeleton, clip)
            seat = next(cue.time for cue in clip.cues if cue.action.get("detach") == "mag_new")
            before = max((frame for frame in frames if frame[0] <= seat + 1e-8), key=lambda f: f[0])
            after = min((frame for frame in frames if frame[0] >= seat - 1e-8), key=lambda f: f[0])
            weight = (seat - before[0]) / (after[0] - before[0]) if after[0] > before[0] else 0.0
            rotations = {name: animation.q_slerp(before[1].rotations[name],
                                                 after[1].rotations[name], weight)
                         for name in before[1].rotations}
            # Native placement follows PropRight, even if an unreachable
            # intended gun frame were to differ from the solved attachment.
            solved = animation.replace(before[1], rotations=rotations,
                                       gun=self.skeleton.world(rotations)["PropRight"])
            for kind, (vertices, dominant) in skin.vertices(solved).items():
                with self.subTest(clip=clip.name, skin=kind):
                    names = np.array([skin.names[index] for index in dominant])
                    palm = vertices[np.char.startswith(names, "Wrist")]
                    underneath = np.all((palm[:, :2] >= lower[:2]) &
                                        (palm[:, :2] <= upper[:2]), axis=1)
                    self.assertTrue(underneath.any(), "The palm misses the magazine base")
                    contact = palm[underneath]
                    self.assertLess(float(np.min(np.abs(contact[:, 2] - bottom))), 0.008)
                    self.assertLess(float(contact[:, 2].max()), bottom + 0.015,
                                    "The palm climbs beside the grip instead of pushing its base")
                    for finger in ("FingerIndex3", "FingerMiddle3", "FingerRing3"):
                        tips = vertices[np.char.startswith(names, finger)]
                        self.assertGreater(len(tips), 0)
                        self.assertGreater(float(tips[:, 1].mean()), upper[1] + 0.010,
                                           "Fingertips form a longitudinal hook beside the grip")
                        self.assertLess(float(tips[:, 2].max()), bottom + 0.060,
                                        "Fingertips rise into the firing hand")

    def test_rifle_cell_contact_does_not_hide_a_flipped_palm(self):
        """The cell prime must preserve orientation as well as its contact point."""
        for builder in (animation.laser_assault, animation.laser_rifle):
            for clip in builder()[1]:
                if "Reload" not in clip.name:
                    continue
                animation.prepare_choreography(clip)
                for time, solved, _ in animation.evaluate(self.skeleton, clip):
                    with self.subTest(clip=clip.name, time=time):
                        requested = solved.view.compose(clip.pose(time).left)
                        actual = solved.frames["WristLeft"].compose(
                            animation.Frame(animation.PALM["Left"]))
                        correction = animation.q_mul(actual.q, animation.q_inv(requested.q))
                        self.assertLess(animation.q_angle(correction), 0.1)
                        rotation = solved.rotations["WristLeft"]
                        swing = animation.q_mul(rotation, animation.q_inv(
                            animation.q_twist(rotation, animation.X)))
                        self.assertLess(animation.q_angle(swing), 50.0)

    def test_smg_hand_pulls_the_actual_charging_knob(self):
        _, clips = animation.submachine_gun()
        clip = next(clip for clip in clips if clip.name == "SMGReloadEmpty")
        animation.prepare_choreography(clip)
        time, motion, _ = max(clip.parts["knob"].keys, key=lambda key: key[1].p[0])
        pivot = animation.part_pivot("smg", "knob")
        requested = clip.left_frame(time, clip.gun_frame(time))
        knob = clip.gun_frame(time).apply(animation.v_add(pivot, motion.p))
        self.assertLess(animation.v_len(animation.v_sub(requested.p, knob)), 0.0001)


if __name__ == "__main__":
    unittest.main()
