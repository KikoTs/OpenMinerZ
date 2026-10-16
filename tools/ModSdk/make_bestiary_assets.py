"""Draws the art of the Bestiary example (examples/mods/bestiary) from code.

mod-ai: the creatures, projectiles and boss music of the creature AI example,
made the way make_example_assets.py makes the other examples' art (and with its
helpers): box figures on a small node skeleton with idle/walk/run/attack/hurt
clips, and a synthesized music loop. Nothing comes from CastleMiner Z; the
example's ghouls wear the player's own retail zombie only when the game finds
it installed, and these models otherwise. Deterministic: running it again
rewrites the same bytes.

    py -3 tools/ModSdk/make_bestiary_assets.py [--root examples/mods]

Needs Python 3.10+ and Pillow.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_example_assets import (  # noqa: E402  (the shared helpers)
    Box,
    biped,
    music_loop,
    quadruped,
    quat_x,
    quat_y,
    swing,
    write_model,
    write_wav,
)


def quat_z(degrees):
    r = math.radians(degrees) / 2
    return (0.0, 0.0, math.sin(r), math.cos(r))


def bone_archer(path: Path) -> None:
    """A gaunt archer facing +Z with a bow in its left hand."""
    height = 1.85
    leg_h = height * 0.45
    body_h = height * 0.33
    head = height * 0.19
    bone, dark, bow = (214, 206, 186), (120, 112, 100), (96, 66, 40)
    boxes = [
        Box("body", None, (0, leg_h, 0), (0, leg_h + body_h / 2, 0), (0.42, body_h, 0.2), bone),
        Box("ribs", "body", (0, leg_h, 0), (0, leg_h + body_h * 0.62, 0.02), (0.46, body_h * 0.36, 0.22), dark),
        Box("head", "body", (0, leg_h + body_h, 0), (0, leg_h + body_h + head / 2, 0.02), (head, head, head), bone),
        Box("eyes", "head", (0, leg_h + body_h, 0), (0, leg_h + body_h + head * 0.55, head * 0.52),
            (head * 0.7, head * 0.16, 0.02), (40, 30, 30)),
        Box("arm_l", "body", (-0.29, leg_h + body_h * 0.92, 0), (-0.29, leg_h + body_h * 0.45, 0),
            (0.11, body_h * 0.95, 0.11), bone),
        Box("bow", "arm_l", (-0.29, leg_h + body_h * 0.92, 0), (-0.29, leg_h + 0.02, 0.06), (0.05, 0.9, 0.06), bow),
        Box("arm_r", "body", (0.29, leg_h + body_h * 0.92, 0), (0.29, leg_h + body_h * 0.45, 0),
            (0.11, body_h * 0.95, 0.11), bone),
        Box("leg_l", "body", (-0.11, leg_h, 0), (-0.11, leg_h / 2, 0), (0.13, leg_h, 0.13), bone),
        Box("leg_r", "body", (0.11, leg_h, 0), (0.11, leg_h / 2, 0), (0.13, leg_h, 0.13), bone),
    ]
    walk = {"leg_l": swing(30, 1.0), "leg_r": swing(30, 1.0, 0.5), "arm_r": swing(25, 1.0)}
    run = {"leg_l": swing(48, 0.6), "leg_r": swing(48, 0.6, 0.5), "arm_r": swing(40, 0.6)}
    idle = {"head": swing(6, 3.0, 0.0, quat_y, 6), "arm_r": swing(3, 3.0)}
    # Draw: the bow arm comes up level, the string hand back to the cheek,
    # loose at 0.5 s (the shot's windup), then down.
    attack = {
        "arm_l": [(0.0, quat_x(0)), (0.3, quat_x(-88)), (0.55, quat_x(-88)), (0.9, quat_x(0))],
        "arm_r": [(0.0, quat_x(0)), (0.3, quat_x(-80)), (0.48, quat_x(-60)), (0.55, quat_x(-95)),
                  (0.9, quat_x(0))],
    }
    hurt = {"body": [(0.0, quat_x(0)), (0.1, quat_x(-14)), (0.3, quat_x(0))]}
    write_model(path, boxes, {"idle": (3.0, idle), "walk": (1.0, walk), "run": (0.6, run),
                              "attack": (0.9, attack), "hurt": (0.3, hurt)})


def arrow(path: Path) -> None:
    """A bone arrow along +Z, centred on its origin: thick, pale and red-fletched
    so it reads in flight at a distance."""
    boxes = [
        Box("shaft", None, (0, 0, 0), (0, 0, 0), (0.08, 0.08, 0.8), (232, 224, 202)),
        Box("head", "shaft", (0, 0, 0), (0, 0, 0.44), (0.16, 0.16, 0.16), (64, 64, 72)),
        Box("fletch_a", "shaft", (0, 0, 0), (0, 0, -0.32), (0.3, 0.025, 0.22), (214, 44, 40)),
        Box("fletch_b", "shaft", (0, 0, 0), (0, 0, -0.32), (0.025, 0.3, 0.22), (214, 44, 40)),
    ]
    write_model(path, boxes, {})


def hex_bolt(path: Path) -> None:
    """The Hollow King's hex: a spinning violet crystal, centred."""
    boxes = [
        Box("core", None, (0, 0, 0), (0, 0, 0), (0.32, 0.32, 0.32), (150, 60, 220)),
        Box("halo", "core", (0, 0, 0), (0, 0, 0), (0.5, 0.08, 0.5), (220, 160, 255)),
    ]
    spin = {"core": [(0.0, quat_y(0)), (0.5, quat_y(180)), (1.0, quat_y(360))]}
    write_model(path, boxes, {"idle": (1.0, spin)})


