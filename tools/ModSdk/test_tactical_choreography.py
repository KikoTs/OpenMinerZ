"""Check authored motion against an export of the user's installed skeleton.

Run: py -3 tools/ModSdk/test_tactical_choreography.py path/to/rig.glb
No owned animation or skeleton data is embedded in this test.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import make_tactical_animations as animation


def audit(rig: Path) -> dict:
    """Validate contacts, joint limits, timing and continuous helper rotations."""
    skeleton = animation.Skeleton(animation.read_rig(rig)[0])
    failures: list[str] = []
    report: dict = {"clips": 0, "frames": 0, "families": {}}
    clip_names: set[str] = set()
    for builder in animation.FAMILY_BUILDERS:
        family, clips = builder()
        metrics = {"palm_error_m": 0.0, "gun_error_m": 0.0,
                   "wrist_bend_degrees": 0.0, "helper_step_degrees": 0.0}
        for clip in clips:
            animation.prepare_choreography(clip)
            if clip.name in clip_names:
                failures.append(f"duplicate clip: {clip.name}")
            clip_names.add(clip.name)
            report["clips"] += 1
            for name, track in vars(clip).items():
                if not isinstance(track, animation.Track):
                    continue
                stamps = [key[0] for key in track.keys]
                if any(t < 0 or t > clip.length + .001 for t in stamps):
                    failures.append(f"{clip.name}/{name}: a key lies outside the clip")
                if len(stamps) != len(set(stamps)):
                    failures.append(f"{clip.name}/{name}: duplicate time")
            if any(cue.time < 0 or cue.time > clip.length for cue in clip.cues):
                failures.append(f"{clip.name}: a cue lies outside the clip")
            previous = None
            for index in range(animation.frame_count(clip.length)):
                time = min(clip.length, index / animation.FRAME_RATE)
                pose = clip.pose(time)
                solved = animation.solve_pose(skeleton, pose, previous)
                report["frames"] += 1
                actual = solved.frames["WristLeft"].apply(animation.PALM["Left"])
                desired = solved.view.compose(pose.left).p
                palm_error = animation.v_len(animation.v_sub(actual, desired))
                gun_error = animation.v_len(animation.v_sub(solved.gun.p, solved.frames["PropRight"].p))
                wrist = solved.rotations["WristLeft"]
                swing = animation.q_mul(wrist, animation.q_inv(animation.q_twist(wrist, animation.X)))
                bend = animation.q_angle(swing)
                metrics["gun_error_m"] = max(metrics["gun_error_m"], gun_error)
                # Pick's one-arm mask intentionally ignores the support arm.
                if family.key != "Pick":
                    metrics["palm_error_m"] = max(metrics["palm_error_m"], palm_error)
                    metrics["wrist_bend_degrees"] = max(metrics["wrist_bend_degrees"], bend)
                for bone, rotation in solved.rotations.items():
                    if not all(math.isfinite(value) for value in rotation):
                        failures.append(f"{clip.name}/{bone}@{time}: nonfinite rotation")
                if previous is not None:
                    step = animation.q_angle(animation.q_mul(
                        solved.rotations["31"], animation.q_inv(previous.rotations["31"])))
                    metrics["helper_step_degrees"] = max(metrics["helper_step_degrees"], step)
                previous = solved
            # Single-round reloads repeat without jerking back to a pouch.
            if clip.name in ("PumpShotgunReload", "PumpShotgunReload@classic/shotgun", "LaserShotgunReload"):
                a, b = clip.pose(0.0), clip.pose(clip.length)
                if animation.v_len(animation.v_sub(a.left.p, b.left.p)) > .0001:
                    failures.append(f"{clip.name}: left palm loop seam")
            if "Reload" in clip.name and "Shoulder" not in clip.name:
                if "@rpg" in clip.name and len(clip.cues) < 5:
                    failures.append("launcher lost its dedicated loading sequence")
        report["families"][builder.__name__] = {key: round(value, 5) for key, value in metrics.items()}
        if metrics["palm_error_m"] > .01:
            failures.append(f"{builder.__name__}: left hand leaves contact by {metrics['palm_error_m']:.4f} m")
        if metrics["gun_error_m"] > .001:
            failures.append(f"{builder.__name__}: right hand cannot reach its weapon")
        if metrics["wrist_bend_degrees"] > 65.01:
            failures.append(f"{builder.__name__}: wrist exceeds its flexion limit")
        if metrics["helper_step_degrees"] > 60:
            failures.append(f"{builder.__name__}: forearm twist helper discontinuity")
    expected = {"PickIdle@knife", "PickWalk@knife", "PickUse@knife", "Grenade_Cook", "Grenade_Throw", "Grenade_Reset"}
    if not expected <= clip_names:
        failures.append(f"missing dedicated actions: {sorted(expected - clip_names)}")
    report["failures"] = failures
    return report


def main() -> None:
    """Print a machine-readable audit and fail on broken authored motion."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rig", type=Path)
    args = parser.parse_args()
    report = audit(args.rig)
    print(json.dumps(report, indent=2))
    raise SystemExit(bool(report["failures"]))


if __name__ == "__main__":
    main()
