"""Check held weapon grips and aim transitions on an installed rig export.

Run: py -3 tools/ModSdk/test_tactical_grips.py out/rig-lab/rig.glb
The source rig remains local; this test contains no owned skin or animation data.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import make_tactical_animations as animation
import tactical_authoring


def bend(rotation: tuple) -> float:
    """Angle between the hand's long axis and its forearm, excluding pronation."""
    twist = animation.q_twist(rotation, animation.X)
    return animation.q_angle(animation.q_mul(rotation, animation.q_inv(twist)))


def audit(rig: Path) -> dict:
    """Reject claws below a handguard, strained wrists and aim endpoint pops."""
    tactical_authoring.load()
    skeleton = animation.Skeleton(animation.read_rig(rig)[0])
    failures = []
    report = {}
    for model in ("modern/ak", "classic/ak"):
        family, clips = animation.assault_rifle(model)
        metrics = {"left_bend_degrees": 0.0, "right_bend_degrees": 0.0,
                   "palm_up_alignment": 1.0, "fingers_across_alignment": 1.0,
                   "contact_error_m": 0.0,
                   "raise_step_degrees": 0.0, "aim_seam_m": 0.0,
                   "aim_seam_degrees": 0.0, "reload_orientation_loss_degrees": 0.0,
                   "reload_contact_error_m": 0.0, "reload_right_bend_degrees": 0.0}
        for clip in clips:
            if "Reload" in clip.name:
                animation.prepare_choreography(clip)
                for time, solved, _ in animation.evaluate(skeleton, clip):
                    target = solved.view.compose(clip.pose(time).left)
                    hand = solved.frames["WristLeft"]
                    angle = animation.q_angle(animation.q_mul(hand.q, animation.q_inv(target.q)))
                    error = animation.v_len(animation.v_sub(hand.apply(animation.PALM["Left"]), target.p))
                    metrics["reload_orientation_loss_degrees"] = max(metrics["reload_orientation_loss_degrees"], angle)
                    metrics["reload_contact_error_m"] = max(metrics["reload_contact_error_m"], error)
                    metrics["reload_right_bend_degrees"] = max(metrics["reload_right_bend_degrees"],
                                                               bend(solved.rotations["WristRight"]))
                continue
            animation.prepare_choreography(clip)
            frames = animation.evaluate(skeleton, clip)
            previous = None
            for _, solved, _ in frames:
                local_hand = solved.gun.inverse().compose(solved.frames["WristLeft"])
                normal = local_hand.direction((0.0, -1.0, 0.0))
                metrics["palm_up_alignment"] = min(metrics["palm_up_alignment"], normal[2])
                fingers = local_hand.direction((1.0, 0.0, 0.0))
                metrics["fingers_across_alignment"] = min(metrics["fingers_across_alignment"], fingers[1])
                error = animation.v_len(animation.v_sub(
                    local_hand.apply(animation.PALM["Left"]), family.gun.support.p))
                metrics["contact_error_m"] = max(metrics["contact_error_m"], error)
                for side in ("Left", "Right"):
                    key = side.lower() + "_bend_degrees"
                    metrics[key] = max(metrics[key], bend(solved.rotations["Wrist" + side]))
                if clip.match_end_pose and previous is not None:
                    for bone in ("ShoulderLeft", "ElbowLeft", "WristLeft",
                                 "ShoulderRight", "ElbowRight", "WristRight"):
                        angle = animation.q_angle(animation.q_mul(
                            solved.rotations[bone], animation.q_inv(previous.rotations[bone])))
                        metrics["raise_step_degrees"] = max(metrics["raise_step_degrees"], angle)
                previous = solved
            if clip.match_end_pose:
                # Compare the baked endpoint, including its previous-frame IK
                # state, to the independently solved next animation.
                idle = family.clip("reference", 0.1, aimed_base=True)
                reference = animation.solve_pose(skeleton, idle.pose(0.0))
                end = frames[-1][1]
                for bone in ("Head", "CollarLeft", "ShoulderLeft", "ElbowLeft", "WristLeft",
                             "CollarRight", "ShoulderRight", "ElbowRight", "WristRight"):
                    distance = animation.v_len(animation.v_sub(end.frames[bone].p, reference.frames[bone].p))
                    angle = animation.q_angle(animation.q_mul(
                        end.rotations[bone], animation.q_inv(reference.rotations[bone])))
                    metrics["aim_seam_m"] = max(metrics["aim_seam_m"], distance)
                    metrics["aim_seam_degrees"] = max(metrics["aim_seam_degrees"], angle)
        # A longitudinal side-facing fist was the rejected claw. The palm
        # must cradle the underside while the fingers cross the fore-end.
        # Actual stock/avatar surface and sight clearance are checked in
        # test_tactical_support, rather than inferred from this virtual point.
        if metrics["palm_up_alignment"] < 0.80 or metrics["fingers_across_alignment"] < 0.55:
            failures.append(f"{model}: support no longer cradles across the fore-end")
        if metrics["contact_error_m"] > 0.003:
            failures.append(f"{model}: support grip moves off the handguard")
        if max(metrics["left_bend_degrees"], metrics["right_bend_degrees"]) > 55.1:
            failures.append(f"{model}: held wrist exceeds the 55-degree anatomical limit")
        if metrics["raise_step_degrees"] > 38.0:
            failures.append(f"{model}: elbow/wrist flips during the raise")
        if metrics["aim_seam_m"] > 0.0001 or metrics["aim_seam_degrees"] > 0.1:
            failures.append(f"{model}: raised arms do not match the aimed idle")
        # The separate contact test requires <0.1 degrees while actually
        # holding a magazine or bolt; the released hand may relax in transit.
        if metrics["reload_orientation_loss_degrees"] > 12.0:
            failures.append(f"{model}: reload wrist solve excessively changes the hand path")
        if metrics["reload_contact_error_m"] > 0.003:
            failures.append(f"{model}: reload hand cannot reach its contact")
        if metrics["reload_right_bend_degrees"] > 55.0:
            failures.append(f"{model}: reload overbends the right wrist")
        report[model] = {key: round(value, 6) for key, value in metrics.items()}

    # A handle needs straight proximal phalanges and deeply curled distal ones.
    # The former generic fist left a visible void between the palm and grip.
    handle = animation.hand_rotations(skeleton, "Right", animation.Grip(0.85), handle=True)
    for finger in ("Middle", "Ring", "Small"):
        base = animation.q_angle(handle[f"Finger{finger}Right"])
        middle = animation.q_angle(handle[f"Finger{finger}2Right"])
        tip = animation.q_angle(handle[f"Finger{finger}3Right"])
        if not (base < 25.0 and middle > 85.0 and tip > 60.0):
            failures.append(f"right {finger}: handle curl bends at the wrong joints")
    # Verify the fitted index against the pistol's trigger position, including
    # the user's 1.1x avatar hand size, rather than merely checking joint angles.
    trigger = (-0.0606, 0.1112, 0.0495)
    trigger_error, thumb_height = 0.0, float("inf")
    _, pistol_clips = animation.pistol()
    for clip in pistol_clips:
        animation.prepare_choreography(clip)
        for _, solved, _ in animation.evaluate(skeleton, clip):
            local_wrist = solved.gun.inverse().compose(solved.frames["WristRight"])
            for scale in (1.0, 1.1):
                finger = local_wrist
                for bone in ("FingerIndexRight", "FingerIndex2Right", "FingerIndex3Right"):
                    finger = finger.compose(animation.Frame(
                        animation.v_scale(skeleton.offset(bone), scale), solved.rotations[bone]))
                pad = finger.apply((-0.018 * scale, 0.0, 0.0))
                trigger_error = max(trigger_error, animation.v_len(animation.v_sub(pad, trigger)))
            thumb = solved.gun.inverse().apply(solved.frames["FingerThumb3Right"].p)
            thumb_height = min(thumb_height, thumb[2])
    if trigger_error > 0.020:
        failures.append("pistol: index finger pad extends away from the trigger guard")
    if thumb_height < 0.045:
        failures.append("pistol: thumb hooks underneath the slide instead of resting alongside it")
    report["pistol_fingers"] = {"trigger_pad_distance_m": round(trigger_error, 6),
                               "minimum_thumb_height_m": round(thumb_height, 6)}
    return {"grips": report, "failures": failures}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rig", type=Path)
    result = audit(parser.parse_args().rig)
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result["failures"]))


if __name__ == "__main__":
    main()