def gloom_bat(path: Path) -> None:
    """A bat facing +Z: body, head, two wings that flap."""
    fur, wing, eye = (58, 46, 60), (40, 32, 46), (230, 90, 60)
    boxes = [
        Box("body", None, (0, 0.25, 0), (0, 0.25, 0), (0.22, 0.2, 0.34), fur),
        Box("head", "body", (0, 0.3, 0.16), (0, 0.32, 0.24), (0.18, 0.16, 0.14), fur),
        Box("eyes", "head", (0, 0.3, 0.16), (0, 0.35, 0.315), (0.12, 0.03, 0.01), eye),
        Box("ear_l", "head", (0, 0.3, 0.16), (-0.06, 0.43, 0.22), (0.04, 0.08, 0.03), fur),
        Box("ear_r", "head", (0, 0.3, 0.16), (0.06, 0.43, 0.22), (0.04, 0.08, 0.03), fur),
        Box("wing_l", "body", (-0.11, 0.28, 0), (-0.4, 0.28, 0), (0.58, 0.02, 0.3), wing),
        Box("wing_r", "body", (0.11, 0.28, 0), (0.4, 0.28, 0), (0.58, 0.02, 0.3), wing),
    ]

    def flap(period, degrees):
        return {"wing_l": swing(degrees, period, 0.0, quat_z), "wing_r": swing(-degrees, period, 0.0, quat_z)}

    attack = flap(0.25, 45)
    attack["head"] = [(0.0, quat_x(0)), (0.15, quat_x(30)), (0.35, quat_x(0))]
    write_model(path, boxes, {"idle": (0.6, flap(0.6, 30)), "walk": (0.35, flap(0.35, 40)),
                              "run": (0.22, flap(0.22, 50)), "attack": (0.35, attack),
                              "hurt": (0.3, {"body": [(0.0, quat_x(0)), (0.1, quat_x(-25)), (0.3, quat_x(0))]})})


