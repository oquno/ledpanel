export function lerpTrack(track,t,initial){if(!track?.length)return initial;const sorted=track;if(t<=sorted[0].tick)return sorted[0].value;for(let i=0;i<sorted.length-1;i++){const a=sorted[i],b=sorted[i+1];if(t<b.tick){if(a.ease==='step')return a.value;return a.value+Math.floor((b.value-a.value)*(t-a.tick)/(b.tick-a.tick));}}return sorted.at(-1).value}
export function sceneTick(scene,elapsed){return scene.playback==='loop'?elapsed%scene.durationTicks:Math.min(elapsed,scene.durationTicks-1)}
export function sequenceAsset(layer,tick){
  const total=layer.frames.reduce((sum,f)=>sum+f.ticks,0);
  let t=Math.max(0,tick-layer.startTick);
  t=layer.repeat==='loop'?t%total:Math.min(t,total-1);
  for(const frame of layer.frames){if(t<frame.ticks)return frame.assetId;t-=frame.ticks}
}
export function renderScene(scene,layout,assets,tick,ctx){
  const W=layout.width,H=layout.height;
  ctx.globalAlpha=1;ctx.clearRect(0,0,W,H);ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);
  if(tick<0)return;
  for(const layer of scene.variants[layout.id].layers){
    if(tick<layer.startTick||tick>=layer.endTick)continue;
    const x=lerpTrack(layer.tracks?.x,tick,layer.x),y=lerpTrack(layer.tracks?.y,tick,layer.y),opacity=lerpTrack(layer.tracks?.opacity,tick,layer.opacity)/255;
    ctx.globalAlpha=Math.max(0,Math.min(1,opacity));
    if(layer.kind==='rect'){ctx.fillStyle=layer.color;ctx.fillRect(x,y,layer.width,layer.height)}
    else{const assetId=layer.kind==='text'?layer.rasterAssetId:layer.kind==='sequence'?sequenceAsset(layer,tick):layer.assetId;const img=assets.get(assetId);if(img)ctx.drawImage(img,x,y)}
  }
  ctx.globalAlpha=1;
}
