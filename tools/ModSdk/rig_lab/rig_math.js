import {Matrix4, Vector3, Quaternion} from './vendor/three.core.js';

const unit = new Vector3(1, 1, 1);
const local = (p, q, s=[1,1,1]) => new Matrix4().compose(new Vector3(...p), new Quaternion(...q), new Vector3(...s));
export function rigMatrices(rig, rotations) {
  const attachment = [], body = [], avatar = [];
  const sourceBody = new Map();
  rig.body.forEach((b,i) => {if (!sourceBody.has(b.source)) sourceBody.set(b.source,i);});
  rig.bones.forEach((b,i) => {
    const m=local(b.position, rotations[i]);
    attachment[i] = b.parent < 0 ? m : attachment[b.parent].clone().multiply(m);
  });
  for (const i of rig.bodyOrder) {
    const b = rig.body[i];
    const m = local(b.position, rotations[b.source]);
    body[i] = b.parent < 0 ? new Matrix4().fromArray(rig.bodyRoot).multiply(attachment[b.source]) : body[b.parent].clone().multiply(m);
  }
  const below=[];
  rig.avatarBones.forEach((b,i) => {
    const parent=rig.bones[i].parent;
    below[i]=parent>=0 && (['WristLeft','WristRight'].includes(rig.bones[parent].name) || below[parent]);
    const mapped=sourceBody.get(i);
    if (mapped!==undefined && !below[i]) avatar[i]=body[mapped].clone().scale(new Vector3(...b.cumulativeScale));
    else if (parent<0) avatar[i]=attachment[i].clone();
    else avatar[i]=avatar[parent].clone().multiply(local(b.position,rotations[i],b.scale));
  });
  return {attachment,body,avatar};
}

export function checkParity(rig) {
  let max=0, worst='';
  rig.parity.forEach((sample,s) => {
    const computed=rigMatrices(rig,sample.rotation);
    for(const kind of ['body','avatar']) sample[kind].forEach((matrix,b) => matrix.forEach((x,i) => {
      const error=Math.abs(x-computed[kind][b].elements[i]);
      if(error>max) {max=error;worst=`${s}/${kind}/${b}/${i}`;}
    }));
  });
  return {max,worst,ok:max<0.00002};
}
