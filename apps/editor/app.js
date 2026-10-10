import { createBlankProject, sha256, randomUuid } from '/packages/formats/browser.mjs';
import { validateProject, compileRuntime } from '/packages/formats/index.mjs';
import { zipStore, unzipStore } from '/packages/formats/zip.mjs';
import { renderScene, sceneTick } from '/packages/render-core/index.mjs';
import { animationTracks, resizeScene, ensureDisplayLayouts, selectDisplayLayout } from '/apps/editor/animation.mjs';
import { canActivateCandidate, canPlayActive, canRunPanelTest, publishSequence } from '/apps/editor/deployment.mjs';
import { decodeImage, pngBytes, scaleImage, textCanvas, fitSize, extractVideo } from '/apps/editor/media.mjs';

const $=s=>document.querySelector(s), project=createBlankProject();
let controlCsrf='', controlRevision=0, firmwareStatus=null, playbackSupported=false, capabilitiesKnown=false, capabilitiesLoadFailed=false, panelReady=false, panelFault='';
const authReady=fetch('/api/v2/auth/csrf',{credentials:'same-origin',cache:'no-store'}).then(async response=>{if(!response.ok)throw new Error('ログインが必要です');controlCsrf=(await response.json()).csrfToken;const status=await fetch('/api/v2/status',{credentials:'same-origin',cache:'no-store'});if(!status.ok)throw new Error('実機の状態を読めません');const data=await status.json();firmwareStatus=panelTestDeviceStatus=data;controlRevision=data.revision;maintenanceMode=data.maintenance===true;maintenanceScope=data.maintenanceScope||'local_only';candidateHash=data.state.candidatePackageHash;activePackageHash=data.state.activePackageHash;renderDeployment();renderRigEditor();renderPanelTest();renderLiveControls()}).catch(error=>toast(error.message));
let selectedScene=null, selectedLayer=null, selectedCue=0, selectedLayout=project.defaultLayout;
let tick=0, playing=false, playOrigin=0, assets=new Map(), assetBytes=new Map(), deployedHash=null, candidateHash=null, activePackageHash=null, dirty=false;
let busy=false, videoAbort=null, rigEnvelope=null, rigSaving=false, maintenanceMode=false, maintenanceScope='local_only', publishStep='', publishResultMessage='';
let topologyDistributionSupported=false, topologyPollTimer=null, topologyPollUntil=0, deviceNodes=[];
let panelTestSupported=false,panelTestDeviceStatus=null,panelTestUntil=0,panelTestTimer=null,panelTestBusy=false;
const history=[], future=[], drafts=new Map();
let firmwareBootId='', firmwareRestartNoticeShown=false;
const uid=prefix=>`${prefix}_${randomUuid().replaceAll('-','').slice(0,20)}`;
const blankScene=()=>({id:uid('scene'),name:'新しいシーン',durationTicks:180,playback:'loop',variants:Object.fromEntries(project.layouts.map(l=>[l.id,{layers:[]}]))});
project.scenes.push(blankScene());selectedScene=project.scenes[0].id;project.scenes[0].name='曲名';
project.cues.push({id:uid('cue'),name:'1曲目',sceneId:selectedScene,advance:'manual',holdTicks:null});
function toast(message){$('#toast').textContent=message;$('#toast').classList.add('show');clearTimeout(toast.timer);toast.timer=setTimeout(()=>$('#toast').classList.remove('show'),5000)}
function apiError(result,fallback){
  const code=typeof result?.error==='string'?result.error:result?.error?.code;
  const messages={
    UNAUTHENTICATED:'ログインし直してください。',AUTH_OR_CSRF_REQUIRED:'ログインし直してください。',
    NOT_FOUND:'送信先が見つかりません。画面を更新してください。',
    MASTER_REQUIRED:'親機の画面で操作してください。',MAINTENANCE_REQUIRED:'更新準備を完了できませんでした。',
    MULTI_NODE_SYNC_UNAVAILABLE:'もう1台との通信を確認できません。',TOPOLOGY_NOT_SYNCHRONIZED:'もう1台への配置確認を待っています。',
    PANEL_PROFILE_UNVERIFIED:'パネルの表示方式が未確認のため、通常表示を止めています。',
    PANEL_DMA_UNAVAILABLE:'パネルを初期化できませんでした。パネルの電源・配線・設定を確認してください。',
    PANEL_NIGHTLIGHT_UNAVAILABLE:'弱い点灯を開始できませんでした。',
    STALE_REVISION:'画面の情報が古くなりました。ページを再読み込みしてからやり直してください。',
    CONFLICT:'機器の状態が変わりました。更新してやり直してください。',
  };
  return messages[code]||(code?`処理に失敗しました。コード: ${code}`:fallback);
}
function watchTopologySync(){
  if(!topologyDistributionSupported||rigEnvelope?.topologySynchronized)return;
  topologyPollUntil=Date.now()+90000;
  if(topologyPollTimer)return;
  const poll=async()=>{
    topologyPollTimer=null;
    if(Date.now()>topologyPollUntil||rigEnvelope?.topologySynchronized)return;
    await renderDevices();
    if(!rigEnvelope?.topologySynchronized&&Date.now()<=topologyPollUntil)
      topologyPollTimer=setTimeout(poll,4000);
  };
  topologyPollTimer=setTimeout(poll,2000);
}
async function checkFirmwareRestart(){
  if(firmwareRestartNoticeShown)return;
  try{
    const response=await fetch('/api/v2/health',{credentials:'same-origin',cache:'no-store'});
    if(!response.ok)return;
    const data=await response.json();if(typeof data.bootId!=='string'||!data.bootId)return;
    if(!firmwareBootId){firmwareBootId=data.bootId;return}
    if(data.bootId!==firmwareBootId){firmwareRestartNoticeShown=true;$('#firmwareRestartNotice').hidden=false}
  }catch{}
}
function esc(v){return String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function activeLayout(){return project.layouts.find(l=>l.id===selectedLayout)||project.layouts[0]}
function scene(){return project.scenes.find(s=>s.id===selectedScene)||project.scenes[0]}
function variant(){return scene().variants[activeLayout().id]}
function layer(){return variant().layers.find(l=>l.id===selectedLayer)}
function snapshot(){return {project:structuredClone(project),assets:new Map(assets),bytes:new Map(assetBytes),selectedScene,selectedLayer,selectedLayout}}
function restore(s){drafts.clear();Object.assign(project,s.project);assets=s.assets;assetBytes=s.bytes;selectedScene=s.selectedScene;selectedLayer=s.selectedLayer;selectedLayout=s.selectedLayout;tick=0;previewSceneIfAnimated();markDirty();renderAll()}
function markDirty(){dirty=true;deployedHash=null;window.lastLpk=null;publishResultMessage='';$('#compileStatus').textContent='未生成';$('#saveState').textContent='未保存';renderDeployment()}
function renderDeployment(){
  const blocker=publishBlocker();
  const publishButton=$('#publishProject');
  publishButton.disabled=Boolean(blocker)||busy;
  publishButton.textContent=busy?'処理中…':'この内容を表示';
  publishButton.title=busy?(publishStep||'実機へ反映しています'):blocker||'表示データを作成し、実機へ送り、再生を始めます';
  $('#maintenanceToggle').textContent=maintenanceMode?'更新準備を終える':'更新の準備';
  $('#maintenanceToggle').disabled=!controlCsrf||busy;
  $('#deployCandidate').disabled=!maintenanceMode||!window.lastLpk||!controlCsrf||busy;
  $('#deployCandidate').title=maintenanceMode?'':'表示データを作り、「更新の準備」を押してください';
  const topologySynchronized=rigEnvelope?.topologySynchronized===true;
  $('#activateCandidate').disabled=!canActivateCandidate({maintenanceMode,maintenanceScope,candidateHash,csrf:controlCsrf,busy,topologyDistributionSupported,topologySynchronized});
  $('#activateCandidate').title=maintenanceScope==='follower'?'親機の画面で操作してください':maintenanceScope==='local_only'?'追加パネルの接続を確認してください':topologyDistributionSupported&&!topologySynchronized?'追加パネルが内容を確認するまで待っています。':maintenanceScope==='single_node'?'この1台だけに反映します':'';
  const state=firmwareStatus?.state||{};
  const showing=state.running&&!state.blackout&&activePackageHash&&activePackageHash===deployedHash;
  const status=$('#deploymentStatus');
  status.textContent=publishStep||publishResultMessage||blocker||
    (showing?`実機に「${project.name}」を表示中です。`:activePackageHash&&activePackageHash===deployedHash
      ?'この内容は本体に保存済みです。再生するには「本番」画面の「表示」を押してください。'
      :'編集プレビューです。実機に出すには「この内容を表示」を押してください。');
  status.dataset.state=publishStep?'working':publishResultMessage?'error':blocker?'blocked':showing?'active':'ready';
  const lowLight=firmwareStatus?.panelOutput==='dim_nightlight';
  $('#deviceState').textContent=!firmwareStatus?.state?'状態確認中':
    lowLight?'弱く点灯中':state.running&&!state.blackout?'表示中':state.blackout?'消灯中':'停止中';
  renderLiveControls();
}
function publishBlocker(){
  if(!firmwareStatus?.state)return '本体の状態を確認しています。';
  if(!controlCsrf)return 'ログイン状態を確認できません。再ログインしてください。';
  if(firmwareStatus.role==='follower')return '追加パネルでは操作できません。親機の画面を開いてください。';
  if(!capabilitiesKnown)return 'パネルの準備状態を確認しています。';
  if(capabilitiesLoadFailed)return '機器の状態を読み取れません。接続を確認して「機器」タブの「更新」を押してください。';
  if(!panelReady)return `パネルが表示できる状態ではありません（${apiError({error:panelFault||'PANEL_DMA_UNAVAILABLE'})}）。機器タブで接続と設定を確認してください。`;
  if(!playbackSupported)return '本体の再生機能が準備できていません。機器タブで状態を確認してください。';
  if(maintenanceScope==='follower')return '追加パネルでは操作できません。親機の画面を開いてください。';
  if(maintenanceScope==='local_only')return '追加パネルの接続を確認できません。機器タブで接続状態を確認してください。';
  // Publishing synchronizes the staged layout itself. Requiring the previous
  // topology here prevents the first update after the panels reboot.
  if(topologyDistributionSupported&&!rigEnvelope)return 'パネル構成を読み込んでいます。';
  return '';
}
function renderPanelTest(){
  const button=$('#panelTestButton');if(!button)return;
  if(panelTestDeviceStatus?.panelOutput==='dim_nightlight'){button.textContent='弱く点灯中';button.disabled=true;$('#panelTestStatus').textContent='暖色で弱く点灯しています。';return;}
  if(panelTestDeviceStatus?.panelOutput==='diagnostic_rgb_bars'){
    button.textContent='RGBテスト表示中';button.disabled=true;
    $('#panelTestStatus').textContent='RGB色帯テストを表示中です。終了すると自動で消灯します。';
    return;
  }
  button.textContent='点灯を確認（30秒）';
  const state=panelTestDeviceStatus?.state||{};
  const active=panelTestUntil>Date.now();
  const safe=canRunPanelTest({csrf:controlCsrf,busy:busy||panelTestBusy,supported:panelTestSupported,role:panelTestDeviceStatus?.role,mode:state.mode,running:state.running,blackout:state.blackout,maintenance:panelTestDeviceStatus?.maintenance});
  button.disabled=!safe||active;
  if(active)$('#panelTestStatus').textContent=`RGB色帯を全画面で点滅しています。あと${Math.max(1,Math.ceil((panelTestUntil-Date.now())/1000))}秒で自動消灯します。`;
  else if(panelTestSupported)$('#panelTestStatus').textContent=safe?'表示を止めてから実行してください。控えめな明るさで色を30秒表示し、自動で消灯します。':'表示を止めて消灯すると、点灯テストを実行できます。';
  else $('#panelTestStatus').textContent='点灯テストを使えません。パネルは消灯中です。';
}
function renderLiveControls(){
  const state=firmwareStatus?.state||{};
  const statusKnown=!!firmwareStatus?.state;
  const stopped=state.mode==='stopped'&&!state.running;
  const lowLight=firmwareStatus?.panelOutput==='dim_nightlight';
  const ready=canPlayActive({playbackSupported,activeHash:activePackageHash,editorHash:deployedHash,busy});
  const paused=!state.running&&state.mode!=='stopped';
  const showing=!!state.running&&!state.blackout;
  $('#go').disabled=!ready||showing;
  $('#go').textContent=showing?'表示中':paused?'▶ 再開':'▶ 表示';
  $('#go').title=ready?'':'先にデザイン画面で「この内容を表示」を押してください';
  $('#next').disabled=!ready||stopped;
  $('#prev').disabled=!ready||stopped;
  $('#pause').disabled=!ready||state.mode==='stopped';
  $('#pause').textContent=state.running?'一時停止':'再開';
  $('#stopLive').disabled=!statusKnown||!controlCsrf||busy||(stopped&&!lowLight);
  $('#stopLive').textContent=stopped&&!lowLight?'停止済み':'停止して消灯';
  $('#stopLive').title=lowLight?'弱い点灯を止めてパネルを消灯します':stopped?'すでに停止しています':'再生を止めてパネルを消灯します';
  if(Number.isInteger(state.brightnessCap))$('#brightness').max=state.brightnessCap;
  if(Number.isInteger(state.userBrightness))$('#brightness').value=state.userBrightness;
  const cue=project.cues[selectedCue];
  const playbackState=!statusKnown?'状態を確認中':lowLight?'低照度で点灯中':state.running?'再生中':stopped?'停止中':'一時停止中';
  $('#liveCurrent').textContent=!statusKnown?playbackState:lowLight?playbackState:state.running?(state.cueId?project.cues.find(c=>c.id===state.cueId)?.name||'再生中':'再生中'):playbackState;
  $('#liveHint').textContent=!statusKnown?'機器の状態を確認しています。':
    lowLight?'弱く点灯中です。デザイン画面で「この内容を表示」を押すと、そのまま切り替わります。':
    stopped?'「表示」で再生と点灯を始めます。':paused?'一時停止中です。「再開」または「停止して消灯」を選べます。':'再生中です。「停止して消灯」で止めます。';
  $('#liveDetail').textContent=lowLight?'暖色で弱く点灯中です。':!activePackageHash?'本体に表示内容がありません。デザイン画面で「この内容を表示」を押してください。':
    state.blackout?`${playbackState}・消灯中`:
    state.running?'パネルに表示中':`停止中 · ${cue?.name||'表示を選択'}`;
}
async function readPlaybackStatus(){
  const response=await fetch('/api/v2/status',{credentials:'same-origin',cache:'no-store'});
  if(!response.ok)throw new Error('実機の状態を読み取れません');
  firmwareStatus=panelTestDeviceStatus=await response.json();
  if(Number.isInteger(firmwareStatus.revision))controlRevision=firmwareStatus.revision;
  if(firmwareStatus.state){
    candidateHash=firmwareStatus.state.candidatePackageHash;
    activePackageHash=firmwareStatus.state.activePackageHash;
  }
  maintenanceMode=firmwareStatus.maintenance===true;
  maintenanceScope=firmwareStatus.maintenanceScope||maintenanceScope;
  renderLiveControls();renderDeployment();
  return firmwareStatus;
}
async function refreshPlaybackStatus(){
  if(busy)return;
  try{await readPlaybackStatus()}catch{}
}
// Serialize asynchronous edits so saves and undo cannot capture half an edit.
async function edit(action){
  if(busy)return;
  const before=snapshot();busy=true;$('#design').setAttribute('aria-busy','true');$('#design').inert=true;
  try{await action();validateProject(project);history.push(before);if(history.length>20)history.shift();future.length=0;markDirty()}
  catch(e){restore(before);toast(e.message)}
  finally{busy=false;$('#design').removeAttribute('aria-busy');$('#design').inert=false;renderAll()}
}
function draw(canvas,t=tick){const l=activeLayout(),s=scene();if(canvas.width!==l.width)canvas.width=l.width;if(canvas.height!==l.height)canvas.height=l.height;const ctx=canvas.getContext('2d');ctx.imageSmoothingEnabled=false;renderScene(s,l,assets,sceneTick(s,t),ctx)}
function renderPreview(){
  tick=Math.min(tick,scene().durationTicks-1);draw($('#canvas'));drawLive();
  $('#scrub').max=scene().durationTicks-1;$('#scrub').value=tick;
  $('#tickLabel').textContent=`${(tick/30).toFixed(2)} / ${(scene().durationTicks/30).toFixed(2)} 秒`;
  $('#play').textContent=playing?'⏸ 一時停止':'▶ 再生';
}
function startPreviewAt(startTick){tick=startTick;playing=true;playOrigin=performance.now()-tick*1000/30}
function layerHasAnimation(l){return Object.values(l.tracks||{}).some(keys=>Array.isArray(keys)&&keys.length>0)||(l.kind==='sequence'&&l.frames?.length>1)}
function animatedPreviewStartTick(){const starts=variant().layers.filter(layerHasAnimation).map(l=>l.startTick);return starts.length?Math.min(...starts):null}
function previewSceneIfAnimated(){const start=animatedPreviewStartTick();if(start===null)return false;startPreviewAt(start);return true}
function renderAll(){
  $('#projectLabel').textContent=project.name;$('#projectName').value=project.name;
  $('#layoutSelect').innerHTML=project.layouts.map(l=>`<option value="${l.id}">${esc(l.name)} (${l.width}×${l.height})</option>`).join('');$('#layoutSelect').value=selectedLayout;
  $('#layoutSelect').title=selectedLayout==='vertical'?'親機が上、2枚目が下。この画面サイズで実機へ表示します。':selectedLayout==='horizontal'?'親機が左、2枚目が右。この画面サイズで実機へ表示します。':'この画面サイズで実機へ表示します。';
  let hint=$('#layoutPlacement');if(!hint){hint=document.createElement('p');hint.id='layoutPlacement';hint.className='muted';$('#layoutSelect').parentElement.after(hint)}hint.textContent=$('#layoutSelect').title;
  $('#sceneList').innerHTML=project.scenes.map(s=>`<button class="scene ${s.id===selectedScene?'active':''}" data-scene="${s.id}">${esc(s.name)}</button>`).join('');
  $('#layerList').innerHTML=variant().layers.slice().reverse().map(l=>`<button class="layer ${l.id===selectedLayer?'active':''}" data-layer="${l.id}">${esc(l.name||l.kind)} <small>${Object.keys(l.tracks).length?'動きあり':l.kind}</small></button>`).join('')||'<p class="muted">左のボタンから文字・画像を追加してください。</p>';
  $('#sceneName').value=scene().name;$('#duration').value=scene().durationTicks;$('#scenePlayback').value=scene().playback;
  $('#undo').disabled=!history.length;$('#redo').disabled=!future.length;
  renderInspector();renderCues();renderPreview();
}
const input=(label,p,value,min,max)=>`<label>${label}<input data-p="${p}" type="number" min="${min}" max="${max}" step="1" value="${value}" required></label>`;
const options=(items,value)=>items.map(([v,t])=>`<option value="${v}" ${v===value?'selected':''}>${t}</option>`).join('');
function sizeOf(l){if(l.kind==='rect')return l;return project.assets.find(a=>a.id===(l.kind==='text'?l.rasterAssetId:l.kind==='sequence'?l.frames[0].assetId:l.assetId))}
function renderInspector(){
  const l=layer();if(!l){$('#inspectorBody').innerHTML='<p class="muted">レイヤーを選択すると、サイズやアニメーションを編集できます。</p>';return}
  const size=sizeOf(l),d=drafts.get(l.id)||{};
  let html=`<fieldset><legend>配置</legend><label>名前<input data-p="name" maxlength="80" required value="${esc(l.name||l.kind)}"></label><div class="pair">${input('X','x',l.x,-4096,4096)}${input('Y','y',l.y,-4096,4096)}</div>${input('不透明度（0〜255）','opacity',l.opacity,0,255)}<button data-action="center">中央に配置</button></fieldset>`;
  if(l.kind==='text')html+=`<fieldset><legend>文字</legend><label>本文<textarea data-p="text" maxlength="512" required>${esc(l.text)}</textarea></label><div class="pair">${input('枠幅 px','boxWidth',l.boxWidth,1,2048)}${input('文字サイズ','fontSizePx',l.fontSizePx,6,64)}</div><div class="pair">${input('行高','lineHeightPx',l.lineHeightPx,6,96)}${input('字間','letterSpacingPx',l.letterSpacingPx,0,16)}</div><label>書体<select data-p="fontFamily">${options([['sans-serif','ゴシック'],['serif','明朝'],['monospace','等幅']],l.fontFamily)}</select></label><label>太さ<select data-p="fontWeight">${options([['400','標準'],['700','太字']],String(l.fontWeight))}</select></label><label>文字揃え<select data-p="align">${options([['left','左'],['center','中央'],['right','右']],l.align)}</select></label><label>色<input type="color" data-p="color" value="${l.color}"></label><button data-action="textFit">文字全体に枠を合わせる</button></fieldset>`;
  if(l.kind==='rect')html+=`<fieldset><legend>矩形</legend><div class="pair">${input('幅','width',l.width,1,2048)}${input('高さ','height',l.height,1,2048)}</div><label>色<input type="color" data-p="color" value="${l.color}"></label></fieldset>`;
  if(l.kind==='image')html+=`<fieldset><legend>画像サイズ</legend><p class="muted">表示 ${size.width} × ${size.height} px</p><div class="pair"><label>幅 px<input id="imageWidth" type="number" min="1" max="2048" value="${size.width}"></label><label>高さ px<input id="imageHeight" type="number" min="1" max="2048" value="${size.height}"></label></div><label class="check"><input id="aspectLock" type="checkbox" checked>縦横比を固定</label><label>補間<select id="resampling"><option value="nearest">くっきり（ドット絵）</option><option value="smooth">なめらか（写真）</option></select></label><div class="button-row"><button data-action="resize">サイズ適用</button><button data-action="fit">画面に収める</button><button data-action="original">元サイズ</button></div></fieldset>`;
  if(l.kind==='sequence')html+=`<fieldset><legend>動画フレーム</legend><p class="muted">${l.frames.length}枚 / ${(l.frames.reduce((n,f)=>n+f.ticks,0)/30).toFixed(2)}秒 / ${size.width}×${size.height}px</p><label>フレーム再生<select data-p="repeat">${options([['loop','繰り返し'],['hold','最後を保持']],l.repeat)}</select></label></fieldset>`;
  html+=`<fieldset><legend>アニメーション</legend><label>スクロール<select id="motion">${options([['none','なし'],['left','← 左へ'],['right','→ 右へ'],['up','↑ 上へ'],['down','↓ 下へ']],d.direction||'none')}</select></label><label>点滅<select id="effect">${options([['none','なし'],['blink','点滅'],['glow','グロー点滅（ゆっくり明暗）']],d.effect||'none')}</select></label><label>1周期（tick / 30 = 秒）<input id="period" type="number" min="4" max="18000" step="1" value="${d.period||60}"></label><label class="check"><input id="animRepeat" type="checkbox" ${d.repeat===false?'':'checked'}>シーン内で繰り返す</label><div class="button-row"><button data-action="motion">移動を適用</button><button data-action="effect">点滅を適用</button></div><p class="muted">動きのあるレイヤーを選ぶと保存済みアニメーションを再生します。種類を変えた場合も自動再生します。周期・繰返しを変えた後は適用ボタンを押してください。移動と点滅を組み合わせられます。</p><p class="muted">保存済み: ${Object.entries(l.tracks).map(([k,v])=>`${k} ${v.length}キー`).join(' / ')||'動きなし'}</p><button data-action="clearAnimation">すべての動きを解除</button></fieldset>`;
  html+=`<fieldset><legend>表示時間（tick）</legend><div class="pair">${input('開始','startTick',l.startTick,0,scene().durationTicks-1)}${input('終了','endTick',l.endTick,1,scene().durationTicks)}</div></fieldset><div class="button-row"><button data-action="duplicate">複製</button><button data-action="front">前面へ</button><button data-action="back">背面へ</button><button id="deleteLayer" class="danger" data-action="delete">削除</button></div>`;
  $('#inspectorBody').innerHTML=html;
  if(l.kind==='image')for(const [a,b] of [['Width','Height'],['Height','Width']])$('#image'+a).oninput=()=>{if($('#aspectLock').checked)$('#image'+b).value=Math.max(1,Math.round(Number($('#image'+a).value)*size[b.toLowerCase()]/size[a.toLowerCase()]))};
}
async function addRaster(canvas,id=uid('asset')){
  const data=await pngBytes(canvas), image=await decodeImage(new Blob([data],{type:'image/png'}));
  assetBytes.set(id,data);assets.set(id,image);
  const info={id,path:`assets/${id}.png`,mime:'image/png',role:'raster',width:canvas.width,height:canvas.height,sha256:await sha256(data)};
  const current=project.assets.find(a=>a.id===id);if(current)Object.assign(current,info);else project.assets.push(info);
  return id;
}
async function rasterizeText(l){l.rasterAssetId=await addRaster(textCanvas(l))}
function baseLayer(kind,name){return {id:uid(kind),name,kind,x:0,y:0,opacity:255,startTick:0,endTick:scene().durationTicks,tracks:{}}}
function appendLayer(l){if(variant().layers.length>=16)throw new Error('1レイアウトのレイヤーは16個までです');variant().layers.push(l);selectedLayer=l.id}
async function addText(){const l={...baseLayer('text','文字'),text:'曲名',fontFamily:'sans-serif',fontSizePx:16,fontWeight:700,color:'#FFFFFF',align:'left',lineHeightPx:20,letterSpacingPx:0,boxWidth:48,rasterAssetId:''};await rasterizeText(l);appendLayer(l)}
function addRect(){appendLayer({...baseLayer('rect','矩形'),width:8,height:8,color:'#00AAFF'})}
async function importImage(file){
  if(file.size>20*1024*1024||!['image/png','image/jpeg'].includes(file.type))throw new Error('20MiB以下のPNG/JPEGを選んでください');
  const source=await decodeImage(file);if(source.width>4096||source.height>4096)throw new Error('元画像は4096×4096以下にしてください');
  const id=uid('original'),bytes=new Uint8Array(await file.arrayBuffer());assets.set(id,source);assetBytes.set(id,bytes);
  project.assets.push({id,path:`assets/${id}.${file.type==='image/jpeg'?'jpg':'png'}`,mime:file.type,role:'original',width:source.width,height:source.height,sha256:await sha256(bytes)});
  const size=fitSize(source.width,source.height,activeLayout()),assetId=await addRaster(scaleImage(source,size.width,size.height));
  appendLayer({...baseLayer('image',file.name.slice(0,80)),assetId,originalAssetId:id});
}
function referencedAssets(includeOriginal=true){const ids=new Set();for(const s of project.scenes)for(const v of Object.values(s.variants))for(const l of v.layers){if(l.rasterAssetId)ids.add(l.rasterAssetId);if(l.assetId)ids.add(l.assetId);if(includeOriginal&&l.originalAssetId)ids.add(l.originalAssetId);for(const f of l.frames||[])ids.add(f.assetId)}return ids}
function pruneAssets(){const used=referencedAssets();project.assets=project.assets.filter(a=>used.has(a.id));for(const id of assets.keys())if(!used.has(id)){assets.delete(id);assetBytes.delete(id)}}
async function updateLayer(p,value){
  const l=layer();if(!l)return;
  if(p==='color')value=value.toUpperCase();
  if(p==='text'){value=value.normalize('NFC');if([...value].length>256)throw new Error('文字は256文字までです')}
  if(p==='opacity'&&l.opacity>0&&l.tracks.opacity)l.tracks.opacity=l.tracks.opacity.map(k=>({...k,value:Math.round(k.value*value/l.opacity)}));
  if(['x','y'].includes(p)&&l.tracks[p])l.tracks[p]=l.tracks[p].map(k=>({...k,value:k.value+value-l[p]}));
  l[p]=value;
  if(l.kind==='text'&&['text','color','fontSizePx','fontFamily','fontWeight','align','lineHeightPx','letterSpacingPx','boxWidth'].includes(p)){await rasterizeText(l);pruneAssets()}
}
async function layerAction(action){
  const l=layer();if(!l)return;
  if(action==='delete'){variant().layers=variant().layers.filter(x=>x!==l);selectedLayer=null;pruneAssets()}
  if(action==='duplicate'){const copy=structuredClone(l);copy.id=uid(l.kind);copy.name=(l.name+' コピー').slice(0,80);appendLayer(copy)}
  if(['front','back'].includes(action)){const list=variant().layers,i=list.indexOf(l),j=action==='front'?Math.min(list.length-1,i+1):Math.max(0,i-1);[list[i],list[j]]=[list[j],list[i]]}
  if(action==='center'){const s=sizeOf(l);await updateLayer('x',Math.floor((activeLayout().width-s.width)/2));await updateLayer('y',Math.floor((activeLayout().height-s.height)/2))}
  if(action==='textFit'){const ctx=document.createElement('canvas').getContext('2d');ctx.font=`${l.fontWeight} ${l.fontSizePx}px ${l.fontFamily}`;const segmenter=new Intl.Segmenter('ja',{granularity:'grapheme'});l.boxWidth=Math.max(1,Math.ceil(Math.max(...l.text.split('\n').map(line=>{const chars=[...segmenter.segment(line)].map(s=>s.segment);return chars.reduce((w,c)=>w+ctx.measureText(c).width,0)+Math.max(0,chars.length-1)*l.letterSpacingPx}))));await rasterizeText(l);pruneAssets()}
  if(['resize','fit','original'].includes(action)){
    const source=assets.get(l.originalAssetId||l.assetId);
    if(!l.originalAssetId){const a=project.assets.find(a=>a.id===l.assetId),id=uid('original');project.assets.push({...a,id,path:`assets/${id}.png`,role:'original'});assets.set(id,source);assetBytes.set(id,assetBytes.get(a.id));l.originalAssetId=id}
    const size=action==='fit'?fitSize(source.width,source.height,activeLayout()):action==='original'?source:{width:Number($('#imageWidth').value),height:Number($('#imageHeight').value)};
    l.assetId=await addRaster(scaleImage(source,size.width,size.height,$('#resampling').value==='smooth'));pruneAssets();
  }
  if(['motion','effect'].includes(action)){
    const settings={direction:$('#motion').value,effect:$('#effect').value,period:Number($('#period').value),repeat:$('#animRepeat').checked,target:action};
    if(settings.period>l.endTick-l.startTick)throw new Error('周期を表示時間以下にするか、シーンを長くしてください');
    l.tracks=animationTracks(l,activeLayout(),sizeOf(l),settings);drafts.set(l.id,settings);tick=l.startTick;playing=true;playOrigin=performance.now()-tick*1000/30;
  }
  if(action==='clearAnimation'){l.tracks={};drafts.delete(l.id)}
}

async function buildRuntimePackage(){
  validateProject(project);const raw=new Map();
  for(const id of referencedAssets(false)){
    const img=assets.get(id);if(!img)throw new Error(`画像がありません: ${id}`);
    const ctx=scaleImage(img,img.width,img.height).getContext('2d'),rgba=ctx.getImageData(0,0,img.width,img.height).data,bytes=new Uint8Array(img.width*img.height*3);
    for(let i=0,j=0;i<rgba.length;i+=4,j+=3){const rgb=rgba[i+3]?((rgba[i]>>3)<<11)|((rgba[i+1]>>2)<<5)|(rgba[i+2]>>3):0;bytes[j]=rgb&255;bytes[j+1]=rgb>>8;bytes[j+2]=rgba[i+3]}
    raw.set(id,bytes);
  }
  return compileRuntime(project,raw);
}
async function compile(){
  if(busy)return toast('編集処理が完了するまでお待ちください');
  busy=true;
  try{
    const lpk=await buildRuntimePackage();deployedHash=await sha256(lpk);window.lastLpk=lpk;$('#compileStatus').textContent=`生成済み ${(lpk.length/1024).toFixed(1)} KiB`;
    renderDeployment();download(new Blob([lpk]),`${project.name}.lpk`);toast('表示データを作成しました');
  }catch(e){toast(`表示データを作れませんでした: ${e.message}`)}finally{busy=false;renderDeployment()}
}
function download(blob,name){const a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}
async function postJson(path,body,fallback){
  await authReady;if(!controlCsrf)throw new Error('ログインしてください');
  const response=await fetch(path,{method:'POST',credentials:'same-origin',cache:'no-store',headers:{'Content-Type':'application/json','X-CSRF-Token':controlCsrf},body:JSON.stringify(body)});
  const result=await response.json().catch(()=>({}));if(!response.ok)throw new Error(apiError(result,fallback));
  if(Number.isInteger(result.acceptedRevision))controlRevision=result.acceptedRevision;
  return result;
}
async function setMaintenance(enabled){
  await authReady;await readPlaybackStatus();
  const result=await postJson(`/api/v2/maintenance/${enabled?'enter':'exit'}`,{requestId:randomUuid(),expectedRevision:controlRevision},'更新準備を変更できません');
  maintenanceMode=result.maintenance===true;maintenanceScope=result.maintenanceScope||maintenanceScope;
  return result;
}
async function stagePackage(bytes){
  await authReady;if(!controlCsrf)throw new Error('ログインしてください');
  const hash=await sha256(bytes),response=await fetch('/api/v2/deployments',{method:'POST',credentials:'same-origin',cache:'no-store',headers:{'Content-Type':'application/vnd.ledpanel.package','X-CSRF-Token':controlCsrf,'X-Expected-Revision':String(controlRevision),'X-Content-SHA256':hash},body:bytes});
  const result=await response.json().catch(()=>({}));if(!response.ok)throw new Error(apiError(result,'本体へ送信できませんでした'));
  controlRevision=result.acceptedRevision;candidateHash=result.packageHash;maintenanceScope=result.maintenanceScope||maintenanceScope;
  return result;
}
async function activatePackage(hash){
  const result=await postJson('/api/v2/deployments/activate',{packageHash:hash,expectedRevision:controlRevision},'本体の内容を更新できませんでした');
  activePackageHash=result.packageHash;candidateHash=null;
  if(topologyDistributionSupported){
    const layout=rigEnvelope?.rig?.layouts?.find(item=>item.layoutId===selectedLayout);
    const requiredIds=new Set((layout?.bindings||[]).map(binding=>String(binding.nodeId)));
    const deadline=Date.now()+15000;
    while(true){
      const status=await readPlaybackStatus();
      const followers=(status.nodes||[]).filter(node=>node.role==='follower'&&node.enabled!==false&&requiredIds.has(String(node.nodeId)));
      if(followers.every(node=>node.online&&node.ready&&node.activeHash===hash))break;
      if(Date.now()>=deadline)throw new Error('追加パネルの表示準備を確認できませんでした。');
      await new Promise(resolve=>setTimeout(resolve,250));
    }
  }
  return result;
}
async function postCommand(op,args={}){
  await authReady;await readPlaybackStatus();
  return postJson('/api/v2/commands',{requestId:randomUuid(),expectedRevision:controlRevision,op,args},'実機の操作に失敗しました');
}
async function waitForTopology(packageHash){
  if(!topologyDistributionSupported)return;
  const deadline=Date.now()+90000;
  while(Date.now()<deadline){
    publishStep='複数パネルへ送り、受信を確認しています…';renderDeployment();
    await renderDevices();
    const layoutId=firmwareStatus?.state?.layoutId||selectedLayout;
    const layout=rigEnvelope?.rig?.layouts?.find(item=>item.layoutId===layoutId);
    const requiredIds=new Set((layout?.bindings||[]).map(binding=>String(binding.nodeId)));
    const followers=deviceNodes.filter(node=>node.role==='follower'&&node.enabled!==false&&requiredIds.has(String(node.nodeId)));
    const topologyReady=rigEnvelope?.topologySynchronized===true;
    const packagesReady=followers.every(node=>node.online&&
      (node.candidateHash===packageHash||node.activeHash===packageHash));
    if(topologyReady&&packagesReady)return;
    await new Promise(resolve=>setTimeout(resolve,2000));
  }
  throw new Error('追加パネルの接続確認が終わりません。機器タブで接続状態を確認してください。');
}
async function publishProject(){
  if(busy)return;
  busy=true;publishResultMessage='';publishStep='表示内容を確認しています…';renderDeployment();
  let activated=false;
  try{
    await authReady;
    const blocker=publishBlocker();if(blocker)throw new Error(blocker);
    publishStep='表示データを作っています…';renderDeployment();
    const bytes=await buildRuntimePackage(),hash=await sha256(bytes);
    deployedHash=hash;window.lastLpk=bytes;$('#compileStatus').textContent=`生成済み ${(bytes.length/1024).toFixed(1)} KiB`;
    await publishSequence({
      readStatus:readPlaybackStatus,
      command:postCommand,
      enterMaintenance:()=>setMaintenance(true),
      sendPackage:()=>stagePackage(bytes),
      waitForPeers:topologyDistributionSupported?waitForTopology:null,
      activatePackage,
      exitMaintenance:()=>setMaintenance(false),
      cueId:project.cues[selectedCue]?.id,
      onStep:step=>{publishStep=step;renderDeployment()},
    });
    activated=true;publishStep='';publishResultMessage='';
    toast('実機に表示しました');
  }catch(error){
    activated=error.activated===true;
    publishStep='';
    if(maintenanceMode){try{await setMaintenance(false)}catch(cleanupError){error=new Error(`${error.message} 更新状態を解除できませんでした: ${cleanupError.message}`)}}
    try{await readPlaybackStatus()}catch{}
    const recoveryNote=error.restoreError?` 以前の表示にも戻せませんでした: ${error.restoreError.message}`:'';
    publishResultMessage=activated?`本体への保存は完了しましたが、表示を開始できませんでした: ${error.message}${recoveryNote}`:`実機に表示できませんでした: ${error.message}${recoveryNote}`;
    toast(activated?'保存済みですが表示を始められませんでした':`表示できません: ${error.message}`);
  }finally{busy=false;publishStep='';renderDeployment()}
}
$('#publishProject').onclick=publishProject;
$('#maintenanceToggle').onclick=async()=>{if(busy)return;busy=true;renderDeployment();try{await setMaintenance(!maintenanceMode);await renderDevices();if(maintenanceMode)watchTopologySync();toast(maintenanceMode?'パネルを更新する準備ができました':'通常の表示に戻しました')}catch(error){toast(`更新準備: ${error.message}`)}finally{busy=false;renderDeployment()}};
$('#deployCandidate').onclick=async()=>{if(!window.lastLpk)return;busy=true;renderDeployment();try{await stagePackage(window.lastLpk);if(topologyDistributionSupported){if(rigEnvelope)rigEnvelope.topologySynchronized=false;await renderDevices();watchTopologySync()}renderDeployment();toast('本体へ送信しました')}catch(error){toast(`本体への送信: ${error.message}`)}finally{busy=false;renderDeployment()}};
$('#activateCandidate').onclick=async()=>{if(!candidateHash)return;busy=true;renderDeployment();try{await activatePackage(candidateHash);renderDeployment();toast('本体の内容を更新しました')}catch(error){toast(`本体の更新: ${error.message}`)}finally{busy=false;renderDeployment()}};
function saveProject(){
  if(busy)return toast('編集処理が完了するまでお待ちください');
  try{validateProject(project);const files=[{name:'project.json',data:JSON.stringify(project,null,2)}];for(const a of project.assets){const data=assetBytes.get(a.id);if(!data)throw new Error(`画像がありません: ${a.id}`);files.push({name:a.path,data})}download(new Blob([zipStore(files)],{type:'application/vnd.ledpanel.project+zip'}),`${project.name}.ledproj`);dirty=false;$('#saveState').textContent='保存ファイル出力済み'}catch(e){toast(e.message)}
}
async function loadProject(file){
  if(file.size>20*1024*1024)throw new Error('プロジェクトは20MiB以下にしてください');
  const bytes=new Uint8Array(await file.arrayBuffer()),entries=file.name.endsWith('.json')?[{name:'project.json',data:bytes}]:unzipStore(bytes),entry=entries.find(e=>e.name==='project.json');
  if(!entry)throw new Error('project.jsonがありません');
  const next=JSON.parse(new TextDecoder().decode(entry.data));validateProject(next);
  ensureDisplayLayouts(next,createBlankProject().layouts);validateProject(next);
  const images=new Map(),data=new Map();
  for(const a of next.assets){const e=entries.find(e=>e.name===a.path);if(!e)throw new Error(`画像がありません: ${a.path}`);if(await sha256(e.data)!==a.sha256)throw new Error(`画像ハッシュが一致しません: ${a.id}`);const img=await decodeImage(new Blob([e.data],{type:a.mime}));if(img.width!==a.width||img.height!==a.height)throw new Error(`画像サイズが一致しません: ${a.id}`);images.set(a.id,img);data.set(a.id,e.data)}
  stop();Object.assign(project,next);assets=images;assetBytes=data;selectedScene=project.scenes[0].id;selectedLayout=project.defaultLayout;selectedLayer=null;selectedCue=0;drafts.clear();previewSceneIfAnimated();toast('プロジェクトを読み込みました');
}
async function command(op,args={}){
  if(busy)return;
  try{
    await postCommand(op,args);
    if(op==='go')await postCommand('blackout',{enabled:false});
    await readPlaybackStatus();toast(op==='go'?'表示を始めました':op==='stop'?'停止して消灯しました':'実機に適用しました');
  }catch(error){try{await readPlaybackStatus()}catch{}toast(`実機操作: ${error.message}`)}
}
function stop(){playing=false;tick=0}
function animate(now){if(playing){tick=Math.floor((now-playOrigin)*30/1000);if(scene().playback==='hold'&&tick>=scene().durationTicks){tick=scene().durationTicks-1;playing=false}else tick=sceneTick(scene(),tick);renderPreview()}requestAnimationFrame(animate)}

document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tabs button,.tab').forEach(x=>x.classList.remove('active'));b.classList.add('active');$(`#${b.dataset.tab}`).classList.add('active');renderAll();if(b.dataset.tab==='devices')renderDevices()});
$('#layoutSelect').onchange=e=>{const id=e.target.value;edit(()=>{stop();selectDisplayLayout(project,id);selectedLayout=id;selectedLayer=null;previewSceneIfAnimated()})};
$('#newText').onclick=()=>edit(addText);$('#newRect').onclick=()=>edit(addRect);
$('#newScene').onclick=()=>edit(()=>{if(project.scenes.length>=64)throw new Error('シーンは64個までです');const s=blankScene();project.scenes.push(s);selectedScene=s.id;selectedLayer=null;stop()});
$('#sceneList').onclick=e=>{const s=e.target.closest('[data-scene]');if(s){selectedScene=s.dataset.scene;selectedLayer=null;stop();previewSceneIfAnimated();renderAll()}};
$('#layerList').onclick=e=>{const item=e.target.closest('[data-layer]');if(item){selectedLayer=item.dataset.layer;const selected=layer();if(selected&&layerHasAnimation(selected))startPreviewAt(selected.startTick);renderAll()}};
$('#inspectorBody').onchange=e=>{
  if(e.target.id==='motion'||e.target.id==='effect'){const target=e.target.id;edit(()=>layerAction(target));return}
  const p=e.target.dataset.p;if(!p)return;if(!e.target.checkValidity()){e.target.reportValidity();return}const value=e.target.type==='number'||p==='fontWeight'?Number(e.target.value):e.target.value;edit(()=>updateLayer(p,value))
};
$('#inspectorBody').onclick=e=>{const b=e.target.closest('[data-action]');if(b)edit(()=>layerAction(b.dataset.action))};
$('#projectName').onchange=e=>edit(()=>{project.name=e.target.value.trim()||'新しいライブ表示'});
$('#sceneName').onchange=e=>edit(()=>{scene().name=e.target.value.trim()||'シーン'});
$('#duration').onchange=e=>edit(()=>{resizeScene(scene(),Number(e.target.value));stop();drafts.clear()});
$('#scenePlayback').onchange=e=>edit(()=>{scene().playback=e.target.value});
$('#undo').onclick=()=>{if(busy||!history.length)return;future.push(snapshot());stop();restore(history.pop())};
$('#redo').onclick=()=>{if(busy||!future.length)return;history.push(snapshot());stop();restore(future.pop())};
document.addEventListener('keydown',e=>{if(!$('#design').classList.contains('active')||$('#videoDialog').open||e.target.closest('input,textarea,select'))return;if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='z'){e.preventDefault();$(e.shiftKey?'#redo':'#undo').click()}if(e.code==='Space'){e.preventDefault();$('#play').click()}});
$('#compile').onclick=compile;$('#saveProject').onclick=saveProject;
$('#loadProject').onclick=()=>$('#projectFile').click();
$('#projectFile').onchange=e=>{const file=e.target.files[0];e.target.value='';if(file)edit(()=>loadProject(file))};
$('#importImage').onclick=()=>$('#imageFile').click();
$('#imageFile').onchange=e=>{const file=e.target.files[0];e.target.value='';if(file)edit(()=>importImage(file))};
$('#scrub').oninput=e=>{tick=Number(e.target.value);playOrigin=performance.now()-tick*1000/30;renderPreview()};
$('#play').onclick=()=>{playing=!playing;playOrigin=performance.now()-tick*1000/30;renderPreview()};
$('#stop').onclick=()=>{stop();renderPreview()};
$('#newCue').onclick=()=>edit(()=>{if(project.cues.length>=128)throw new Error('キューは128個までです');const s=scene();project.cues.push({id:uid('cue'),name:s.name,sceneId:s.id,advance:'manual',holdTicks:null})});
$('#go').onclick=()=>command(firmwareStatus?.state?.mode==='stopped'?'go':'resume',{cueId:project.cues[selectedCue]?.id});$('#next').onclick=()=>command('next');$('#prev').onclick=()=>command('prev');$('#pause').onclick=()=>command(firmwareStatus?.state?.running?'pause':'resume');$('#stopLive').onclick=()=>command('stop');$('#brightness').onchange=e=>command('brightness',{value:Number(e.target.value)});
$('#refreshDevices').onclick=renderDevices;$('#panelTestButton').onclick=async()=>{
  renderPanelTest();
  if(!canRunPanelTest({csrf:controlCsrf,busy:busy||panelTestBusy,supported:panelTestSupported,role:panelTestDeviceStatus?.role,mode:panelTestDeviceStatus?.state?.mode,running:panelTestDeviceStatus?.state?.running,blackout:panelTestDeviceStatus?.state?.blackout,maintenance:panelTestDeviceStatus?.maintenance}))return;
  panelTestBusy=true;renderPanelTest();
  try{
    await authReady;
    const currentResponse=await fetch('/api/v2/status',{credentials:'same-origin',cache:'no-store'});
    if(!currentResponse.ok)throw new Error('実機の安全状態を確認できません');
    panelTestDeviceStatus=await currentResponse.json();
    if(!canRunPanelTest({csrf:controlCsrf,busy,supported:panelTestSupported,role:panelTestDeviceStatus?.role,mode:panelTestDeviceStatus?.state?.mode,running:panelTestDeviceStatus?.state?.running,blackout:panelTestDeviceStatus?.state?.blackout,maintenance:panelTestDeviceStatus?.maintenance})){
      renderPanelTest();throw new Error('表示を止め、消灯してから実行してください');
    }
    const response=await fetch('/api/v2/panel-test',{method:'POST',credentials:'same-origin',cache:'no-store',headers:{'Content-Type':'application/json','X-CSRF-Token':controlCsrf},body:'{}'});
    const result=await response.json();if(!response.ok)throw new Error(apiError(result,'点灯テストを開始できません'));
    panelTestUntil=Date.now()+(result.durationMs||30000);toast('点灯確認を開始しました。30秒後に自動消灯します');
    clearInterval(panelTestTimer);panelTestTimer=setInterval(()=>{renderPanelTest();if(panelTestUntil<=Date.now()){clearInterval(panelTestTimer);panelTestTimer=null}},250);
  }catch(error){toast(`点灯テスト: ${error.message}`)}
  finally{panelTestBusy=false;renderPanelTest()}
};
$('#addDevice').onclick=()=>{$('#panelConnectionInfo').open=true;$('#panelConnectionInfo').scrollIntoView({behavior:'smooth',block:'center'})};
$('#saveRig').onclick=saveRig;
$('#importVideo').onclick=()=>$('#videoDialog').showModal();
$('#cancelVideo').onclick=()=>{if(videoAbort)videoAbort.abort();else $('#videoDialog').close()};
$('#videoDialog').addEventListener('cancel',e=>{if(videoAbort){e.preventDefault();videoAbort.abort()}});
$('#startVideo').onclick=async()=>{
  const file=$('#videoFile').files[0];if(!file)return toast('動画を選んでください');
  if(busy)return;
  $('#startVideo').disabled=true;videoAbort=new AbortController();
  await edit(async()=>{
    const frames=await extractVideo(file,activeLayout(),{start:Number($('#videoStart').value),duration:Number($('#videoDuration').value),fps:Number($('#videoFps').value),signal:videoAbort.signal,onProgress:(n,total)=>$('#videoProgress').textContent=`フレーム変換中 ${n} / ${total}`});
    const l={...baseLayer('sequence',file.name.slice(0,80)),repeat:'loop',frames:[]};
    for(const frame of frames){if(videoAbort.signal.aborted)throw new Error('取り込みを中止しました');const id=uid('frame');assetBytes.set(id,frame.data);assets.set(id,await decodeImage(new Blob([frame.data],{type:'image/png'})));project.assets.push({id,path:`assets/${id}.png`,mime:'image/png',role:'raster',width:frame.width,height:frame.height,sha256:await sha256(frame.data)});l.frames.push({assetId:id,ticks:frame.ticks})}
    const duration=l.frames.reduce((n,f)=>n+f.ticks,0);if(duration>scene().durationTicks)resizeScene(scene(),duration);l.endTick=scene().durationTicks;appendLayer(l);$('#videoDialog').close();toast(`${frames.length}フレームを取り込みました（音声なし）`);
  });
  videoAbort=null;$('#startVideo').disabled=false;$('#videoProgress').textContent='';
};
window.addEventListener('beforeunload',e=>{if(dirty||busy){e.preventDefault();e.returnValue=''}});
renderAll();renderDevices().then(()=>{if(topologyDistributionSupported&&!rigEnvelope?.topologySynchronized)watchTopologySync()});renderLiveControls();requestAnimationFrame(animate);
$('#reloadAfterRestart').onclick=()=>window.location.reload();
window.addEventListener('online',checkFirmwareRestart);setInterval(checkFirmwareRestart,15000);setInterval(refreshPlaybackStatus,1000);checkFirmwareRestart();