def grave_leaper(path: Path) -> None:
    """A crouched, long-limbed leaper facing +Z: thick legs, knuckles near the
    ground, a forward lean; it crouches and springs when it runs."""
    leg_h = 0.55
    body_h = 0.5
    head = 0.26
    skin, rags, eye = (170, 162, 140), (72, 64, 58), (226, 70, 44)
    boxes = [
        Box("body", None, (0, leg_h, 0), (0, leg_h + body_h / 2, 0), (0.46, body_h, 0.3), rags),
        Box("head", "body", (0, leg_h + body_h, 0), (0, leg_h + body_h + head / 2, 0.06), (head, head, head), skin),
        Box("eyes", "head", (0, leg_h + body_h, 0), (0, leg_h + body_h + head * 0.58, 0.06 + head * 0.52),
            (head * 0.72, head * 0.14, 0.02), eye),
        Box("arm_l", "body", (-0.3, leg_h + body_h * 0.9, 0), (-0.3, leg_h + body_h * 0.9 - 0.42, 0.04),
            (0.12, 0.84, 0.12), skin),
        Box("arm_r", "body", (0.3, leg_h + body_h * 0.9, 0), (0.3, leg_h + body_h * 0.9 - 0.42, 0.04),
            (0.12, 0.84, 0.12), skin),
        Box("leg_l", "body", (-0.14, leg_h, 0), (-0.14, leg_h / 2, -0.04), (0.2, leg_h, 0.26), rags),
        Box("leg_r", "body", (0.14, leg_h, 0), (0.14, leg_h / 2, -0.04), (0.2, leg_h, 0.26), rags),
    ]
    lean = [(0.0, quat_x(30)), (1.0, quat_x(30))]
    idle = {"body": [(0.0, quat_x(30)), (1.0, quat_x(34)), (2.0, quat_x(30))], "head": swing(8, 2.0, 0.0, quat_y, 6),
            "arm_l": swing(4, 2.0), "arm_r": swing(4, 2.0, 0.5)}
    walk = {"leg_l": swing(28, 0.9), "leg_r": swing(28, 0.9, 0.5), "arm_l": swing(30, 0.9, 0.5),
            "arm_r": swing(30, 0.9), "body": lean}
    # Running is bounding: both legs together, the body pitching with them.
    run = {"leg_l": swing(50, 0.5), "leg_r": swing(50, 0.5), "arm_l": swing(55, 0.5, 0.5), "arm_r": swing(55, 0.5, 0.5),
           "body": [(0.0, quat_x(26)), (0.25, quat_x(44)), (0.5, quat_x(26))]}
    # Both claws up and down on the prey at 0.3 s (the bite's windup).
    attack = {"arm_l": [(0.0, quat_x(0)), (0.2, quat_x(-130)), (0.32, quat_x(15)), (0.6, quat_x(0))],
              "arm_r": [(0.0, quat_x(0)), (0.2, quat_x(-130)), (0.32, quat_x(15)), (0.6, quat_x(0))],
              "body": lean}
    hurt = {"body": [(0.0, quat_x(30)), (0.1, quat_x(14)), (0.3, quat_x(30))]}
    write_model(path, boxes, {"idle": (2.0, idle), "walk": (0.9, walk), "run": (0.5, run),
                              "attack": (0.6, attack), "hurt": (0.3, hurt)})


