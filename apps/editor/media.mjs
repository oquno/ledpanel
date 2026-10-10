export function canvasFor(width, height) {
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > 2048 || height > 2048 || width*height > 65536) throw new Error('表示画像は各辺2048px、合計65536画素以内にしてください');
  const canvas = document.createElement('canvas');
  canvas.width = width; canvas.height = height;
  return canvas;
}

export async function decodeImage(blob) {
  const url = URL.createObjectURL(blob), image = new Image();
  try {
    await new Promise((resolve,reject)=>{image.onload=resolve;image.onerror=()=>reject(new Error('画像を読み込めません'));image.src=url});
    return image;
  } finally { URL.revokeObjectURL(url); }
}

export async function pngBytes(canvas) {
  const blob = await new Promise(resolve=>canvas.toBlob(resolve,'image/png'));
  if (!blob) throw new Error('画像の変換に失敗しました');
  return new Uint8Array(await blob.arrayBuffer());
}

export function fitSize(width, height, bounds) {
  const scale = Math.min(1, bounds.width/width, bounds.height/height, Math.sqrt(65536/(width*height)), 2048/width, 2048/height);
  return {width:Math.max(1,Math.floor(width*scale)),height:Math.max(1,Math.floor(height*scale))};
}

export function scaleImage(source, width, height, smoothing=false) {
  const canvas = canvasFor(width,height), ctx=canvas.getContext('2d');
  ctx.imageSmoothingEnabled=smoothing;
  ctx.drawImage(source,0,0,width,height);
  return canvas;
}

export function textCanvas(layer) {
  const lines=layer.text.split('\n'), canvas=canvasFor(layer.boxWidth,lines.length*layer.lineHeightPx), ctx=canvas.getContext('2d');
  ctx.font=`${layer.fontWeight} ${layer.fontSizePx}px ${layer.fontFamily}`;
  ctx.fillStyle=layer.color;ctx.textBaseline='top';
  const segmenter = new Intl.Segmenter('ja',{granularity:'grapheme'});
  lines.forEach((line,row)=>{
    const chars=[...segmenter.segment(line)].map(s=>s.segment), widths=chars.map(c=>ctx.measureText(c).width);
    const width=widths.reduce((a,b)=>a+b,0)+Math.max(0,chars.length-1)*layer.letterSpacingPx;
    let x=layer.align==='center'?Math.floor((layer.boxWidth-width)/2):layer.align==='right'?layer.boxWidth-width:0;
    chars.forEach((c,i)=>{ctx.fillText(c,x,row*layer.lineHeightPx);x+=widths[i]+layer.letterSpacingPx});
  });
  return canvas;
}

function videoEvent(video, event, signal, action) {
  return new Promise((resolve,reject)=>{
    const cleanup=()=>{clearTimeout(timer);video.removeEventListener(event,done);video.removeEventListener('error',failed);signal?.removeEventListener('abort',aborted)};
    const done=()=>{cleanup();resolve()};
    const failed=()=>{cleanup();reject(new Error('この動画を読み込めません。MP4/WebMなどブラウザー対応形式を選んでください'))};
    const aborted=()=>{cleanup();reject(new DOMException('取り込みを中止しました','AbortError'))};
    const timer=setTimeout(failed,15000);
    video.addEventListener(event,done,{once:true});video.addEventListener('error',failed,{once:true});signal?.addEventListener('abort',aborted,{once:true});
    if(signal?.aborted)aborted();else action();
  });
}

// Decode locally, then keep only panel-sized PNGs. No video or audio is uploaded.
export async function extractVideo(file, bounds, {start=0,duration=3,fps=10,signal,onProgress=()=>{}}={}) {
  if (file.size > 100*1024*1024) throw new Error('動画は100MiB以下にしてください');
  if (!Number.isFinite(start) || start<0 || !Number.isFinite(duration) || duration<=0 || duration>6 || ![5,10,15,30].includes(fps)) throw new Error('開始時刻・長さ（最大6秒）・fpsを確認してください');
  const video=document.createElement('video'), url=URL.createObjectURL(file);
  video.muted=true;video.preload='auto';video.playsInline=true;
  try {
    await videoEvent(video,'loadeddata',signal,()=>{video.src=url;video.load()});
    if (!Number.isFinite(video.duration) || start>=video.duration) throw new Error('開始時刻が動画の長さを超えています');
    const seconds=Math.min(duration,video.duration-start), count=Math.max(1,Math.ceil(seconds*fps));
    const size=fitSize(video.videoWidth,video.videoHeight,bounds);
    if(count>120 || size.width*size.height*count>700000)throw new Error('フレーム数・画素数が多すぎます。長さやfpsを下げてください');
    const frames=[];
    for(let i=0;i<count;i++) {
      if(signal?.aborted)throw new DOMException('取り込みを中止しました','AbortError');
      const time=start+i/fps;
      if(Math.abs(video.currentTime-time)>0.00001)await videoEvent(video,'seeked',signal,()=>{video.currentTime=time});
      const data=await pngBytes(scaleImage(video,size.width,size.height,true));
      frames.push({data,ticks:Math.max(1,Math.round(Math.min(1/fps,seconds-i/fps)*30)),...size});
      onProgress(i+1,count);
    }
    return frames;
  } finally {video.removeAttribute('src');video.load();URL.revokeObjectURL(url);}
}