function renderCues(){
  selectedCue=Math.min(selectedCue,project.cues.length-1);
  $('#cueTable').innerHTML=project.cues.map((c,i)=>`<tr><td>${i+1}</td><td><input data-cue-name="${c.id}" maxlength="80" value="${esc(c.name)}"></td><td><select data-cue-scene="${c.id}">${project.scenes.map(s=>`<option value="${s.id}" ${s.id===c.sceneId?'selected':''}>${esc(s.name)}</option>`).join('')}</select></td><td><select data-cue-advance="${c.id}"><option value="manual" ${c.advance==='manual'?'selected':''}>手動</option><option value="auto" ${c.advance==='auto'?'selected':''}>${c.holdTicks||300} tick後</option></select></td><td><button data-cue-go="${c.id}">選択</button><button data-cue-del="${c.id}">削除</button></td></tr>`).join('');
  document.querySelectorAll('[data-cue-name]').forEach(e=>e.onchange=()=>edit(()=>{project.cues.find(c=>c.id===e.dataset.cueName).name=e.value.trim()||'キュー'}));
  document.querySelectorAll('[data-cue-scene]').forEach(e=>e.onchange=()=>edit(()=>{project.cues.find(c=>c.id===e.dataset.cueScene).sceneId=e.value}));
  document.querySelectorAll('[data-cue-advance]').forEach(e=>e.onchange=()=>edit(()=>{const c=project.cues.find(c=>c.id===e.dataset.cueAdvance);c.advance=e.value;c.holdTicks=e.value==='auto'?300:null}));
  document.querySelectorAll('[data-cue-go]').forEach(e=>e.onclick=()=>{selectedCue=project.cues.findIndex(c=>c.id===e.dataset.cueGo);drawLive();toast('本番画面の選択キューを更新しました')});
  document.querySelectorAll('[data-cue-del]').forEach(e=>e.onclick=()=>edit(()=>{if(project.cues.length>1)project.cues=project.cues.filter(c=>c.id!==e.dataset.cueDel)}));
}
function renderRigEditor(){
  if(!rigEnvelope?.rig)return;
  const rig=rigEnvelope.rig,nodes=rig.nodes||[],projectLayoutIds=new Set(project.layouts.map(layout=>layout.id));
  $('#rigSummary').textContent=`登録 ${nodes.length}台・使用中 ${nodes.filter(node=>node.enabled).length}台 · 配置 ${rigEnvelope.topologySynchronized?'確認済み':'未確認'}`;
  const nodeHtml=nodes.map(node=>`<fieldset class="rig-node"><legend>パネル ${node.nodeId}</legend><div class="pair"><label>表示名<input data-rig-label="${node.nodeId}" maxlength="80" value="${esc(node.label)}"></label><label>明るさの上限<input data-rig-brightness="${node.nodeId}" type="number" min="0" max="255" value="${node.brightnessCap}"></label></div><label class="check"><input data-rig-enabled="${node.nodeId}" type="checkbox" ${node.enabled?'checked':''} ${node.role==='master'?'disabled':''}>このパネルを使う</label><label>向き<select data-rig-rotation="${node.nodeId}">${options([[0,'通常'],[180,'上下反転']],node.rotation)}</select></label><details><summary>接続の詳細</summary><small>機器番号: ${node.nodeId} · 種類: ${node.role==='master'?'親機':'追加機器'} · ID: ${esc(node.hardwareId)}</small></details></fieldset>`).join('');
  const savedLayouts=new Map((rig.layouts||[]).map(layout=>[layout.layoutId,layout]));
  const layoutHtml=project.layouts.map(layout=>{
    const saved=savedLayouts.get(layout.id),bound=new Map((saved?.bindings||[]).map(binding=>[binding.slotId,binding.nodeId]));
    const slots=layout.slots.map(slot=>`<label>${esc(slot.id)} (${slot.x},${slot.y})<select data-rig-binding="${esc(layout.id)}|${esc(slot.id)}"><option value="">未割当</option>${nodes.filter(node=>node.enabled).map(node=>`<option value="${node.nodeId}" ${bound.get(slot.id)===node.nodeId?'selected':''}>Node ${node.nodeId} · ${esc(node.label)}</option>`).join('')}</select></label>`).join('');
    return `<fieldset class="rig-layout"><legend>${esc(layout.name)} · ${esc(layout.id)}</legend><div class="pair">${slots}</div></fieldset>`;
  }).join('');
  $('#rigEditorFields').innerHTML=nodeHtml+layoutHtml;
  $('#rigEditor').hidden=false;
  $('#saveRig').disabled=!controlCsrf||rigSaving||!rigEnvelope.editable;
}
async function renderDevices(){
  try{
    const [nodeResponse,rigResponse,provisioningResponse,capabilityResponse,statusResponse]=await Promise.all([
      fetch('/api/v2/nodes',{credentials:'same-origin',cache:'no-store'}),
      fetch('/api/v2/rig',{credentials:'same-origin',cache:'no-store'}),
      fetch('/api/v2/provisioning',{credentials:'same-origin',cache:'no-store'}),
      fetch('/api/v2/capabilities',{credentials:'same-origin',cache:'no-store'}),
      fetch('/api/v2/status',{credentials:'same-origin',cache:'no-store'})
    ]);
    if(!nodeResponse.ok||!rigResponse.ok)throw new Error('機器情報を読み込めません');
    if(provisioningResponse.ok){
      const provisioning=await provisioningResponse.json();
      const mode=provisioning.networkMode;
      $('#networkModeStatus').textContent=mode==='infrastructure'
        ?'外部AP接続（SSID・鍵・接続先はUSB設定で管理）'
        :mode==='softap'?'内蔵AP接続（SSID・鍵はUSB設定ファイルを参照）'
        :'ネットワーク設定を確認できません';
    }
    capabilitiesKnown=true;capabilitiesLoadFailed=!capabilityResponse.ok;
    const capabilities=capabilityResponse.ok?await capabilityResponse.json():null;
    topologyDistributionSupported=capabilities?.features?.topologyDistribution===true;
    panelTestSupported=capabilities?.features?.panelDiagnosticTest===true;
    playbackSupported=capabilities?.features?.playback===true;
    panelReady=capabilities?.panelReady===true;
    panelFault=capabilities?.panelFault||'';
    if(statusResponse.ok)firmwareStatus=panelTestDeviceStatus=await statusResponse.json();
    renderPanelTest();
    renderLiveControls();
    $('#fleetCapabilityStatus').textContent=describeFleetCapabilities(capabilities);
    const d=await nodeResponse.json();deviceNodes=d.nodes;rigEnvelope=await rigResponse.json();maintenanceScope=rigEnvelope.maintenanceScope||maintenanceScope;renderDeployment();
    $('#connectionDot').className='dot';$('#connectionText').textContent=`本体に接続 / パネル${d.nodes.length}台`;
    $('#deviceList').innerHTML=d.nodes.map(n=>{
      const label=esc(n.label||`Node ${n.nodeId}`), cls=n.online?'status-ok':'status-warn', state=n.online?'online':'offline';
      const active=n.activeHash?esc(n.activeHash.slice(0,12))+'…':'なし',candidate=n.candidateHash?esc(n.candidateHash.slice(0,12))+'…':'なし';
      const clock=n.clockSynced?'時刻合わせ済み':'時刻未確認';
      const stateText=n.online?(n.ready?'接続中・使用できます':'接続中・準備待ち'):'未接続';
      return `<div class="card"><h3>${label}</h3><div>パネル ${n.nodeId} · ${n.role==='master'?'親機':'追加機器'}</div><div class="${cls}">${stateText}</div><details><summary>接続の詳細</summary><div>状態: ${esc(n.syncState||'未確認')} · 理由: ${esc(n.faultCode||'なし')}</div><div>割当: ${esc(n.slotId||'なし')} · ${clock}</div><div>使用中データ: ${active} · 送信待ち: ${candidate}</div><small>機器ID: ${esc(n.hardwareId||'')}</small></details></div>`;
    }).join('');
    renderRigEditor();renderDeployment();
  }catch{
    capabilitiesKnown=true;capabilitiesLoadFailed=true;playbackSupported=false;panelReady=false;
    $('#connectionDot').className='dot off';$('#connectionText').textContent='実機未接続';
    $('#rigSummary').textContent='パネル設定を読み込めません。ログイン状態を確認して更新してください。';
    $('#rigEditor').hidden=true;renderDeployment();
  }
}
function describeFleetCapabilities(capabilities){
  const features=capabilities?.features;
  if(!features)return 'パネルの状態を取得できません。親機との接続を確認してください。';
  const labels=[];
  if(features.multiNodeSync===true&&features.safeStateTransactions===true) labels.push('複数台の停止・明るさ調整に対応');
  else labels.push('複数台の操作は未対応');
  labels.push(`配置設定 ${features.topologyDistribution===true?'利用可':'未確認'}`);
  labels.push(`データ送信 ${features.packageDistribution===true?'利用可':'未確認'}`);
  labels.push(`複数台で同時表示 ${features.playback===true?'利用可':'未対応'}`);
  if(capabilities.panelOutput==='dim_nightlight')labels.push('パネルは弱く点灯中');
  else if(capabilities.panelOutput==='diagnostic_rgb_bars')labels.push('点灯確認中');
  else if(capabilities.panelReady===false)labels.push('パネルの表示確認が必要');
  return labels.join(' · ');
}
async function saveRig(){
  if(!rigEnvelope?.rig||rigSaving)return;
  const rig=structuredClone(rigEnvelope.rig),nodesById=new Map(rig.nodes.map(node=>[String(node.nodeId),node]));
  for(const node of rig.nodes){
    const label=$(`[data-rig-label="${node.nodeId}"]`),enabled=$(`[data-rig-enabled="${node.nodeId}"]`),rotation=$(`[data-rig-rotation="${node.nodeId}"]`),brightness=$(`[data-rig-brightness="${node.nodeId}"]`);
    node.label=label.value.trim();node.enabled=node.role==='master'?true:enabled.checked;node.rotation=Number(rotation.value);node.brightnessCap=Number(brightness.value);
    if(!node.label||![0,180].includes(node.rotation)||!Number.isInteger(node.brightnessCap)||node.brightnessCap<0||node.brightnessCap>255)return toast(`パネル ${node.nodeId} の設定を確認してください`);
  }
  const priorLayouts=new Map(rig.layouts.map(layout=>[layout.layoutId,layout]));
  for(const layout of project.layouts){
    const bindings=[],used=new Set();
    for(const slot of layout.slots){
      const selection=$(`[data-rig-binding="${layout.id}|${slot.id}"]`),nodeId=selection.value;
      if(!nodeId)continue;
      if(used.has(nodeId))return toast(`${layout.name}: 同じパネルを重ねて割り当てられません`);
      const node=nodesById.get(nodeId);if(!node?.enabled)return toast(`${layout.name}: 使用するパネルを選んでください`);
      used.add(nodeId);bindings.push({slotId:slot.id,nodeId:Number(nodeId)});
    }
    priorLayouts.set(layout.id,{layoutId:layout.id,bindings});
  }
  rig.layouts=[...priorLayouts.values()];
  rigSaving=true;$('#rigEditor').setAttribute('aria-busy','true');renderRigEditor();
  try{
    await authReady;if(!controlCsrf)throw new Error('ログインしてください');
    const response=await fetch('/api/v2/rig',{method:'PUT',credentials:'same-origin',cache:'no-store',headers:{'Content-Type':'application/json','X-CSRF-Token':controlCsrf},body:JSON.stringify({requestId:randomUuid(),expectedRevision:controlRevision,expectedTopologyRevision:rigEnvelope.rigRevision,rig})});
    const result=await response.json();if(!response.ok)throw new Error(apiError(result,'設定を保存できません'));
    controlRevision=result.acceptedRevision;toast('設定を保存しました。別のパネルがある場合は接続を確認してください');
    await renderDevices();
    watchTopologySync();
  }catch(error){toast(`パネル設定: ${error.message}`)}
  finally{rigSaving=false;$('#rigEditor').removeAttribute('aria-busy');renderRigEditor()}
}
function drawLive(){draw($('#liveCanvas'));const c=project.cues[selectedCue];$('#liveCurrent').textContent=c?c.name:'停止中';$('#liveDetail').textContent=activePackageHash?'本体に保存済み / 消灯中':candidateHash?'本体へ送信済み / 表示への反映待ち':'本体へ未送信 / 消灯中'}
