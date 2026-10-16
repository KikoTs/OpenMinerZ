import * as THREE from 'three';
import {OrbitControls} from './vendor/OrbitControls.js';
import {TransformControls} from './vendor/TransformControls.js';
import {rigMatrices} from './rig_math.js';

const unit=new THREE.Vector3(1,1,1);
export const matrix=f=>new THREE.Matrix4().compose(new THREE.Vector3(...f.slice(0,3)),new THREE.Quaternion(...f.slice(3)),unit);
export function sample(clip,t){
  const frames=clip.frames;
  if(frames.length===1)return frames[0];
  const i=Math.max(0,Math.min(frames.length-2,Math.floor(t*30))),a=frames[i],b=frames[i+1];
  const u=Math.max(0,Math.min(1,(t-a.time)/(b.time-a.time)));
  const vec=(x,y)=>x.map((v,j)=>v+(y[j]-v)*u),quat=(x,y)=>new THREE.Quaternion(...x).slerp(new THREE.Quaternion(...y),u).toArray();
  const frame=(x,y)=>[...vec(x.slice(0,3),y.slice(0,3)),...quat(x.slice(3),y.slice(3))];
  return {...a,time:t,rotations:a.rotations.map((q,j)=>quat(q,b.rotations[j])),gun:frame(a.gun,b.gun),camera:frame(a.camera,b.camera),view:frame(a.view,b.view),target:vec(a.target,b.target),targetRotation:quat(a.targetRotation||[0,0,0,1],b.targetRotation||[0,0,0,1]),palm:vec(a.palm,b.palm),error:a.error+(b.error-a.error)*u,wristCorrection:a.wristCorrection+(b.wristCorrection-a.wristCorrection)*u,parts:Object.fromEntries(Object.entries(a.parts).map(([n,f])=>[n,frame(f,b.parts[n]||f)])),visible:u===1?b.visible:a.visible};
}

