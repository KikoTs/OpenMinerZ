"""Linked support editing, transient calibration and profile validation contracts."""
from copy import deepcopy
from dataclasses import asdict
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import make_tactical_animations as animation
import tactical_authoring as authoring


class LinkedAuthoringTests(unittest.TestCase):
    def setUp(self):
        self.previous = deepcopy(authoring.ACTIVE or authoring.load())
        self.profile = authoring.defaults()
        authoring.configure(animation, self.profile)

    def tearDown(self):
        authoring.configure(animation, self.previous)

    def linked(self, model="modern/ak", **support):
        profile = deepcopy(self.profile)
        profile["models"][model] = {"support": support}
        authoring.configure(animation, profile)
        return profile

    def assertFrameNear(self, actual, expected):
        self.assertLess(animation.v_len(animation.v_sub(actual.p, expected.p)), 1e-9)
        self.assertLess(animation.q_angle(animation.q_mul(actual.q, animation.q_inv(expected.q))), 1e-5)

    def test_one_grip_edit_follows_idle_walk_aim_fire_and_reload_endpoints(self):
        delta = (.012, -.008, .005)
        turn = animation.q_axis_angle(animation.X, 8)
        self.linked(position=list(delta), rotation=list(turn))
        family, clips = animation.assault_rifle()
        original_support = family.gun.support
        expected = animation.Frame(animation.v_add(original_support.p, delta),
                                   animation.q_mul(turn, original_support.q))
        found = set()
        for clip in clips:
            old_keys = deepcopy(clip.left.keys)
            events = deepcopy(clip.cues)
            carried = deepcopy(clip.part_follow)
            parts = {name: deepcopy(track.keys) for name, track in clip.parts.items()}
            authoring.apply_clip(clip, animation)
            metadata = authoring.linked_support_keys(clip)
            self.assertTrue(metadata, clip.name)
            linked = {key["time"] for key in metadata}
            for time, hand, easing in clip.left.keys:
                if time not in linked:
                    self.assertIn((time, hand, easing), old_keys)
                    continue
                local = hand.frame if hand.space == "gun" else clip.gun_frame(time).inverse().compose(hand.frame)
                self.assertFrameNear(local, expected)
            self.assertEqual(clip.cues, events)
            self.assertEqual(clip.part_follow, carried)
            self.assertEqual({name: track.keys for name, track in clip.parts.items()}, parts)
            found.add(clip.name)
            if "Reload" in clip.name:
                self.assertTrue(authoring.support_is_active(clip, 0))
                self.assertTrue(authoring.support_is_active(clip, clip.length))
                self.assertFalse(authoring.support_is_active(clip, 1.1))
        self.assertEqual(family.gun.support, original_support)
        self.assertTrue({"GunIdle", "GunRun", "GunShoulder", "GunShoulderIdle", "GunShoot", "GunReload"} <= found)

    def test_exact_model_scope_does_not_change_classic_or_other_weapons(self):
        self.linked(position=[.02, 0, 0])
        for builder in (animation.classic_assault_rifle, animation.pistol, animation.submachine_gun):
            _, clips = builder()
            for clip in clips:
                keys = deepcopy(clip.left.keys)
                authoring.apply_clip(clip, animation)
                self.assertEqual(clip.left.keys, keys)

    def test_moving_shotgun_pump_keeps_the_adjusted_contact(self):
        delta = (0, -.008, -.006)
        self.linked("modern/shotgun", position=list(delta), grip={"curl": .6, "thumb": .2})
        family, clips = animation.pump_shotgun()
        clip = next(clip for clip in clips if clip.name == "PumpShotgunShoot")
        authoring.apply_clip(clip, animation)
        matches = authoring.linked_support_keys(clip)
        moving = [match for match in matches if match["part"] == "pump"]
        self.assertTrue(moving)
        spec = next(part for part in animation.PARTS[family.gun.model] if part["name"] == "pump")
        adjusted = animation.Frame(animation.v_add(family.gun.support.p, delta), family.gun.support.q)
        for match in moving:
            time = match["time"]
            motion = clip.parts["pump"].value(time)
            expected = animation.part_motion(spec["pivot"], motion.p, motion.q).compose(adjusted)
            hand = next(hand for key_time, hand, _ in clip.left.keys if key_time == time)
            self.assertFrameNear(hand.frame, expected)
        for _, grip, _ in clip.left_grip.keys:
            self.assertEqual(grip.curl, .6)
            self.assertEqual(grip.thumb, .2)

    def test_support_finger_edits_preserve_reload_magazine_and_bolt_grips(self):
        self.linked(grip={"curl": .55, "thumb": .13, "thumb_opposition": 8, "handle": 1, "spread": .2})
        _, clips = animation.assault_rifle()
        clip = clips[-1]
        original_track = deepcopy(clip.left_grip)
        before = deepcopy(clip.left_grip.keys)
        authoring.apply_clip(clip, animation)
        changed = 0
        for (time, actual, _), (_, old, _) in zip(clip.left_grip.keys, before):
            if authoring.support_is_active(clip, time):
                self.assertEqual(actual.curl, .55)
                changed += 1
            else:
                self.assertEqual(actual, old)
        self.assertGreater(changed, 0)
        self.assertEqual(clip.left_grip.value(1.0), original_track.value(1.0))

    def test_explicit_clip_keys_win_over_linked_defaults(self):
        profile = self.linked(position=[.02, 0, 0], rotation=list(animation.q_axis_angle(animation.Z, 12)))
        profile["clips"]["GunIdle"] = {"left": [{"time": 0, "space": "gun", "position": [-.2, .12, .08],
                                                 "rotation": [0, 0, 0, 1]}]}
        authoring.configure(animation, profile)
        _, clips = animation.assault_rifle()
        clip = clips[0]
        authoring.apply_clip(clip, animation)
        self.assertEqual(clip.left.keys[0][1].frame.p, (-.2, .12, .08))
        self.assertFalse(authoring.support_is_active(clip, 0))
        self.assertTrue(authoring.linked_support_keys(clip)[0]["explicit"])

    def test_linked_view_anchor_tracks_an_explicit_weapon_edit(self):
        profile = self.linked(position=[.01, 0, 0])
        profile["clips"]["GunReload"] = {"gun": [{"time": 0, "value": [.03, 0, 0, 0, 0, 0]}]}
        authoring.configure(animation, profile)
        family, clips = animation.assault_rifle()
        clip = clips[-2]
        authoring.apply_clip(clip, animation)
        hand = clip.left.keys[0][1]
        self.assertEqual(hand.space, "view")
        actual = clip.gun_frame(0).inverse().compose(hand.frame)
        self.assertFrameNear(actual, animation.Frame(animation.v_add(family.gun.support.p, (.01, 0, 0)),
                                                     family.gun.support.q))

    def test_reapplying_or_resetting_a_draft_does_not_accumulate_offsets(self):
        _, clips = animation.assault_rifle()
        clip = clips[-2]
        original = deepcopy(clip.left.keys)
        self.linked(position=[.02, .01, 0])
        authoring.apply_clip(clip, animation)
        adjusted = deepcopy(clip.left.keys)
        authoring.apply_clip(clip, animation)
        self.assertEqual(clip.left.keys, adjusted)
        authoring.configure(animation, self.profile)
        authoring.apply_clip(clip, animation)
        self.assertEqual(clip.left.keys, original)

    def test_retiming_authored_hand_and_weapon_keys_removes_old_locations(self):
        _, clips = animation.assault_rifle()
        clip = clips[-2]
        original = {channel: deepcopy(getattr(clip, channel)) for channel in ("left", "gun")}
        edits = {}
        for channel in ("left", "gun"):
            time, value, easing = original[channel].keys[2]
            destination = time + .013
            key = {"time": destination, "sourceTime": time, "easing": easing}
            if channel == "left":
                key.update(space=value.space, position=list(value.frame.p), rotation=list(value.frame.q), arc=list(value.arc))
            else:
                key["value"] = list(value)
            edits[channel] = [key]
        profile = deepcopy(self.profile)
        profile["clips"][clip.name] = edits
        authoring.configure(animation, profile)
        authoring.apply_clip(clip, animation)
        for channel in ("left", "gun"):
            key = edits[channel][0]
            track = getattr(clip, channel)
            self.assertFalse(any(abs(time - key["sourceTime"]) <= 1e-8 for time, _, _ in track.keys))
            self.assertTrue(any(abs(time - key["time"]) <= 1e-8 for time, _, _ in track.keys))
            self.assertEqual(len(track.keys), len(original[channel].keys))
            expected = deepcopy(original[channel])
            expected.keys = [item for item in expected.keys if item[0] != key["sourceTime"]]
            expected.key(key["time"], original[channel].keys[2][1], key["easing"])
            expected_clip = deepcopy(clip)
            setattr(expected_clip, channel, expected)
            for index in range(31):
                sample = clip.length * index / 30
                if channel == "left":
                    self.assertFrameNear(clip.left_frame(sample, clip.gun_frame(sample)),
                        expected_clip.left_frame(sample, expected_clip.gun_frame(sample)))
                else:
                    self.assertEqual(track.value(sample), expected.value(sample))
        # Reset restores the captured authored locations, not another override.
        authoring.configure(animation, self.profile)
        authoring.apply_clip(clip, animation)
        for channel in ("left", "gun"):
            self.assertEqual(getattr(clip, channel).keys, original[channel].keys)

    def test_multiple_retimes_remove_sources_before_inserting_any_destination(self):
        _, clips = animation.assault_rifle()
        clip = clips[-2]
        first, second = clip.gun.keys[1:3]
        profile = deepcopy(self.profile)
        profile["clips"][clip.name] = {"gun": [
            {"sourceTime": first[0], "time": second[0], "value": list(first[1])},
            {"sourceTime": second[0], "time": first[0], "value": list(second[1])}]}
        authoring.configure(animation, profile)
        authoring.apply_clip(clip, animation)
        self.assertEqual(clip.gun.value(second[0]), first[1])
        self.assertEqual(clip.gun.value(first[0]), second[1])

    def test_retiming_single_key_keeps_track_nonempty_and_updates_support_metadata(self):
        family, clips = animation.assault_rifle()
        clip = clips[0]
        support = family.gun.support
        profile = deepcopy(self.profile)
        profile["clips"][clip.name] = {"left": [{"sourceTime": 0, "time": .025, "space": "gun",
            "position": list(support.p), "rotation": list(support.q)}]}
        authoring.configure(animation, profile)
        authoring.apply_clip(clip, animation)
        self.assertEqual(len(clip.left.keys), 1)
        self.assertEqual(clip.left.keys[0][0], .025)
        self.assertEqual(clip.left.value(0), clip.left.value(clip.length))
        metadata = authoring.linked_support_keys(clip)
        self.assertEqual(metadata[0]["time"], .025)
        self.assertEqual(metadata[0]["sourceTime"], 0)
        self.assertTrue(metadata[0]["explicit"])

    def test_retime_source_must_be_within_the_actual_clip(self):
        for channel in ("left", "gun"):
            _, clips = animation.assault_rifle()
            clip = clips[0]
            profile = deepcopy(self.profile)
            key = {"sourceTime": clip.length + .001, "time": .025}
            if channel == "left":
                key.update(space="gun", position=[0, 0, 0], rotation=[0, 0, 0, 1])
            else:
                key["value"] = [0] * 6
            profile["clips"][clip.name] = {channel: [key]}
            authoring.configure(animation, profile)
            with self.assertRaises(ValueError):
                authoring.apply_clip(clip, animation)

    def test_attachment_edit_changes_hand_relationship_without_weapon_motion(self):
        family, clips = animation.knife()
        clip = clips[0]
        original = clip.pose(0)
        turn = animation.q_axis_angle(animation.X, 18)
        profile = deepcopy(self.profile)
        profile["models"]["knife"] = {"attachmentRotation": list(turn)}
        authoring.configure(animation, profile)
        authoring.apply_clip(clip, animation)
        self.assertEqual(clip.pose(0).gun, original.gun)
        self.assertEqual(clip.pose(0).prop_right, animation.q_mul(turn, original.prop_right))
        authoring.configure(animation, self.profile)
        authoring.apply_clip(clip, animation)
        self.assertEqual(clip.pose(0).prop_right, original.prop_right)

    def test_deleted_global_calibration_edits_reset_and_restore(self):
        clean = {"schema": 1}
        authoring.configure(animation, clean)
        palms, curls = deepcopy(animation.PALM), deepcopy(animation._CURL)
        edit = {"schema": 1, "palms": {"Left": [.1, -.03, .01]}, "finger_curl": {"Index": [20, 30, 40]}}
        authoring.configure(animation, edit)
        edit["palms"]["Left"][0] = -.1
        self.assertEqual(authoring.ACTIVE["palms"]["Left"][0], .1)
        authoring.configure(animation, clean)
        self.assertEqual(animation.PALM, palms)
        self.assertEqual(animation._CURL, curls)
        authoring.configure(animation, self.profile)
        self.assertEqual(animation.PALM["Left"], tuple(self.profile["palms"]["Left"]))

    def test_defaults_and_saved_profiles_are_independent(self):
        first = authoring.defaults()
        first["palms"]["Left"][0] = -.15
        self.assertEqual(authoring.defaults()["palms"]["Left"][0], .096)
        profile = authoring.defaults()
        profile["models"]["modern/ak"] = {"support": {"position": [.01, 0, 0]}}
        with TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            authoring.save(profile, set(profile["bones"]), path)
            self.assertEqual(json.loads(path.read_text()), profile)
            returned = authoring.load(path)
            returned["models"].clear()
            self.assertTrue(authoring.ACTIVE["models"])
        self.assertFalse(authoring.defaults()["models"])

    def test_empty_model_extensions_preserve_legacy_animation_inputs(self):
        # Compare the original explicit-key behavior with the linked authoring
        # path over every exported pose sample. Equal solver inputs preserve
        # the generated animations when the new model fields are unused.
        def legacy(clip, module):
            for key in authoring.ACTIVE.get("clips", {}).get(clip.name, {}).get("left", []):
                clip.left.key(key["time"], module.HandAt(module.Frame(tuple(key["position"]), tuple(key["rotation"])),
                              key["space"], tuple(key.get("arc", [0, 0, 0]))), key.get("easing", "smooth"))
        profile = deepcopy(self.profile)
        profile["clips"]["GunReload"] = {"left": [{"time": .1, "space": "view", "position": [-.1, -.1, -.3],
                                                   "rotation": [0, 0, 0, 1]}]}
        authoring.configure(animation, profile)
        checked = 0
        for builder in animation.FAMILY_BUILDERS:
            _, expected = builder()
            _, actual = builder()
            for old, new in zip(expected, actual):
                with patch.object(authoring, "apply_clip", legacy):
                    animation.prepare_choreography(old)
                animation.prepare_choreography(new)
                self.assertEqual(old.part_follow, new.part_follow)
                self.assertEqual(old.cues, new.cues)
                for index in range(animation.frame_count(old.length)):
                    time = min(old.length, index / 30)
                    self.assertEqual(asdict(old.pose(time)), asdict(new.pose(time)), (old.name, time))
                    checked += 1
        self.assertGreater(checked, 1000)


