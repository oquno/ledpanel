import { lerpTrack } from '/packages/render-core/index.mjs';
// Presets expand to the existing integer tracks; no runtime extension is needed.
export function animationTracks(layer, layout, size, {direction='none', effect='none', period=60, repeat=true, target='motion'}) {
  if (!Number.isInteger(period) || period < 4 || period > 18000) throw new Error('周期は4〜18000 tickにしてください');
  const tracks = structuredClone(layer.tracks || {});
  const start = layer.startTick, end = layer.endTick;
  const make = (points) => {
    const keys = new Map();
    for (let base = start; base < end; base += period) {
      for (const [offset, value, ease] of points) keys.set(base + offset, {tick:base + offset, value, ease});
      if (!repeat) break;
    }
    const all = [...keys.values()].sort((a,b)=>a.tick-b.tick);
    const result = all.filter(k=>k.tick<=end);
    if (all.at(-1).tick>end) result.push({tick:end,value:lerpTrack(all,end,0),ease:'step'});
    if (result.length > 64) throw new Error('キーが64個を超えます。周期を長くするかシーンを短くしてください');
    result.at(-1).ease = 'step';
    return result;
  };
  if (target === 'motion') {
    delete tracks.x; delete tracks.y;
    if (direction !== 'none') {
      const horizontal = ['left','right'].includes(direction);
      const reverse = ['right','down'].includes(direction);
      const outside = horizontal ? layout.width : layout.height;
      const inside = -(horizontal ? size.width : size.height);
      tracks[horizontal?'x':'y'] = make([[0,reverse?inside:outside,'linear'],[period-1,reverse?outside:inside,'step']]);
    }
  } else {
    delete tracks.opacity;
    if (effect === 'blink') tracks.opacity = make([[0,layer.opacity,'step'],[Math.floor(period/2),0,'step']]);
    if (effect === 'glow') tracks.opacity = make([[0,0,'linear'],[Math.floor(period/2),layer.opacity,'linear'],[period-1,0,'step']]);
  }
  return tracks;
}

export function resizeScene(scene, duration) {
  if (!Number.isInteger(duration) || duration < 1 || duration > 18000) throw new Error('シーンは1〜18000 tickにしてください');
  const ratio = duration / scene.durationTicks;
  for (const variant of Object.values(scene.variants)) for (const layer of variant.layers) {
    layer.startTick = Math.min(duration-1, Math.round(layer.startTick*ratio));
    layer.endTick = Math.max(layer.startTick+1, Math.min(duration,Math.round(layer.endTick*ratio)));
    for (const [name,keys] of Object.entries(layer.tracks)) {
      const mapped = new Map(keys.map(k=>{const tick=Math.min(duration,Math.round(k.tick*ratio));return [tick,{...k,tick}]}));
      layer.tracks[name] = [...mapped.values()];
    }
  }
  scene.durationTicks = duration;
}

// Keep imported projects usable with the standard physical arrangements.
export function ensureDisplayLayouts(project, standards) {
  for (const layout of standards) {
    if (project.layouts.some(existing => existing.id === layout.id)) continue;
    if (project.layouts.length >= 16) break;
    project.layouts.push(structuredClone(layout));
    for (const scene of project.scenes) scene.variants[layout.id] = {layers:[]};
  }
}

export function selectDisplayLayout(project, targetId) {
  const source = project.layouts.find(layout => layout.id === project.defaultLayout);
  const target = project.layouts.find(layout => layout.id === targetId);
  if (!target) throw new Error('画面サイズが見つかりません');
  const empty = project.scenes.every(scene => scene.variants[targetId].layers.length === 0);
  if (empty && source.id !== targetId) {
    const coordinate = (value, oldSize, newSize) => value < 0 ? value : Math.round(value * newSize / oldSize);
    for (const scene of project.scenes) {
      const layers = structuredClone(scene.variants[source.id].layers);
      for (const layer of layers) {
        for (const [axis, dimension] of [['x','width'],['y','height']]) {
          layer[axis] = coordinate(layer[axis], source[dimension], target[dimension]);
          for (const key of layer.tracks?.[axis] || []) key.value = coordinate(key.value, source[dimension], target[dimension]);
          if (layer.kind === 'rect') layer[dimension] = Math.max(1, Math.round(layer[dimension] * target[dimension] / source[dimension]));
        }
      }
      scene.variants[targetId].layers = layers;
    }
  }
  project.defaultLayout = targetId;
}
