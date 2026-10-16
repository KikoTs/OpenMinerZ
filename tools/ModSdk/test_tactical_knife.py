"""Knife grip and thrust checks using the installed mesh and solved avatar rig."""
from pathlib import Path
import unittest

import numpy as np

import make_tactical_animations as animation
from tactical_mesh_audit import CONTENT, read_model


@unittest.skipUnless(Path("out/rig-lab/rig.glb").exists()
                     and (CONTENT / "modern/DerivedData/knife.omsm").exists(),
                     "Needs the locally extracted avatar and knife")
class KnifeContactTests(unittest.TestCase):
    def test_hilt_crosses_palm_and_strike_follows_blade(self) -> None:
        animation.tactical_authoring.load()
        skeleton = animation.Skeleton(animation.read_rig(Path("out/rig-lab/rig.glb"))[0])
        family, clips = animation.knife()
        self.assertEqual({clip.name for clip in clips},
                         {"PickIdle@knife", "PickWalk@knife", "PickUse@knife"})
        clip = next(item for item in clips if item.name == "PickUse@knife")
        self.assertAlmostEqual(clip.length, .317)
        animation.prepare_choreography(clip)
        mesh = read_model("modern/knife")[0]
        # These three complete source islands are blade, handle and guard.
        handle = mesh.positions[mesh.triangles[mesh.components == 1]].reshape(-1, 3)
        guard = mesh.positions[mesh.triangles[mesh.components == 2]].reshape(-1, 3).mean(axis=0)
        blade = mesh.positions[mesh.triangles[mesh.components == 0]].reshape(-1, 3)
        tip = blade[np.argmax(blade[:, 2])]
        _, directions = np.linalg.eigh(np.cov(handle.T))
        shaft = directions[:, -1]
        if np.dot(shaft, guard - handle.mean(axis=0)) < 0:
            shaft = -shaft
        frames = animation.evaluate(skeleton, clip)
        for time, solved, _ in frames:
            wrist = solved.frames["WristRight"]
            prop = solved.frames["PropRight"]
            relative = wrist.inverse().compose(prop)
            # The shaft must run across the finger row toward the thumb,
            # rather than along the fingers and out through the knuckles.
            self.assertGreater(animation.q_rotate(relative.q, shaft)[2], .75)
            rotation = solved.rotations["WristRight"]
            swing = animation.q_mul(rotation, animation.q_inv(animation.q_twist(rotation, animation.X)))
            self.assertLess(animation.q_angle(swing), 60.)
        first = frames[0][1].frames["PropRight"]
        strike = min(frames, key=lambda row: abs(row[0] - .1))[1].frames["PropRight"]
        direction = animation.v_norm(animation.v_sub(first.apply(tip), first.apply(guard)))
        travel = animation.v_sub(strike.apply(guard), first.apply(guard))
        self.assertGreater(animation.v_len(travel), .14)
        self.assertGreater(animation.v_dot(direction, animation.v_norm(travel)), .98)
        last = frames[-1][1].frames["PropRight"]
        self.assertLess(animation.v_len(animation.v_sub(last.apply(guard), first.apply(guard))), .001)


if __name__ == "__main__":
    unittest.main()
