// Browser ES module; cryptography is asynchronous Web Crypto.
import { createBlankProject as createBrowserBlankProject, encodeLpk as browserEncodeLpk } from './browser.mjs';
import { sha256 as digestSha256, hmacSha256 as digestHmacSha256 } from './crypto.mjs';

export const FORMAT_VERSION = 2;
export const PROTOCOL_VERSION = 2;
export const MAX_NODES = 16;
export const MAX_PACKAGE_BYTES = 2621440;
const ID_RE = /^[a-z][a-z0-9_-]{0,31}$/;
const HASH_RE = /^[0-9a-f]{64}$/;
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

export function canonicalJson(value) {
  const sort = (v) => Array.isArray(v) ? v.map(sort) : (v && typeof v === 'object' ? Object.fromEntries(Object.keys(v).sort().map(k => [k, sort(v[k])])) : v);
  return JSON.stringify(sort(value));
}
const hex = bytes => [...new Uint8Array(bytes)].map(x => x.toString(16).padStart(2, '0')).join('');
export async function sha256(bytes) { return digestSha256(bytes); }
export async function hmacSha256(key, bytes) { return digestHmacSha256(key, bytes); }
export function assertId(v, path='id') { if (typeof v !== 'string' || !ID_RE.test(v)) throw new Error(`${path}: invalid id`); }
export function assertHash(v, path='hash') { if (typeof v !== 'string' || !HASH_RE.test(v)) throw new Error(`${path}: invalid sha256`); }
export function assertUuid(v, path='uuid') { if (typeof v !== 'string' || !UUID_RE.test(v)) throw new Error(`${path}: invalid uuid`); }
export function u64(value, path='u64') { if (typeof value !== 'string' || !/^(0|[1-9][0-9]{0,19})$/.test(value)) throw new Error(`${path}: uint64 must be decimal string`); return BigInt(value); }

function ownKeys(obj, allowed, path) { for (const key of Object.keys(obj)) if (!allowed.includes(key)) throw new Error(`${path}: unknown property ${key}`); }
function integer(v, min, max, path) { if (!Number.isInteger(v) || v < min || v > max) throw new Error(`${path}: expected integer ${min}..${max}`); }
function listIds(items, path) { const seen = new Set(); for (const x of items) { assertId(x.id, `${path}.id`); if (seen.has(x.id)) throw new Error(`${path}: duplicate id ${x.id}`); seen.add(x.id); } }

