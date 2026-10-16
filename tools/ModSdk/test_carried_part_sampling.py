"""Reproduce the runtime's fractional-cue grip calibration on 30 Hz tracks."""
from pathlib import Path
import unittest

import make_tactical_animations as animation


class CarriedPartSamplingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rig = Path(__file__).resolve().parents[2] / "out/rig-lab/rig.glb"
        if not rig.exists():
            raise unittest.SkipTest("Run Rig Lab --prepare to export the installed rig")
        cls.skeleton = animation.Skeleton(animation.read_rig(rig)[0])

    def sample(self, frames, time: float, part: str):
        """Interpolate exported samples, as CharacterPose and part_motion do."""
        before = max((frame for frame in frames if frame[0] <= time + 1e-8), key=lambda frame: frame[0])
        after = min((frame for frame in frames if frame[0] >= time - 1e-8), key=lambda frame: frame[0])
        weight = (time - before[0]) / (after[0] - before[0]) if after[0] > before[0] else 0.0
        rotations = {bone: animation.q_slerp(before[1].rotations.get(bone, animation.IDENTITY),
                                              after[1].rotations.get(bone, animation.IDENTITY), weight)
                     for bone in self.skeleton.names}
        motion = animation.lerp_frame(before[2].get(part, animation.Frame()),
                                      after[2].get(part, animation.Frame()), weight)
        return self.skeleton.world(rotations), motion

    def test_fractional_pickup_cues_preserve_the_seating_socket(self):
        checked = 0
        fractional = 0
        old_parts = 0
        for builder in animation.FAMILY_BUILDERS:
            _, clips = builder()
            for clip in clips:
                if not clip.part_follow:
                    continue
                animation.prepare_choreography(clip)
                frames = animation.evaluate(self.skeleton, clip)
                for part, (_, _, end, pivot) in clip.part_follow.items():
                    if not (part.endswith("_new") or part in ("shell", "clip")):
                        cue = next((cue for cue in clip.cues if cue.action.get("attach") == part), None)
                        if cue is not None:
                            with self.subTest(clip=clip.name, old_part=part, pickup=cue.time):
                                _, motion = self.sample(frames, cue.time, part)
                                placement = animation.part_motion(pivot, motion.p, motion.q)
                                self.assertLess(animation.v_len(placement.p), 0.001)
                                self.assertLess(animation.q_angle(placement.q), 0.25)
                                old_parts += 1
                        continue
                    cue = next((cue for cue in clip.cues
                                if cue.action.get("show") == part and cue.action.get("bone")), None)
                    if cue is None:
                        continue
                    with self.subTest(clip=clip.name, part=part, pickup=cue.time):
                        at_pickup, motion = self.sample(frames, cue.time, part)
                        placement = at_pickup["PropRight"].compose(
                            animation.part_motion(pivot, motion.p, motion.q))
                        grip = at_pickup[cue.action["bone"]].inverse().compose(placement)
                        at_seat, _ = self.sample(frames, end, part)
                        carried = at_seat[cue.action["bone"]].compose(grip)
                        socket = at_seat["PropRight"]
                        self.assertLess(animation.v_len(animation.v_sub(carried.p, socket.p)), 0.001)
                        self.assertLess(animation.q_angle(animation.q_mul(
                            carried.q, animation.q_inv(socket.q))), 0.25)
                        checked += 1
                        fractional += abs(cue.time * 30 - round(cue.time * 30)) > 0.01
        self.assertGreater(checked, 20)
        self.assertGreater(fractional, 10)
        self.assertGreater(old_parts, 10)


if __name__ == "__main__":
    unittest.main()