export class RigViewport{
  constructor(stage,{begin,change,end}={}){
    this.stage=stage;this.view='fps';this.mode='translate';this.parts={};this.skinMeshes=[];this.skeletons={};this.boneDots=[];this.delta=new THREE.Matrix4();this.dragging=false;this.weaponToken=0;
    this.scene=new THREE.Scene();this.scene.background=new THREE.Color('#1a2932');
    this.renderer=new THREE.WebGLRenderer({antialias:true});this.renderer.setPixelRatio(Math.min(devicePixelRatio,2));this.renderer.outputColorSpace=THREE.SRGBColorSpace;stage.appendChild(this.renderer.domElement);this.renderer.domElement.tabIndex=0;this.renderer.domElement.addEventListener('pointerdown',()=>this.renderer.domElement.focus({preventScroll:true}));
    this.camera=new THREE.PerspectiveCamera(90,1,.012,100);
    this.orbit=new OrbitControls(this.camera,this.renderer.domElement);this.orbit.target.set(0,1.25,0);this.orbit.enableDamping=true;this.orbit.enabled=false;
    this.scene.add(new THREE.HemisphereLight(0xdcefff,0x59616b,2.1));const light=new THREE.DirectionalLight(0xffffff,2.2);light.position.set(1,3,2);this.scene.add(light);
    this.floor=new THREE.GridHelper(6,60,0x416479,0x293b47);this.scene.add(this.floor);
    this.weaponGroup=new THREE.Group();this.scene.add(this.weaponGroup);this.markers=new THREE.Group();this.scene.add(this.markers);this.boneGroup=new THREE.Group();this.scene.add(this.boneGroup);
    const sphere=(color,size)=>new THREE.Mesh(new THREE.SphereGeometry(size,12,8),new THREE.MeshBasicMaterial({color,depthTest:false,transparent:true,opacity:.8}));this.sphere=sphere;
    this.target=sphere(0x70ffb2,.007);this.palm=sphere(0xff6cd0,.004);this.rightPalm=new THREE.Vector3();this.markers.add(this.target,this.palm);
    this.gizmo=new TransformControls(this.camera,this.renderer.domElement);this.scene.add(this.gizmo.getHelper());this.gizmo.setSize(.7);
    this.gizmo.addEventListener('dragging-changed',e=>{this.dragging=e.value;this.orbit.enabled=!e.value&&this.view!=='fps';if(e.value)begin?.();else end?.();});
    this.gizmo.addEventListener('objectChange',()=>{if(!this.dragging)return;this.target.updateMatrixWorld();const m=this.coordinates(this.editSpace).invert().multiply(this.target.matrixWorld),p=new THREE.Vector3(),q=new THREE.Quaternion();m.decompose(p,q,new THREE.Vector3());change?.({position:p.toArray(),rotation:q.toArray()});});
    new ResizeObserver(()=>this.resize()).observe(stage);
  }
  resize(){const {width,height}=this.stage.getBoundingClientRect();this.renderer.setSize(width,height,false);this.camera.aspect=width/Math.max(1,height);this.camera.updateProjectionMatrix();}
  async initialize(rig){
    this.rig=rig;
    const textures=await Promise.all(rig.textures.map(async t=>{if(!t.width)return null;const bytes=new Uint8Array(await(await fetch('/scene/'+t.url)).arrayBuffer());const tex=new THREE.DataTexture(bytes,t.width,t.height);tex.colorSpace=THREE.SRGBColorSpace;tex.needsUpdate=true;return tex;}));
    const armBone=index=>{for(let i=index;i>=0;i=rig.bones[i].parent)if(['CollarLeft','CollarRight'].includes(rig.bones[i].name))return true;return false;};
    for(const kind of ['stock','avatar']){const source=kind==='stock'?rig.body:rig.avatarBones;const bones=source.map(()=>{const b=new THREE.Bone();b.matrixAutoUpdate=false;this.scene.add(b);return b;});this.skeletons[kind]=new THREE.Skeleton(bones,source.map(b=>new THREE.Matrix4().fromArray(b.inverseBind)));}
    for(const s of rig.surfaces){
      const geometry=new THREE.BufferGeometry();for(const [name,data,width] of [['position',s.position,3],['normal',s.normal,3],['uv',s.uv,2],['skinWeight',s.weights,4]])geometry.setAttribute(name,new THREE.Float32BufferAttribute(data.flat(),width));geometry.setAttribute('skinIndex',new THREE.Uint16BufferAttribute(s.joints.flat(),4));if(s.color)geometry.setAttribute('color',new THREE.Float32BufferAttribute(s.color.flat(),4));
      const arms=[];for(let i=0;i<s.indices.length;i+=3){const tri=s.indices.slice(i,i+3);if(tri.some(v=>s.joints[v].some((b,j)=>s.weights[v][j]>.01&&armBone(s.skin==='stock'?rig.body[b].source:b))))arms.push(...tri);}geometry.setIndex(s.indices);
      const material=new THREE.MeshStandardMaterial({map:textures[s.texture]||null,vertexColors:!!s.color,roughness:.82,side:THREE.DoubleSide,transparent:!!s.blended,alphaTest:.05});const mesh=new THREE.SkinnedMesh(geometry,material);mesh.bind(this.skeletons[s.skin],new THREE.Matrix4());mesh.frustumCulled=false;mesh.userData={kind:s.skin,full:s.indices,arms};this.scene.add(mesh);this.skinMeshes.push(mesh);
    }
    rig.bones.forEach(b=>{const dot=this.sphere(b.name.includes('Left')?0x5dc7ff:b.name.includes('Right')?0xffba65:0xa1b5c3,.005);this.boneGroup.add(dot);this.boneDots.push(dot);});
    this.boneLines=new THREE.LineSegments(new THREE.BufferGeometry(),new THREE.LineBasicMaterial({color:0x7cabbc,depthTest:false,transparent:true,opacity:.65}));this.boneGroup.add(this.boneLines);this.resize();
  }
  async loadWeapon(model,get){
    const token=++this.weaponToken;if(this.model===model)return;const data=await get('/api/weapon?model='+encodeURIComponent(model));const group=new THREE.Group(),parts={};
    for(const s of data.surfaces){const spec=data.parts.find(p=>p.name===s.part)||{},geo=new THREE.BufferGeometry();geo.setAttribute('position',new THREE.Float32BufferAttribute(s.position.flat(),3));geo.setAttribute('normal',new THREE.Float32BufferAttribute(s.normal.flat(),3));if(s.uv)geo.setAttribute('uv',new THREE.Float32BufferAttribute(s.uv.flat(),2));geo.setIndex(s.indices);const tex=s.texture?await new THREE.TextureLoader().loadAsync(s.texture):null;if(tex){tex.flipY=false;tex.colorSpace=THREE.SRGBColorSpace;}const mesh=new THREE.Mesh(geo,new THREE.MeshStandardMaterial({color:new THREE.Color(...s.color),map:tex,roughness:.75,metalness:.12,side:THREE.DoubleSide}));mesh.matrixAutoUpdate=false;mesh.userData={spec,own:s.own};group.add(mesh);(parts[s.part]??=[]).push(mesh);}
    const dispose=g=>g.traverse(o=>{o.geometry?.dispose();o.material?.map?.dispose();o.material?.dispose();});
    if(token!==this.weaponToken){dispose(group);return;}this.scene.remove(this.weaponGroup);dispose(this.weaponGroup);this.weaponGroup=group;this.scene.add(group);this.parts=parts;this.model=model;
  }
  visibility({skin,bones,markers}){this.options={skin,bones,markers};for(const m of this.skinMeshes){m.visible=m.userData.kind===skin;const shape=this.view==='fps'?'arms':'full';if(m.userData.shape!==shape){m.geometry.setIndex(m.userData[shape]);m.userData.shape=shape;}}this.boneGroup.visible=bones;this.markers.visible=markers&&this.leftActive;this.floor.visible=this.view!=='fps';}
  setView(view){this.view=view;this.orbit.enabled=view!=='fps';this.camera.up.set(0,1,0);if(view!=='fps'){this.orbit.target.set(0,1.35,-.08);const positions={front:[0,1.45,1.5],side:[1.2,1.45,0],left:[-1.2,1.45,0],top:[0,2.6,-.08]};this.camera.position.set(...positions[view]);if(view==='top')this.camera.up.set(0,0,-1);this.camera.lookAt(this.orbit.target);this.orbit.update();}if(this.options)this.visibility(this.options);}
  focus(rightHand=false){if(!this.frame)return;const anchor=(rightHand||!this.leftActive?this.rightPalm:this.palm.position).clone();this.setView('front');this.orbit.target.copy(anchor);this.camera.position.copy(anchor).add(new THREE.Vector3(.075,.04,.13));this.camera.lookAt(this.orbit.target);this.orbit.update();}
  coordinates(space){return this.delta.clone().multiply(matrix(this.frame[space==='gun'?'gun':'view']));}
  setFrame(f,clip,profile){
    this.frame=f;const rig=this.rig,lookup=new Map(clip.bones.map((name,i)=>[profile.bones[name]||name,i]));const mask=rig.masks[clip.name.split('@')[0].replace('ReloadEmpty','Reload')];
    const rotations=rig.bones.map((b,i)=>mask&&!mask[i]?rig.idleRotation[i]:f.rotations[lookup.get(b.name)]||[0,0,0,1]),matrices=rigMatrices(rig,rotations);
    for(const [kind,key] of [['stock','body'],['avatar','avatar']])this.skeletons[kind].bones.forEach((b,i)=>{b.matrix.copy(matrices[key][i]);b.matrixWorld.copy(b.matrix);});
    const lines=[];matrices.attachment.forEach((m,i)=>{this.boneDots[i].position.setFromMatrixPosition(m);const p=rig.bones[i].parent;if(p>=0)lines.push(...this.boneDots[i].position.toArray(),...new THREE.Vector3().setFromMatrixPosition(matrices.attachment[p]).toArray());});this.boneLines.geometry.setAttribute('position',new THREE.Float32BufferAttribute(lines,3));
    const actualGun=matrices.attachment[rig.bones.findIndex(b=>b.name==='PropRight')];this.delta.copy(actualGun).multiply(matrix(f.gun).invert());this.weaponGroup.matrixAutoUpdate=false;this.weaponGroup.matrix.copy(actualGun);
    this.rightPalm.fromArray(profile.palms.Right).applyMatrix4(matrices.attachment[rig.bones.findIndex(b=>b.name==='WristRight')]);
    for(const [name,meshes] of Object.entries(this.parts))for(const mesh of meshes){const spec=mesh.userData.spec,pivot=spec.pivot||[0,0,0],value=f.parts[name]||[0,0,0,0,0,0,1];const m=new THREE.Matrix4().makeTranslation(...pivot).multiply(matrix(value)).multiply(new THREE.Matrix4().makeTranslation(...pivot.map(x=>-x)));if(mesh.userData.own){const r=(spec.rotation||[0,0,0]).map(x=>x*Math.PI/180);m.multiply(new THREE.Matrix4().compose(new THREE.Vector3(...pivot),new THREE.Quaternion().setFromEuler(new THREE.Euler(...r,'ZYX')),new THREE.Vector3().setScalar(spec.scale||1)));}mesh.matrix.copy(m);mesh.visible=f.visible[name]??true;}
    this.palm.position.fromArray(f.palm).applyMatrix4(this.delta);this.leftActive=!mask||!!mask[rig.bones.findIndex(b=>b.name==='WristLeft')];this.markers.visible=!!this.options?.markers&&this.leftActive;
    if(this.view==='fps'){const head=matrices.attachment[rig.bones.findIndex(b=>b.name==='Head')];head.clone().multiply(new THREE.Matrix4().makeTranslation(0,.05,0)).multiply(new THREE.Matrix4().makeRotationY(Math.PI)).decompose(this.camera.position,this.camera.quaternion,new THREE.Vector3());}
  }
  edit(contact,enabled){
    if(!this.frame)return;
    if(!this.dragging){const m=contact?this.coordinates(contact.space).multiply(matrix([...contact.position,...contact.rotation])):this.delta.clone().multiply(matrix([...this.frame.target,...(this.frame.targetRotation||[0,0,0,1])]));m.decompose(this.target.position,this.target.quaternion,this.target.scale);this.target.updateMatrixWorld();}
    this.editSpace=contact?.space||'gun';if(enabled&&this.leftActive){if(this.gizmo.object!==this.target)this.gizmo.attach(this.target);}else if(!this.dragging)this.gizmo.detach();
  }
  tools(mode,space,snap){this.mode=mode;this.gizmo.setMode(mode);this.gizmo.setSpace(space);this.gizmo.setTranslationSnap(snap?.001:null);this.gizmo.setRotationSnap(snap?THREE.MathUtils.degToRad(5):null);}
  render(){if(this.view!=='fps')this.orbit.update();this.renderer.render(this.scene,this.camera);}
}
