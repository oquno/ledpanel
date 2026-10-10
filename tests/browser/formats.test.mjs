import test, { assert } from './harness.mjs';

async function read(path) { const r = await fetch('/' + path); if (!r.ok) throw new Error(path); return new Uint8Array(await r.arrayBuffer()); }
async function readJson(path) { return JSON.parse(new TextDecoder().decode(await read(path))); }
import { decodeLpk, encodeLpk, validateProject, sha256 } from '/packages/formats/index.mjs';
import { sha256Fallback, hmacSha256Fallback } from '/packages/formats/crypto.mjs';
const project=await readJson('examples/demo-project/project.json');
test('HTTP-safe SHA-256 and HMAC fallbacks match standard vectors',async()=>{
  const encoder=new TextEncoder();
  assert.equal(sha256Fallback(new Uint8Array()),'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(sha256Fallback(encoder.encode('abc')),'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.equal(sha256Fallback(encoder.encode('a'.repeat(1000))),await sha256(encoder.encode('a'.repeat(1000))));
  assert.equal(hmacSha256Fallback(new Uint8Array(20).fill(0x0b),encoder.encode('Hi There')),'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7');
});
test('demo project validates with scalable layouts',async()=>{validateProject(project);assert.equal(project.layouts.length,3);assert.ok(project.layouts.some(x=>x.id==='grid_2x2'))});
test('original images may exceed raster limits and malformed tracks are rejected',async()=>{
  const p=structuredClone(project);p.assets.push({id:'original_large',path:'assets/original_large.png',mime:'image/png',role:'original',width:1024,height:1024,sha256:'0'.repeat(64)});validateProject(p);
  p.assets.at(-1).role='raster';await assert.rejects(()=>validateProject(p));p.assets.pop();
  const layer=Object.values(p.scenes[0].variants)[0].layers[0];layer.tracks={x:[{tick:0,value:0,ease:'linear'},{tick:0,value:1,ease:'step'}]};await assert.rejects(()=>validateProject(p));
});
test('LPK2 examples decode and match whole-file hash',async()=>{for(const n of ['minimal','demo']){const b=await read(`examples/${n}.lpk`),d=await decodeLpk(b);assert.equal(d.header.major,2);assert.equal(d.packageHash,await sha256(b));assert.equal(d.manifest.version,2)}});
test('node table is intentionally non-contiguous',async()=>{const rig=await readJson('examples/demo.ledrig.json');assert.deepEqual(rig.nodes.map(n=>n.nodeId),[7,12,42,100]);assert.equal(rig.masterNodeId,7)});

test('LPK2 altered bytes are rejected', async()=>{const b=await read('examples/demo.lpk');b[b.length-1]^=1;await assert.rejects(()=>decodeLpk(b));});

test('browser encoder reproduces the checked-in LPK2 bytes', async()=>{
  const original=await read('examples/demo.lpk'), decoded=await decodeLpk(original);
  const blobs=new Map(decoded.manifest.assets.map(a=>[a.id,decoded.blob.slice(a.offset,a.offset+a.length)]));
  const encoded=await encodeLpk(decoded.manifest,blobs);
  assert.equal(await sha256(encoded),decoded.packageHash);
});
