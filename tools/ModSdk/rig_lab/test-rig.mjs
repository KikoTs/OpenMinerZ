import fs from 'node:fs';
import assert from 'node:assert/strict';
import {checkParity} from './rig_math.js';
const path=process.argv[2];
if(!path)throw Error('Usage: node test-rig.mjs out/rig-lab/scene/rig.json');
const rig=JSON.parse(fs.readFileSync(path));
const check=checkParity(rig);
assert(check.ok,JSON.stringify(check));
assert.equal(rig.masks.PickUse[rig.bones.findIndex(b=>b.name==='WristLeft')],0);
assert.equal(rig.masks.PickUse[rig.bones.findIndex(b=>b.name==='WristRight')],1);
for(const surface of rig.surfaces){
  const count=surface.skin==='stock'?rig.body.length:rig.avatarBones.length;
  surface.joints.forEach((bones,v)=>{assert(bones.every(b=>b>=0&&b<count));assert(Math.abs(surface.weights[v].reduce((a,b)=>a+b,0)-1)<.01);});
  assert(surface.indices.every(i=>i>=0&&i<surface.position.length));
}
console.log(`Native rig parity passed: ${rig.parity.length} poses, ${rig.surfaces.length} surfaces, maximum matrix error ${check.max}`);