export function validateProject(project) {
  ownKeys(project, ['version','projectId','name','layouts','defaultLayout','assets','scenes','cues'], 'project');
  if (project.version !== 2) throw new Error('project.version must be 2'); assertUuid(project.projectId, 'project.projectId');
  if (!Array.isArray(project.layouts) || project.layouts.length < 1 || project.layouts.length > 16) throw new Error('project.layouts: expected 1..16');
  listIds(project.layouts, 'layouts'); assertId(project.defaultLayout, 'project.defaultLayout');
  const layouts = new Map(project.layouts.map(l => [l.id, l])); if (!layouts.has(project.defaultLayout)) throw new Error('defaultLayout does not exist');
  for (const [i,l] of project.layouts.entries()) {
    ownKeys(l, ['id','name','width','height','slots'], `layouts[${i}]`); integer(l.width,1,4096,`layouts[${i}].width`); integer(l.height,1,4096,`layouts[${i}].height`);
    if (l.width*l.height > 262144 || !Array.isArray(l.slots) || l.slots.length > 16 || l.slots.length < 1) throw new Error(`layouts[${i}]: invalid slots/canvas`);
    listIds(l.slots, `layouts[${i}].slots`); const rects=[];
    for (const [j,s] of l.slots.entries()) { ownKeys(s,['id','x','y'],`layouts[${i}].slots[${j}]`); integer(s.x,0,l.width-64,'slot.x'); integer(s.y,0,l.height-32,'slot.y'); rects.push(s); }
    for (let a=0;a<rects.length;a++) for (let b=a+1;b<rects.length;b++) if (!(rects[a].x+64<=rects[b].x || rects[b].x+64<=rects[a].x || rects[a].y+32<=rects[b].y || rects[b].y+32<=rects[a].y)) throw new Error(`layouts[${i}]: overlapping slots`);
  }
  if (!Array.isArray(project.assets) || project.assets.length > 1024) throw new Error('assets: invalid count'); listIds(project.assets,'assets');
  const assets = new Map(project.assets.map(a => [a.id,a]));
  for (const [i,a] of project.assets.entries()) { ownKeys(a,['id','path','mime','role','width','height','sha256'],`assets[${i}]`); integer(a.width,1,4096,'asset.width'); integer(a.height,1,4096,'asset.height'); if (a.role==='raster' && (a.width>2048 || a.height>2048 || a.width*a.height>65536)) throw new Error('asset too large'); assertHash(a.sha256,'asset.sha256'); }
  if (!Array.isArray(project.scenes) || project.scenes.length < 1 || project.scenes.length > 64) throw new Error('scenes: invalid count'); listIds(project.scenes,'scenes');
  const sceneIds = new Set(project.scenes.map(s=>s.id));
  for (const [i,s] of project.scenes.entries()) { ownKeys(s,['id','name','durationTicks','playback','variants'],`scenes[${i}]`); integer(s.durationTicks,1,18000,'durationTicks'); if (!['loop','hold'].includes(s.playback)) throw new Error('invalid playback'); if (!s.variants || typeof s.variants !== 'object') throw new Error('variants required');
    for (const [lid,v] of Object.entries(s.variants)) { if (!layouts.has(lid)) throw new Error(`scene ${s.id}: unknown layout ${lid}`); if (!Array.isArray(v.layers) || v.layers.length > 16) throw new Error('invalid layers'); const layerIds=new Set(); let keys=0;
      for (const [j,l] of v.layers.entries()) { assertId(l.id,`layer[${j}].id`); if(layerIds.has(l.id))throw new Error('duplicate layer id'); layerIds.add(l.id); if (!['rect','text','image','sequence'].includes(l.kind)) throw new Error('unsupported layer'); integer(l.x,-4096,4096,'layer.x'); integer(l.y,-4096,4096,'layer.y'); integer(l.opacity,0,255,'layer.opacity'); integer(l.startTick,0,s.durationTicks-1,'layer.startTick'); integer(l.endTick,1,s.durationTicks,'layer.endTick'); if(l.startTick>=l.endTick)throw new Error('layer timing'); for(const track of Object.values(l.tracks||{})){if(track.length>64)throw new Error('too many keyframes'); keys+=track.length;}
        if(l.kind==='text'){if(!assets.has(l.rasterAssetId))throw new Error('text raster missing');} if(l.kind==='image'&&!assets.has(l.assetId))throw new Error('image asset missing'); if(l.kind==='sequence'){if(!l.frames?.length)throw new Error('sequence empty'); if(l.frames.reduce((a,f)=>a+f.ticks,0)>18000)throw new Error('sequence too long'); for(const f of l.frames)if(!assets.has(f.assetId))throw new Error('sequence asset missing');}
      } if(keys>512)throw new Error('too many keyframes');
    }
    for(const lid of layouts.keys()) if(!s.variants[lid]) throw new Error(`scene ${s.id}: missing layout variant ${lid}`);
  }
  if (!Array.isArray(project.cues) || project.cues.length < 1 || project.cues.length > 128) throw new Error('cues: invalid count'); listIds(project.cues,'cues');
  for(const c of project.cues){ if(!sceneIds.has(c.sceneId))throw new Error(`cue ${c.id}: unknown scene`); if(c.advance==='manual'){if(c.holdTicks!==null)throw new Error('manual cue holdTicks must be null');}else if(c.advance==='auto'){integer(c.holdTicks,1,108000,'cue.holdTicks');}else throw new Error('cue.advance'); }
  validateEditableLayers(project, assets);
  return {layouts, assets, sceneIds};
}