def hollow_king(path: Path) -> None:
    """The boss: a tall crowned figure with long arms; attack and slam clips."""
    height = 2.8
    leg_h = height * 0.42
    body_h = height * 0.34
    head = height * 0.16
    robe, skin, crown, eye = (52, 40, 66), (176, 170, 150), (210, 170, 60), (200, 240, 255)
    boxes = [
        Box("body", None, (0, leg_h, 0), (0, leg_h + body_h / 2, 0), (0.9, body_h, 0.5), robe),
        Box("mantle", "body", (0, leg_h + body_h, 0), (0, leg_h + body_h * 0.9, 0), (1.1, 0.18, 0.62), (70, 54, 92)),
        Box("head", "body", (0, leg_h + body_h, 0), (0, leg_h + body_h + head / 2, 0.02), (head, head, head), skin),
        Box("eyes", "head", (0, leg_h + body_h, 0), (0, leg_h + body_h + head * 0.55, head * 0.52),
            (head * 0.66, head * 0.12, 0.02), eye),
        Box("crown", "head", (0, leg_h + body_h, 0), (0, leg_h + body_h + head * 1.12, 0.02),
            (head * 1.1, head * 0.3, head * 1.1), crown),
        Box("arm_l", "body", (-0.58, leg_h + body_h * 0.92, 0), (-0.58, leg_h + body_h * 0.3, 0),
            (0.24, body_h * 1.3, 0.24), robe),
        Box("arm_r", "body", (0.58, leg_h + body_h * 0.92, 0), (0.58, leg_h + body_h * 0.3, 0),
            (0.24, body_h * 1.3, 0.24), robe),
        Box("hand_l", "arm_l", (-0.58, leg_h + body_h * 0.92, 0), (-0.58, leg_h - body_h * 0.42, 0),
            (0.3, 0.3, 0.3), skin),
        Box("hand_r", "arm_r", (0.58, leg_h + body_h * 0.92, 0), (0.58, leg_h - body_h * 0.42, 0),
            (0.3, 0.3, 0.3), skin),
        Box("leg_l", "body", (-0.22, leg_h, 0), (-0.22, leg_h / 2, 0), (0.32, leg_h, 0.32), robe),
        Box("leg_r", "body", (0.22, leg_h, 0), (0.22, leg_h / 2, 0), (0.32, leg_h, 0.32), robe),
    ]
    walk = {"leg_l": swing(22, 1.4), "leg_r": swing(22, 1.4, 0.5), "arm_l": swing(16, 1.4, 0.5),
            "arm_r": swing(16, 1.4)}
    run = {"leg_l": swing(38, 0.8), "leg_r": swing(38, 0.8, 0.5), "arm_l": swing(30, 0.8, 0.5),
           "arm_r": swing(30, 0.8)}
    idle = {"head": swing(5, 4.0, 0.0, quat_y, 6), "arm_l": swing(4, 4.0), "arm_r": swing(4, 4.0, 0.5),
            "mantle": swing(2, 4.0)}
    # The swipe lands at 0.55 s (the attack's windup).
    attack = {"arm_r": [(0.0, quat_x(0)), (0.45, quat_x(-150)), (0.6, quat_x(30)), (1.0, quat_x(0))]}
    # Both fists up, then down on the ground at 0.8 s.
    slam = {"arm_l": [(0.0, quat_x(0)), (0.6, quat_x(-170)), (0.8, quat_x(-20)), (1.2, quat_x(0))],
            "arm_r": [(0.0, quat_x(0)), (0.6, quat_x(-170)), (0.8, quat_x(-20)), (1.2, quat_x(0))],
            "body": [(0.0, quat_x(0)), (0.6, quat_x(-8)), (0.8, quat_x(18)), (1.2, quat_x(0))]}
    cast = {"arm_l": [(0.0, quat_x(0)), (0.4, quat_x(-100)), (0.8, quat_x(-100)), (1.0, quat_x(0))],
            "head": [(0.0, quat_x(0)), (0.4, quat_x(-12)), (1.0, quat_x(0))]}
    hurt = {"body": [(0.0, quat_x(0)), (0.12, quat_x(-8)), (0.35, quat_x(0))]}
    write_model(path, boxes, {"idle": (4.0, idle), "walk": (1.4, walk), "run": (0.8, run), "attack": (1.0, attack),
                              "slam": (1.2, slam), "cast": (1.0, cast), "hurt": (0.35, hurt)})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2] / "examples" / "mods"))
    root = Path(parser.parse_args().root) / "bestiary" / "assets"
    biped(root / "models" / "ghoul.gltf",
          {"body": (62, 84, 66), "head": (140, 168, 128), "arms": (112, 144, 104), "legs": (50, 58, 48)},
          height=1.9, attack=True, hunch=18.0)
    bone_archer(root / "models" / "bone_archer.gltf")
    arrow(root / "models" / "bone_arrow.gltf")
    hex_bolt(root / "models" / "hex_bolt.gltf")
    gloom_bat(root / "models" / "gloom_bat.gltf")
    grave_leaper(root / "models" / "grave_leaper.gltf")
    quadruped(root / "models" / "grey_wolf.gltf",
              {"body": (0.36, 0.4, 1.0), "leg": 0.5, "leg_w": 0.12, "head": (0.28, 0.28, 0.4),
               "ears": (0.07, 0.14, 0.04), "tail": (0.08, 0.08, 0.4)},
              {"body": (118, 116, 112), "head": (132, 130, 126), "legs": (96, 94, 90)}, attack=True)
    hollow_king(root / "models" / "hollow_king.gltf")
    # The Hollow King's theme: a low minor pulse, faster in his last stand.
    write_wav(root / "music" / "hollow_king.wav",
              music_loop(11025, 8.0, [55.0, 65.41, 82.41, 98.0], [1.0, 0.6, 0.35, 0.2], 0.45, 0.95), rate=11025)
    write_wav(root / "music" / "last_stand.wav",
              music_loop(11025, 4.0, [110.0, 130.81, 164.81, 207.65], [1.0, 0.55, 0.3], 0.7, 0.9), rate=11025)
    print(f"bestiary assets written under {root}")


if __name__ == "__main__":
    main()
