"""Regression checks for contact authoring, motion continuity and recorded audio."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import unittest

import numpy as np
import make_tactical_animations as anim
import tactical_authoring as author
import tactical_foley as audio


class AuthoringTests(unittest.TestCase):
    def setUp(self):
        self.profile=copy.deepcopy(author.load())

    def test_rejects_bad_bone_maps(self):
        for mapping in ({'WristLeft':'WristRight'}, {'WristLeft':'missing'},
                        {'WristLeft':'ElbowLeft'}, {'WristLeft':'WristLeft','ElbowLeft':'WristLeft'}):
            profile=copy.deepcopy(self.profile);profile['bones']=mapping
            with self.assertRaises(ValueError): author.validate(profile,{'WristLeft','WristRight','ElbowLeft'})

    def test_rejects_invalid_transforms(self):
        for position,rotation in (([float('nan'),0,0],[0,0,0,1]),([0,0],[0,0,0,1]),([0,0,0],[0,0,0,0])):
            profile=copy.deepcopy(self.profile)
            profile['clips']={'GunReload':{'left':[dict(time=0,space='gun',position=position,rotation=rotation)]}}
            with self.assertRaises(ValueError): author.validate(profile)

    def test_rejects_contact_past_end(self):
        _,clips=anim.assault_rifle();clip=next(c for c in clips if c.name=='GunReload')
        previous=author.ACTIVE
        try:
            author.ACTIVE={'clips':{clip.name:{'left':[dict(time=clip.length+1,space='gun',position=[0,0,0],rotation=[0,0,0,1])]}}}
            with self.assertRaises(ValueError): author.apply_clip(clip,anim)
        finally: author.ACTIVE=previous

    def test_continuous_motion_without_contact_overshoot(self):
        times=[0,.2,.65,1,1.1,1.5]
        values=[(0,),(.1,),(.4,),(.55,),(.55,),(.2,)]
        for t in np.linspace(0,1.5,601):
            i=min(len(times)-2,max(k for k,time in enumerate(times) if time<=t))
            value=anim.continuous_curve(times,values,t)[0]
            self.assertGreaterEqual(value,min(values[i][0],values[i+1][0])-1e-10)
            self.assertLessEqual(value,max(values[i][0],values[i+1][0])+1e-10)
        for t in times[1:-1]:
            eps=1e-6;at=anim.continuous_curve(times,values,t)[0]
            before=(at-anim.continuous_curve(times,values,t-eps)[0])/eps
            after=(anim.continuous_curve(times,values,t+eps)[0]-at)/eps
            self.assertAlmostEqual(before,after,delta=.0001)
        self.assertGreater((anim.continuous_curve(times,values,.2+1e-5)[0]-.1)/1e-5,.1)

    def test_edited_grip_still_seats_at_socket(self):
        rig=Path(__file__).resolve().parents[2]/'out/rig-lab/rig.glb'
        if not rig.exists(): self.skipTest('Run Rig Lab --prepare to export owned rig')
        skeleton=anim.Skeleton(anim.read_rig(rig)[0]);_,clips=anim.assault_rifle()
        clip=next(c for c in clips if c.name=='GunReload');anim.prepare_choreography(clip)
        grip,start,end,pivot=clip.part_follow['mag_new'];end=round(end*30)/30
        clip.part_follow['mag_new']=(grip,start,end,pivot)
        contact=clip.left.keys[-3][1]
        clip.left.key(end,anim.HandAt(anim.Frame(anim.v_add(contact.frame.p,(0,.02,-.03)),contact.frame.q)))
        frames=anim.evaluate(skeleton,clip)
        _,_,parts=min(frames,key=lambda f:abs(f[0]-end))
        self.assertLess(anim.v_len(parts['mag_new'].p),.0001)
        self.assertLess(anim.q_angle(parts['mag_new'].q),.01)

    def test_recorded_sources_and_output_headroom(self):
        sources=json.loads((audio.SOURCE/'sources.json').read_text())
        for source in sources:
            self.assertEqual(hashlib.sha256((audio.SOURCE/source['file']).read_bytes()).hexdigest(),source['sha256'])
        for name in audio.RECIPES:
            signal=audio.build(name)
            self.assertTrue(np.isfinite(signal).all())
            self.assertLessEqual(float(np.max(np.abs(signal))),10**(-8/20)+1e-8)
            self.assertGreater(len(signal),audio.RATE*.02)
            self.assertEqual(signal[0],0);self.assertEqual(signal[-1],0)


if __name__=='__main__': unittest.main()
