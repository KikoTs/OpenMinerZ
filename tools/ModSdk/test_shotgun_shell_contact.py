"""Check shotgun shell contact against both installed hand meshes."""
import json
from pathlib import Path
import unittest

import numpy as np

import make_tactical_animations as animation
import rig_lab
import tactical_authoring
from test_tactical_smg_skin import NativeSkin, matrix


class ShotgunShellContactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        rig = root / "out/rig-lab/rig.glb"
        scene = root / "out/rig-lab/scene/rig.json"
        if not rig.exists() or not scene.exists():
            raise unittest.SkipTest("Requires the installed rig and native skin export")
        tactical_authoring.load()
        cls.skeleton = animation.Skeleton(animation.read_rig(rig)[0])
        cls.skin = NativeSkin(json.loads(scene.read_text(encoding="utf-8")))

    def test_shell_stays_between_thumb_and_index_without_forcing_the_wrist(self):
        self.assertLess(self.skin.parity_error(), .00001)
        for model in ("classic/shotgun", "modern/shotgun"):
            _, clips = animation.pump_shotgun(model)
            clip = next(clip for clip in clips if "Reload" in clip.name)
            self.assertEqual(clip.length, .583)
            animation.prepare_choreography(clip)
            seat = next(cue.time for cue in clip.cues if cue.action.get("detach") == "shell")
            self.assertAlmostEqual(seat, .583 * .60)
            spec = next(part for part in animation.PARTS[model] if part["name"] == "shell")
            shell = np.concatenate([
                np.array(surface["position"])[np.unique(surface["indices"])]
                for surface in rig_lab.weapon(model)["surfaces"] if surface["part"] == "shell"
            ])
            lower, upper = shell[:, 0].min(), shell[:, 0].max()
            radius = np.linalg.norm(shell[:, 1:], axis=1).max()
            xyz = spec.get("rotation", (0, 0, 0))
            mesh_rotation = animation.q_mul(animation.q_axis_angle(animation.Z, xyz[2]),
                animation.q_mul(animation.q_axis_angle(animation.Y, xyz[1]),
                                animation.q_axis_angle(animation.X, xyz[0])))
            previous = None
            frames = animation.evaluate(self.skeleton, clip)
            # A per-shell reload may repeat immediately for the next round.
            for bone in ("ShoulderLeft", "ElbowLeft", "WristLeft"):
                self.assertLess(animation.q_angle(animation.q_mul(frames[0][1].rotations[bone],
                    animation.q_inv(frames[-1][1].rotations[bone]))), 4.0)
            for time, solved, parts in frames:
                with self.subTest(model=model, time=time):
                    requested = solved.view.compose(clip.pose(time).left)
                    loss = animation.q_angle(animation.q_mul(solved.frames["WristLeft"].q,
                                                             animation.q_inv(requested.q)))
                    self.assertLess(loss, 7.0)
                    self.assertLess(animation.v_len(animation.v_sub(
                        solved.frames["WristLeft"].apply(animation.PALM["Left"]), requested.p)), .001)
                    wrist = solved.rotations["WristLeft"]
                    swing = animation.q_mul(wrist, animation.q_inv(animation.q_twist(wrist, animation.X)))
                    self.assertLess(animation.q_angle(swing), 55.1)
                    if previous is not None:
                        for bone in ("ShoulderLeft", "ElbowLeft", "WristLeft"):
                            self.assertLess(animation.q_angle(animation.q_mul(
                                solved.rotations[bone], animation.q_inv(previous.rotations[bone]))), 28.0)
                    previous = solved
                    if not .03 < time < seat:
                        continue
                    # Native own meshes first receive their declared rotation
                    # and pivot translation. Omitting this step can make a
                    # correctly carried shell appear beneath the wrist.
                    motion = parts["shell"]
                    shell_in_gun = animation.Frame(animation.v_add(spec["pivot"], motion.p),
                        animation.q_mul(motion.q, mesh_rotation))
                    inverse = np.linalg.inv(matrix(shell_in_gun.p, shell_in_gun.q))
                    for kind, (vertices, dominant) in self.skin.vertices(solved).items():
                        local = (np.c_[vertices, np.ones(len(vertices))] @ inverse.T)[:, :3]
                        for finger in ("FingerIndex", "FingerThumb"):
                            selected = np.array([self.skin.names[index].startswith(finger)
                                                 for index in dominant])
                            points = local[selected]
                            # Distance to the shell's measured finite cylinder.
                            # Both pinch surfaces must touch; neither should
                            # bury the shell through the centre of the finger.
                            axial = np.maximum(points[:, 0] - upper, lower - points[:, 0])
                            radial = np.linalg.norm(points[:, 1:], axis=1) - radius
                            distance = (np.linalg.norm(np.maximum(np.c_[axial, radial], 0), axis=1)
                                        + np.minimum(np.maximum(axial, radial), 0))
                            with self.subTest(skin=kind, finger=finger):
                                self.assertLess(float(distance.min()), .003)
                                self.assertGreater(float(distance.min()), -.0065)


if __name__ == "__main__":
    unittest.main()
