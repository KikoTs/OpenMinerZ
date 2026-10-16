"""Fore-end cradle regressions on solved poses and both native skin surfaces."""
import json
from pathlib import Path
import unittest

import numpy as np

import make_tactical_animations as animation
from tactical_mesh_audit import CONTENT, read_model
from tactical_skin_audit import hand_points, surface_distances


BUILDERS = (animation.assault_rifle, animation.classic_assault_rifle,
            animation.bolt_rifle, animation.pump_shotgun, animation.classic_pump_shotgun,
            animation.machine_gun, animation.laser_assault, animation.laser_rifle,
            animation.laser_smg, animation.laser_shotgun)
RIG = Path("out/rig-lab/rig.glb")
SCENE = Path("out/rig-lab/scene/rig.json")


@unittest.skipUnless(RIG.exists() and SCENE.exists() and (CONTENT / "modern/DerivedData/rifle.omsm").exists(),
                     "Needs the locally extracted avatar, stock skin and weapons")
class SupportCradleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        animation.tactical_authoring.load()
        cls.skeleton = animation.Skeleton(animation.read_rig(RIG)[0])
        cls.scene = json.loads(SCENE.read_text())

    def test_fingers_cross_the_fore_end_and_contact_its_actual_surface(self) -> None:
        """The rejected pose aimed all four fingers along the barrel like a claw."""
        for build in BUILDERS:
            family, clips = build()
            clip = clips[0]
            animation.prepare_choreography(clip)
            solved = animation.evaluate(self.skeleton, clip)[0][1]
            inverse = solved.gun.inverse()
            row = animation.v_norm(animation.v_sub(inverse.apply(solved.frames["FingerSmallLeft"].p),
                                                    inverse.apply(solved.frames["FingerIndexLeft"].p)))
            # A longitudinal knuckle row puts the individual fingers across
            # the stock. Rifle's sloping stock needs a diagonal, not a square grasp.
            with self.subTest(model=family.gun.model):
                self.assertGreater(abs(row[0]), .65)
            triangles = np.concatenate([mesh.positions[mesh.triangles] for mesh in read_model(family.gun.model)])
            for kind in ("stock", "avatar"):
                with self.subTest(model=family.gun.model, skin=kind):
                    gaps = [float(surface_distances(hand_points(self.scene, solved, kind, finger), triangles).min())
                            for finger in ("Index", "Middle", "Ring", "Small")]
                    # Three actual distal pads must touch within 1 cm. The
                    # shorter little finger may rest below a tapered fore-end.
                    self.assertGreaterEqual(sum(gap < .01 for gap in gaps), 3, gaps)

    def test_aim_raise_is_reachable_and_skin_stays_below_sights(self) -> None:
        for build in BUILDERS:
            family, clips = build()
            clip = next(item for item in clips if item.name.split("@")[0].endswith("Shoulder"))
            animation.prepare_choreography(clip)
            frames = animation.evaluate(self.skeleton, clip)
            with self.subTest(model=family.gun.model):
                for time, solved, _ in frames:
                    desired = solved.view.compose(clip.left_frame(time, clip.gun_frame(time)))
                    error = animation.q_angle(animation.q_mul(solved.frames["WristLeft"].q,
                                                                animation.q_inv(desired.q)))
                    self.assertLess(error, 15.)
                    self.assertLess(animation.v_len(animation.v_sub(
                        solved.frames["WristLeft"].apply(animation.PALM["Left"]), desired.p)), .005)
                last, prior = frames[-1][1], frames[-2][1]
                self.assertLess(animation.q_angle(animation.q_mul(last.rotations["WristLeft"],
                                                                   animation.q_inv(prior.rotations["WristLeft"]))), 12.)
                self.assertLess(animation.v_len(animation.v_sub(last.frames["WristLeft"].p,
                                                                  prior.frames["WristLeft"].p)), .01)
            forward = np.array(animation.v_norm(animation.v_sub(family.gun.sight_ahead, family.gun.sight_eye)))
            up = np.array(family.gun.sight_up)
            up -= forward * np.dot(up, forward)
            up /= np.linalg.norm(up)
            for kind in ("stock", "avatar"):
                with self.subTest(model=family.gun.model, skin=kind):
                    vertices = hand_points(self.scene, frames[-1][1], kind)
                    # Check every skin vertex, including the thumb, against
                    # the calibrated sight plane, not the authored palm point.
                    height = (vertices - family.gun.sight_eye) @ up
                    self.assertLess(float(height.max()), -.005)

    def test_lmg_release_turns_palm_after_clearing_receiver(self) -> None:
        _, clips = animation.machine_gun()
        clip = next(item for item in clips if item.name == "LMGReload")
        animation.prepare_choreography(clip)
        for time, solved, _ in animation.evaluate(self.skeleton, clip):
            if time < 6.8:
                continue
            desired = solved.view.compose(clip.left_frame(time, clip.gun_frame(time)))
            self.assertLess(animation.q_angle(animation.q_mul(solved.frames["WristLeft"].q,
                                                                animation.q_inv(desired.q))), 20.)

    def test_both_wrists_and_shoulders_remain_anatomical_through_held_motion(self) -> None:
        """A reachable palm alone can hide a bent wrist or a crossed shoulder."""
        for build in BUILDERS:
            family, clips = build()
            for clip in clips:
                if "Reload" in clip.name:
                    continue
                animation.prepare_choreography(clip)
                previous_right = None
                for time, solved, _ in animation.evaluate(self.skeleton, clip):
                    with self.subTest(model=family.gun.model, clip=clip.name, time=time):
                        for side in ("Left", "Right"):
                            rotation = solved.rotations["Wrist" + side]
                            swing = animation.q_mul(rotation, animation.q_inv(
                                animation.q_twist(rotation, animation.X)))
                            self.assertLess(animation.q_angle(swing), 60.)
                        # Do not fix the wrist by swivelling the collar so far
                        # inward that its shoulder crosses the torso midline.
                        torso = solved.frames["BackUpper"].inverse()
                        self.assertLess(torso.apply(solved.frames["ShoulderRight"].p)[0], -.025)
                        self.assertGreater(torso.apply(solved.frames["ShoulderLeft"].p)[0], .025)
                        self.assertLess(animation.v_len(animation.v_sub(
                            solved.frames["PropRight"].p, solved.gun.p)), .001)
                        right = solved.rotations["WristRight"]
                        if previous_right is not None:
                            self.assertLess(animation.q_angle(animation.q_mul(
                                right, animation.q_inv(previous_right))), 30.)
                        previous_right = right


if __name__ == "__main__":
    unittest.main()
