"""Check AK reload contacts and the free-hand transitions between them."""
from pathlib import Path
import unittest

import make_tactical_animations as animation


class AssaultReloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rig = Path(__file__).resolve().parents[2] / "out/rig-lab/rig.glb"
        if not rig.exists():
            raise unittest.SkipTest("Requires an installed rig export")
        cls.skeleton = animation.Skeleton(animation.read_rig(rig)[0])

    def test_reload_hand_orientation_and_both_wrists(self):
        for model in ("modern/ak", "classic/ak"):
            _, clips = animation.assault_rifle(model)
            for clip in clips[-2:]:
                animation.prepare_choreography(clip)
                contacts = [(start, end) for _, start, end, _ in clip.part_follow.values()]
                if "Empty" in clip.name:
                    seat = next(cue.time for cue in clip.cues if cue.action.get("detach") == "mag_new")
                    bolt = clip.parts["bolt"]
                    pull_start = min(time for time, _, _ in bolt.keys if time > seat)
                    pull_end = max(bolt.keys, key=lambda key: key[1].p[0])[0]
                    contacts.append((pull_start, pull_end))
                for time, solved, _ in animation.evaluate(self.skeleton, clip):
                    with self.subTest(clip=clip.name, time=time):
                        requested = solved.view.compose(clip.pose(time).left)
                        correction = animation.q_mul(solved.frames["WristLeft"].q,
                                                     animation.q_inv(requested.q))
                        # Contact orientation must survive the wrist solve.
                        # A released hand may relax its authored transit while
                        # the forearm shares the turn out of an underhand grip.
                        in_contact = any(start <= time <= end for start, end in contacts)
                        self.assertLess(animation.q_angle(correction), .1 if in_contact else 12.0)
                        self.assertLess(animation.v_len(animation.v_sub(
                            solved.frames["WristLeft"].apply(animation.PALM["Left"]), requested.p)), .003)
                        for side in ("Left", "Right"):
                            wrist = solved.rotations["Wrist" + side]
                            swing = animation.q_mul(wrist, animation.q_inv(animation.q_twist(wrist, animation.X)))
                            self.assertLess(animation.q_angle(swing), 55)

    def test_empty_charge_moves_hook_after_new_magazine_seats(self):
        _, clips = animation.assault_rifle()
        clip = clips[-1]
        seat = next(c.time for c in clip.cues if c.action.get("detach") == "mag_new")
        bolt = clip.parts["bolt"]
        self.assertEqual(bolt.value(seat).p, (0, 0, 0))
        self.assertGreater(max(value.p[0] for _, value, _ in bolt.keys), .07)
        self.assertEqual(bolt.value(clip.length).p, (0, 0, 0))


if __name__ == "__main__":
    unittest.main()