class ProfileValidationTests(unittest.TestCase):
    def assertInvalid(self, path, value):
        profile = authoring.defaults()
        target = profile
        for key in path[:-1]:
            target = target.setdefault(key, {})
        target[path[-1]] = value
        with self.assertRaises(ValueError, msg=f"{path}={value!r}"):
            authoring.validate(profile)

    def test_rejects_bad_nested_types_and_nonfinite_or_extreme_values(self):
        cases = [
            (("schema",), True), (("palms",), []), (("palms", "Left"), [True, 0, 0]),
            (("finger_curl", "Index"), "bad"), (("models",), []), (("models", "smg"), None),
            (("models", "smg", "support"), []),
            (("models", "smg", "support", "position"), [0, 0, float("nan")]),
            (("models", "smg", "support", "position"), [.31, 0, 0]),
            (("models", "smg", "support", "rotation"), [0, 0, 0, 0]),
            (("models", "smg", "support", "grip", "curl"), 1.1),
            (("models", "smg", "support", "grip", "thumb_opposition"), -46),
            (("models", "smg", "support", "grip", "spread"), "0.3"),
            (("models", "smg", "attachmentRotation"), [0, float("inf"), 0, 1]),
            (("clips", "GunIdle", "left"), [None]),
            (("clips", "GunIdle", "gun"), [{"time": 0, "value": [0] * 6, "easing": []}]),
            (("clips", "GunIdle", "gun"), [{"time": 0, "value": [10**1000, 0, 0, 0, 0, 0]}]),
            (("models", "smg", "surpport"), {}),
        ]
        for path, value in cases:
            self.assertInvalid(path, value)

    def test_duplicate_or_out_of_range_key_times_are_rejected(self):
        key = {"time": 0, "space": "gun", "position": [0, 0, 0], "rotation": [0, 0, 0, 1]}
        for keys in ([key, deepcopy(key)], [{**key, "time": -1}], [{**key, "time": float("nan")}],
                     [{**key, "time": True}], [{**key, "time": 121}]):
            self.assertInvalid(("clips", "GunIdle", "left"), keys)

    def test_retime_sources_reject_ambiguous_nonfinite_or_invalid_values(self):
        for channel in ("left", "gun"):
            key = ({"time": .3, "space": "gun", "position": [0, 0, 0], "rotation": [0, 0, 0, 1]}
                   if channel == "left" else {"time": .3, "value": [0] * 6})
            for source in (-1, 121, True, "0.1", float("nan"), float("inf"), None):
                self.assertInvalid(("clips", "GunReload", channel), [{**key, "sourceTime": source}])
            self.assertInvalid(("clips", "GunReload", channel),
                               [{**key, "sourceTime": .1}, {**key, "sourceTime": .1, "time": .4}])

    def test_validation_does_not_alias_the_callers_document(self):
        profile = authoring.defaults()
        validated = authoring.validate(profile)
        validated["palms"]["Left"][0] = 0
        self.assertEqual(profile["palms"]["Left"][0], .096)

    def test_accepted_rounding_error_is_normalized_before_solver_use(self):
        profile = authoring.defaults()
        profile["models"]["smg"] = {"support": {"rotation": [0, 0, 0, .9999]}}
        validated = authoring.validate(profile)
        self.assertEqual(validated["models"]["smg"]["support"]["rotation"], [0, 0, 0, 1])
        self.assertEqual(profile["models"]["smg"]["support"]["rotation"][-1], .9999)


if __name__ == "__main__":
    unittest.main()