export async function encodeLpk(manifest, assetBlobs) {
  if (manifest.version !== 2) throw new Error('runtime version must be 2');
  for (const a of manifest.assets) {
    if (assetBlobs.get(a.id)?.length !== a.width * a.height * 3) throw new Error(`asset ${a.id}: length`);
  }
  const encoded = await browserEncodeLpk(structuredClone(manifest), assetBlobs);
  if (new DataView(encoded.buffer, encoded.byteOffset).getUint32(12, true) > 131072) throw new Error('manifest too large');
  return encoded;
}
export async function decodeLpk(bytes) {
  const b = new Uint8Array(bytes), d = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (b.length < 64 || new TextDecoder().decode(b.slice(0, 4)) !== 'LPK2') throw new Error('invalid LPK2 magic');
  const h = {major: d.getUint16(4, true), minor: d.getUint16(6, true), headerSize: d.getUint32(8, true),
    manifestLength: d.getUint32(12, true), blobLength: d.getUint32(16, true), totalLength: d.getUint32(20, true), digest: hex(b.slice(24, 56))};
  if (h.major !== 2 || h.minor !== 0 || h.headerSize !== 64 || h.totalLength !== b.length ||
      h.totalLength !== 64 + h.manifestLength + h.blobLength || h.totalLength > MAX_PACKAGE_BYTES ||
      h.manifestLength > 131072 || d.getUint32(56, true) || d.getUint32(60, true)) throw new Error('invalid LPK2 header');
  if (await sha256(b.subarray(64)) !== h.digest) throw new Error('LPK2 digest mismatch');
  const manifest = JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(b.subarray(64, 64 + h.manifestLength)));
  return {header: h, manifest, blob: b.subarray(64 + h.manifestLength), packageHash: await sha256(b)};
}
export function compileRuntime(project, rasterBytes) {
  validateProject(project);
  const referenced = new Set();
  const runtimeScenes=project.scenes.map(s=>({...s,variants:Object.fromEntries(Object.entries(s.variants).map(([lid,v])=>[lid,{layers:v.layers.map(l=>{
    const base={id:l.id,x:l.x,y:l.y,opacity:l.opacity,startTick:l.startTick,endTick:l.endTick,tracks:structuredClone(l.tracks)};
    if(l.kind==='rect')return {...base,kind:'rect',width:l.width,height:l.height,color:l.color};
    if(l.kind==='sequence'){for(const f of l.frames)referenced.add(f.assetId);return {...base,kind:'sequence',frames:structuredClone(l.frames),repeat:l.repeat};}
    const assetId=l.kind==='text'?l.rasterAssetId:l.assetId;referenced.add(assetId);return {...base,kind:'sprite',assetId};
  })}]))}));
  const manifest={version:2,runtimeMinor:0,renderer:'lp-raster-1',fps:30,projectId:project.projectId,layouts:project.layouts,defaultLayout:project.defaultLayout,
    assets:project.assets.filter(a=>a.role==='raster' && referenced.has(a.id)).map(a=>({id:a.id,width:a.width,height:a.height,offset:0,length:0,sha256:a.sha256})),
    scenes:runtimeScenes,
    cues:project.cues};
  if(manifest.assets.length>512 || manifest.assets.reduce((sum,a)=>sum+a.width*a.height,0)>800000)throw new Error('runtime asset budget exceeded');
  return encodeLpk(manifest,new Map(manifest.assets.map(a=>[a.id,rasterBytes.get(a.id)])));
}
export function createBlankProject() { return createBrowserBlankProject(); }

function validateEditableLayers(project, assets) {
  const raster=id=>{const a=assets.get(id);if(!a||a.role!=='raster')throw new Error('表示画像がありません');return a};
  for(const s of project.scenes)for(const v of Object.values(s.variants))for(const l of v.layers){
    if(!l.tracks || typeof l.tracks!=='object')throw new Error('tracks required');
    for(const [name,keys] of Object.entries(l.tracks)){
      if(!['x','y','opacity'].includes(name)||!Array.isArray(keys)||!keys.length||keys.length>64)throw new Error('invalid track');
      let previous=-1;
      for(const k of keys){integer(k.tick,0,s.durationTicks,'key.tick');integer(k.value,name==='opacity'?0:-4096,name==='opacity'?255:4096,'key.value');if(k.tick<=previous||!['step','linear'].includes(k.ease))throw new Error('invalid keyframe');previous=k.tick;}
      if(keys.at(-1).ease!=='step')throw new Error('last key must use step');
    }
    if(l.kind==='rect'){integer(l.width,1,2048,'rect.width');integer(l.height,1,2048,'rect.height');}
    if(['rect','text'].includes(l.kind)&&!/^#[0-9a-f]{6}$/i.test(l.color))throw new Error('invalid color');
    if(l.kind==='text'){
      integer(l.fontSizePx,6,64,'fontSizePx');integer(l.lineHeightPx,6,96,'lineHeightPx');integer(l.letterSpacingPx,0,16,'letterSpacingPx');integer(l.boxWidth,1,2048,'boxWidth');
      if(typeof l.text!=='string'||![...l.text].length||[...l.text].length>256||!['sans-serif','serif','monospace'].includes(l.fontFamily)||![400,700].includes(l.fontWeight)||!['left','center','right'].includes(l.align))throw new Error('invalid text');
      const a=raster(l.rasterAssetId);if(a.width!==l.boxWidth||a.height!==l.text.split('\n').length*l.lineHeightPx)throw new Error('text raster size');
    }
    if(l.kind==='image'){raster(l.assetId);if(l.originalAssetId&&assets.get(l.originalAssetId)?.role!=='original')throw new Error('original asset missing');}
    if(l.kind==='sequence'){
      if(l.frames.length>512||!['loop','hold'].includes(l.repeat))throw new Error('invalid sequence');
      const first=raster(l.frames[0].assetId);
      for(const f of l.frames){integer(f.ticks,1,18000,'frame.ticks');const a=raster(f.assetId);if(a.width!==first.width||a.height!==first.height)throw new Error('sequence dimensions');}
    }
  }
}
