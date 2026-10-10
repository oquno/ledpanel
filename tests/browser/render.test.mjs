import test, { assert } from './harness.mjs';

import { lerpTrack, sceneTick } from '/packages/render-core/index.mjs';
import { animationTracks, resizeScene } from '/apps/editor/animation.mjs';
import { sequenceAsset, renderScene } from '/packages/render-core/index.mjs';
test('integer keyframe interpolation follows the specification',()=>{
  const t=[{tick:0,value:0,ease:'linear'},{tick:3,value:-2,ease:'step'}];
  assert.equal(lerpTrack(t,0,99),0);assert.equal(lerpTrack(t,1,99),-1);assert.equal(lerpTrack(t,2,99),-2);assert.equal(lerpTrack(t,3,99),-2);
});

test('four scroll directions wrap offscreen and support a partial last cycle',()=>{
  const l={x:0,y:0,opacity:255,startTick:0,endTick:100,tracks:{}};
  for(const direction of ['left','right','up','down']){
    const axis=['left','right'].includes(direction)?'x':'y',reverse=['right','down'].includes(direction);
    const outside=axis==='x'?128:32,inside=-8;
    const t=animationTracks(l,{width:128,height:32},{width:8,height:8},{direction,period:60})[axis];
    assert.equal(lerpTrack(t,0,0),reverse?inside:outside);
    assert.equal(lerpTrack(t,59,0),reverse?outside:inside);
    assert.equal(lerpTrack(t,60,0),reverse?inside:outside);
    assert.ok(lerpTrack(t,99,0)!==lerpTrack(t,60,0));
    const once=animationTracks(l,{width:128,height:32},{width:8,height:8},{direction,period:60,repeat:false})[axis];
    assert.equal(lerpTrack(once,99,0),reverse?outside:inside);
  }
});
test('blink and glow combine with scrolling and respect nonzero start time',()=>{
  const l={x:0,y:0,opacity:200,startTick:10,endTick:130,tracks:{x:[{tick:0,value:10,ease:'step'}]}};
  const blink=animationTracks(l,{}, {},{target:'effect',effect:'blink',period:60});
  assert.deepEqual(blink.x,l.tracks.x);assert.equal(lerpTrack(blink.opacity,39,0),200);assert.equal(lerpTrack(blink.opacity,40,0),0);assert.equal(lerpTrack(blink.opacity,70,0),200);
  const glow=animationTracks(l,{}, {},{target:'effect',effect:'glow',period:60}).opacity;
  assert.equal(lerpTrack(glow,10,0),0);assert.equal(lerpTrack(glow,25,0),100);assert.equal(lerpTrack(glow,40,0),200);assert.equal(lerpTrack(glow,69,0),0);
});
test('preset limits and scene retiming preserve valid integer keys',async()=>{
  const l={x:0,y:0,opacity:255,startTick:0,endTick:18000,tracks:{}};
  await assert.rejects(()=>animationTracks(l,{}, {},{target:'effect',effect:'blink',period:4}));
  l.endTick=100;l.tracks.opacity=[{tick:0,value:0,ease:'linear'},{tick:50,value:255,ease:'linear'},{tick:100,value:0,ease:'step'}];
  const s={durationTicks:100,variants:{a:{layers:[l]}}};resizeScene(s,50);assert.equal(l.endTick,50);assert.deepEqual(l.tracks.opacity.map(k=>k.tick),[0,25,50]);
});
test('sequence frame boundaries, loop, hold and canvas rendering',()=>{
  const l={id:'clip',kind:'sequence',x:0,y:0,opacity:255,startTick:5,endTick:30,tracks:{},repeat:'loop',frames:[{assetId:'red',ticks:2},{assetId:'blue',ticks:3}]};
  assert.equal(sequenceAsset(l,6),'red');assert.equal(sequenceAsset(l,7),'blue');assert.equal(sequenceAsset(l,10),'red');l.repeat='hold';assert.equal(sequenceAsset(l,20),'blue');
  const images=new Map(['red','blue'].map(color=>{const c=document.createElement('canvas');c.width=c.height=1;const x=c.getContext('2d');x.fillStyle=color;x.fillRect(0,0,1,1);return [color,c]}));
  const canvas=document.createElement('canvas');canvas.width=canvas.height=1;const ctx=canvas.getContext('2d');
  renderScene({variants:{a:{layers:[l]}}},{id:'a',width:1,height:1},images,20,ctx);assert.deepEqual([...ctx.getImageData(0,0,1,1).data],[0,0,255,255]);
});
test('loop and hold scene time are deterministic',()=>{
  const loop={durationTicks:30,playback:'loop'},hold={durationTicks:30,playback:'hold'};
  assert.equal(sceneTick(loop,30),0);assert.equal(sceneTick(loop,61),1);assert.equal(sceneTick(hold,61),29);
});
