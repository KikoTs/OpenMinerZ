"""Reproducible edits of licensed, recorded foley. No generated oscillators.

Sources and hashes live beside the original recordings in recorded_foley.
Each cue is a short recording excerpt with DC removal, a 2 ms edge fade,
conservative gain and mono conversion. No artificial ringing or charge whine.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys
import wave

import numpy as np

SOURCE = Path(__file__).with_name("recorded_foley")
RATE = 44100
# recording, start seconds, end seconds, maximum peak dBFS
EXCERPTS = {
    "release": ("reload.wav", .098, .205, -15),
    "pistol_out": ("reload.wav", .58, .77, -11),
    "mag_out": ("assaultriflereload1_0.wav", .15, .34, -12),
    "mag_insert": ("clipload2.wav", 0, .209, -10),
    "mag_seat": ("lightclunk2.wav", 0, .302, -8),
    "rack_back": ("assaultriflereload1_0.wav", 1.025, 1.18, -10),
    "rack_close": ("lightclunk1.wav", 0, .372, -9),
    "metal": ("clank1.wav", 0, .673, -14),
    "rattle": ("rattle1.wav", 0, .778, -15),
    "slide": ("mechanical1.wav", .07, .32, -12),
    "round": ("singlebullet1.wav", 0, .302, -12),
    "cloth_up": ("cloth_sounds.wav", .05, .72, -22),
    "cloth_down": ("cloth_sounds.wav", 3.72, 4.65, -23),
    "pouch": ("cloth_sounds.wav", 5.4, 5.98, -20),
    "swish": ("cloth_sounds.wav", 6.39, 6.76, -20),
}
GROUPS = {
    "release": "ar_mag_release pistol_mag_release smg_mag_release grenade_pin cell_release rifle_bolt_up rifle_bolt_down",
    "pistol_out": "pistol_mag_out",
    "mag_out": "ar_mag_out smg_mag_out lmg_box_out",
    "mag_insert": "ar_mag_in pistol_mag_in smg_mag_in lmg_box_in cell_in rifle_clip_seat",
    "mag_seat": "ar_mag_seat pistol_mag_seat smg_mag_seat cell_seat grip_tap",
    "rack_back": "ar_charge_back pistol_slide_back smg_knob_back rifle_bolt_back lmg_charge_back shotgun_pump_back",
    "rack_close": "ar_charge_forward pistol_slide_forward smg_knob_forward rifle_bolt_forward lmg_charge_forward shotgun_pump_forward cell_prime lmg_cover_close",
    "metal": "mag_hit_ground lmg_cover_open",
    "rattle": "lmg_belt rifle_rounds_in",
    "slide": "cell_eject rpg_rocket_in laser_pump_back laser_pump_forward",
    "round": "shell_in shell_eject laser_shell_in",
    "cloth_up": "cloth_raise",
    "cloth_down": "cloth_lower",
    "pouch": "pouch_mag pouch_box pouch_cell pouch_clip shell_pick rpg_rocket_pick",
    "swish": "knife_swish grenade_throw",
}
RECIPES = {name: EXCERPTS[excerpt] for excerpt,names in GROUPS.items() for name in names.split()}


def build(name: str) -> np.ndarray:
    """Return a clean short mono excerpt; unknown cues are an authoring error."""
    source, start, end, peak_db = RECIPES[name]
    with wave.open(str(SOURCE/source)) as recording:
        if recording.getsampwidth() != 2 or recording.getframerate() != RATE:
            raise ValueError(f"Unexpected recording format: {source}")
        recording.setpos(round(start*RATE))
        values = np.frombuffer(recording.readframes(round((end-start)*RATE)),dtype='<i2')
        signal = values.reshape(-1,recording.getnchannels()).mean(axis=1)/32768.0
    signal -= signal.mean()
    peak = float(np.max(np.abs(signal)))
    if peak < 1e-5: raise ValueError(f"Silent excerpt: {name}")
    signal *= min(4.0, 10**(peak_db/20)/peak)
    active = np.flatnonzero(np.abs(signal) > max(0.0002, float(np.max(np.abs(signal)))*.006))
    signal = signal[max(0,int(active[0]) - 44):min(len(signal),int(active[-1]) + 441)]
    fade = min(88, len(signal)//2)
    signal[:fade] *= np.linspace(0,1,fade)
    signal[-fade:] *= np.linspace(1,0,fade)
    return signal


def write_sounds(folder: Path, names: list[str]) -> list[str]:
    """Replace pack audio with verified recording edits and write provenance."""
    sources = {s['file']:s for s in json.loads((SOURCE/'sources.json').read_text())}
    names = sorted(set(names))
    if set(names)-RECIPES.keys(): raise ValueError(f"Missing recorded cues: {set(names)-RECIPES.keys()}")
    used = {RECIPES[name][0] for name in names}
    for source in used:
        if hashlib.sha256((SOURCE/source).read_bytes()).hexdigest()!=sources[source]['sha256']:
            raise ValueError(f"Recording hash mismatch: {source}")
    sounds = folder/'assets/sounds'
    sounds.mkdir(parents=True,exist_ok=True)
    provenance = {}
    for name in names:
        signal=build(name)
        with wave.open(str(sounds/f'{name}.wav'),'wb') as output:
            output.setparams((1,2,RATE,0,'NONE','not compressed'))
            output.writeframes(np.round(signal*32767).astype('<i2').tobytes())
        file,start,end,db=RECIPES[name]
        provenance[name]={'source':sources[file], 'trim_seconds':[start,end], 'peak_ceiling_dbfs':db}
    for path in sounds.glob('*.wav'):
        if path.stem not in names: path.unlink()
    (folder/'AUDIO_SOURCES.json').write_text(json.dumps(provenance,indent=2)+'\n',encoding='utf-8')
    (folder/'AUDIO_CREDITS.txt').write_text((SOURCE/'CREDITS.txt').read_text(encoding='utf-8'),encoding='utf-8')
    return names


if __name__=='__main__':
    folder=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[2]/'examples/mods/tactical_animations'
    names=re.findall(r'sound = "sounds/([a-z0-9_]+)"',(folder/'client/tactical.luau').read_text())
    print(f'Wrote {len(write_sounds(folder,names))} recorded foley cues')
