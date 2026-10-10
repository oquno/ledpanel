import test, { assert } from './harness.mjs';
import { unzipStore } from '/packages/formats/zip.mjs';
import { decodeLpk } from '/packages/formats/index.mjs';
import { ensureDisplayLayouts, selectDisplayLayout } from '/apps/editor/animation.mjs';
import { createBlankProject } from '/packages/formats/browser.mjs';
import { extractVideo, decodeImage, scaleImage } from '/apps/editor/media.mjs';
import { canActivateCandidate, canPlayActive, canRunPanelTest, deploymentScopeMessage, publishSequence } from '/apps/editor/deployment.mjs';

async function waitFor(check){const deadline=performance.now()+10000;while(performance.now()<deadline){if(check())return;await new Promise(r=>setTimeout(r,10))}throw new Error('Operation timed out')}
async function openAdvanced($,click){if(!$('#advancedDeployment').open)await click('#advancedDeployment summary')}
async function editor(run){
  const frame=document.createElement('iframe'),pageLoaded=new Promise(resolve=>frame.addEventListener('load',resolve,{once:true}));
  frame.src='/';document.body.append(frame);await pageLoaded;
  const win=frame.contentWindow;
  const doc=frame.contentDocument,$=s=>doc.querySelector(s),downloads=[];
  win.HTMLAnchorElement.prototype.click=function(){downloads.push({url:this.href,name:this.download})};
  const idle=()=>waitFor(()=>!$('#design').hasAttribute('aria-busy') && !$('#rigEditor').hasAttribute('aria-busy') && $('#publishProject').textContent!=='処理中…');
  const click=async s=>{$(s).click();await idle()};
  const change=async(s,value)=>{$(s).value=value;$(s).dispatchEvent(new win.Event('change',{bubbles:true}));await idle()};
  const upload=async(s,file)=>{const data=new win.DataTransfer();data.items.add(file);$(s).files=data.files;$(s).dispatchEvent(new win.Event('change',{bubbles:true}));await idle()};
  const save=async()=>{await click('#saveProject');const response=await fetch(downloads.at(-1).url);const bytes=new Uint8Array(await response.arrayBuffer());const entries=unzipStore(bytes);return {bytes,project:JSON.parse(new TextDecoder().decode(entries.find(e=>e.name==='project.json').data))}};
  try{await run({doc,win,$,click,change,upload,save,downloads,idle})}finally{frame.remove()}
}

test('editor warns before reloading after the device firmware restarts',()=>{
  return editor(async({$,win})=>{
    await new Promise(resolve=>setTimeout(resolve,100));
    const nativeFetch=win.fetch.bind(win);
    win.fetch=(input,init)=>{
      const url=new URL(typeof input==='string'?input:input.url,win.location.href);
      if(url.pathname==='/api/v2/health')return Promise.resolve(new Response(JSON.stringify({ok:true,bootId:'boot-after-reflash'}),{status:200,headers:{'Content-Type':'application/json'}}));
      return nativeFetch(input,init);
    };
    win.dispatchEvent(new win.Event('online'));
    await waitFor(()=>!$('#firmwareRestartNotice').hidden);
    assert.ok($('#firmwareRestartNotice').textContent.includes('保存してから再読み込み'));
    assert.equal($('#reloadAfterRestart').textContent,'再読み込み');
  });
});

test('failed device command shows its error and refreshes the control revision',()=>editor(async({$,win})=>{
  await waitFor(()=>!$('#maintenanceToggle').disabled);
  const nativeFetch=win.fetch.bind(win);let refreshed=false,commandCalls=0,secondRequest=null;
  win.fetch=(input,init={})=>{
    const url=new URL(typeof input==='string'?input:input.url,win.location.href);
    if(url.pathname==='/api/v2/commands'){
      if(++commandCalls===1)return Promise.resolve(new Response(JSON.stringify({error:'PEER_NOT_READY'}),{status:503,headers:{'Content-Type':'application/json'}}));
      secondRequest=JSON.parse(init.body);
      return Promise.resolve(new Response(JSON.stringify({acceptedRevision:43,status:'applied'}),{status:200,headers:{'Content-Type':'application/json'}}));
    }
    if(url.pathname==='/api/v2/status'){
      refreshed=true;
      return Promise.resolve(new Response(JSON.stringify({revision:42}),{status:200,headers:{'Content-Type':'application/json'}}));
    }
    return nativeFetch(input,init);
  };
  $('#brightness').value='32';$('#brightness').dispatchEvent(new win.Event('change',{bubbles:true}));
  await waitFor(()=>refreshed&&$('#toast').textContent.includes('PEER_NOT_READY'));
  assert.ok(refreshed);assert.ok($('#toast').textContent.includes('PEER_NOT_READY'));
  $('#brightness').value='40';$('#brightness').dispatchEvent(new win.Event('change',{bubbles:true}));
  await waitFor(()=>secondRequest!==null);
  assert.equal(secondRequest.expectedRevision,42);assert.ok($('#toast').textContent.includes('適用しました'));
}));

test('text color updates pixels, survives save/reload, and compiles raw pixels',()=>editor(async({$,win,click,change,save,upload})=>{
  await change('#layoutSelect','horizontal');
  await click('#newText');await change('[data-p="color"]','#ff0000');
  const c=$('#canvas'),data=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
  let lit=0;for(let i=0;i<data.length;i+=4){if(data[i]>0)lit++;assert.equal(data[i+1],0);assert.equal(data[i+2],0)}assert.ok(lit>0);
  const saved=await save();assert.equal(saved.project.scenes[0].variants.horizontal.layers[0].color,'#FF0000');
  await click('#deleteLayer');await upload('#projectFile',new File([saved.bytes],'roundtrip.ledproj'));
  assert.equal($('#layerList').children.length,1);
  await click('#compile');await waitFor(()=>win.lastLpk);
  const result=await decodeLpk(win.lastLpk),asset=result.manifest.assets[0],layer=result.manifest.scenes[0].variants.horizontal.layers[0];
  assert.equal(layer.kind,'sprite');assert.equal(layer.text,undefined);assert.equal(layer.name,undefined);assert.equal(asset.length,asset.width*asset.height*3);
  assert.ok(result.blob.some((n,i)=>i%3===1&&n>0));
}));

test('image resize uses original, duplicate assets remain independent, undo and redo work',()=>editor(async({$,win,click,change,upload,save})=>{
  await change('#layoutSelect','horizontal');
  const c=document.createElement('canvas');c.width=128;c.height=64;const ctx=c.getContext('2d');ctx.fillStyle='#00ff00';ctx.fillRect(0,0,128,64);
  const blob=await new Promise(r=>c.toBlob(r));await upload('#imageFile',new File([blob],'image.png',{type:'image/png'}));
  assert.equal($('#imageWidth').value,'64');assert.equal($('#imageHeight').value,'32');
  $('#imageWidth').value='16';$('#imageWidth').dispatchEvent(new win.Event('input'));await click('[data-action="resize"]');
  assert.equal($('#imageHeight').value,'8');await click('[data-action="duplicate"]');await click('[data-action="original"]');
  const {project}=await save(),layers=project.scenes[0].variants.horizontal.layers;
  assert.equal(project.assets.find(a=>a.id===layers[0].assetId).width,16);assert.equal(project.assets.find(a=>a.id===layers[1].assetId).width,128);
  await click('#undo');assert.equal($('#imageWidth').value,'16');await click('#redo');assert.equal($('#imageWidth').value,'128');
}));

test('layout selection persists and animation tracks survive project roundtrip',()=>editor(async({$,click,change,save,upload})=>{
  await change('#layoutSelect','vertical');await click('#newRect');assert.equal($('#layoutSelect').value,'vertical');assert.equal($('#canvas').height,64);
  $('#motion').value='up';$('#period').value='60';await click('[data-action="motion"]');
  $('#effect').value='glow';await click('[data-action="effect"]');
  const before=await save(),l=before.project.scenes[0].variants.vertical.layers[0];assert.ok(l.tracks.y.length);assert.ok(l.tracks.opacity.length);
  await upload('#projectFile',new File([before.bytes],'animation.ledproj'));await change('#layoutSelect','vertical');
  const after=await save();assert.deepEqual(after.project.scenes[0].variants.vertical.layers[0].tracks,l.tracks);
  const inspector=$('#inspectorBody');await click('#layerList [data-layer]');const name=$('[data-p="name"]');await click('#play');await new Promise(r=>setTimeout(r,100));assert.equal($('[data-p="name"]'),name);await click('#stop');
}));

test('failed project import retains existing edits',()=>editor(async({click,upload,save})=>{
  await click('#newRect');const before=await save();await upload('#projectFile',new File(['broken'],'bad.ledproj'));const after=await save();assert.deepEqual(after.project,before.project);
}));

test('device panel explains how to add another panel in plain language',()=>editor(async({$,click})=>{
  assert.equal($('#panelConnectionInfo').open,false);await click('#addDevice');
  assert.ok($('#panelConnectionInfo').open);
  const guide=$('#panelConnectionInfo').textContent;
  assert.ok(guide.includes('同じWi-Fiにつなぎます'));
  assert.ok(guide.includes('pair-follower'));
  assert.ok(guide.includes('BOOTボタンを各3秒押します'));
  await waitFor(()=>$('#fleetCapabilityStatus').textContent.includes('複数台の操作は未対応'));
  assert.ok($('#fleetCapabilityStatus').textContent.includes('配置設定 未確認'));
  assert.ok($('#fleetCapabilityStatus').textContent.includes('データ送信 未確認'));
  assert.ok($('#fleetCapabilityStatus').textContent.includes('複数台で同時表示 未対応'));
  await waitFor(()=>$('#networkModeStatus').textContent.includes('内蔵AP'));
  await waitFor(()=>$('#deviceList').textContent.includes('PANEL_PROFILE_UNVERIFIED'));
  assert.ok($('#deviceList').textContent.includes('PANEL_PROFILE_UNVERIFIED'));
  assert.ok($('#deviceList').textContent.includes('使用中データ: なし · 送信待ち: なし'));
}));

test('rig editor saves a local node-slot draft and reports pending topology sync',()=>editor(async({$,click})=>{
  await openAdvanced($,click);
  await waitFor(()=>!$('#maintenanceToggle').disabled);
  await click('#maintenanceToggle');
  await waitFor(()=>$('#maintenanceToggle').textContent==='更新準備を終える');
  await click('.tabs button[data-tab="devices"]');
  await waitFor(()=>!$('#rigEditor').hidden);
  assert.equal($('#saveRig').disabled,false);
  const binding=$('[data-rig-binding="horizontal|panel_2"]');
  binding.value='12';
  await click('#saveRig');
  await waitFor(()=>$('[data-rig-binding="horizontal|panel_2"]').value==='12');
  assert.ok($('#rigSummary').textContent.includes('未確認'));
  assert.equal($('[data-rig-binding="horizontal|panel_2"]').value,'12');
  await click('#maintenanceToggle');
  await waitFor(()=>$('#maintenanceToggle').textContent==='更新の準備');
}));

test('package transfer requires maintenance mode and a generated LPK',()=>editor(async({$,click})=>{
  await openAdvanced($,click);
  await waitFor(()=>!$('#maintenanceToggle').disabled);
  await waitFor(()=>$('#maintenanceToggle').textContent==='更新の準備');
  assert.equal($('#maintenanceToggle').textContent,'更新の準備');
  assert.equal($('#deployCandidate').disabled,true);
  await click('#maintenanceToggle');
  await waitFor(()=>$('#maintenanceToggle').textContent==='更新準備を終える');
  assert.equal($('#deployCandidate').disabled,true);
  await click('#maintenanceToggle');
  await waitFor(()=>$('#maintenanceToggle').textContent==='更新の準備');
  assert.equal($('#deployCandidate').disabled,true);
}));

test('a single-node Master can activate while an unsynchronized follower blocks activation',()=>{
  assert.equal(canActivateCandidate({maintenanceMode:true,maintenanceScope:'single_node',candidateHash:'a'.repeat(64),csrf:'token',busy:false}),true);
  assert.equal(canActivateCandidate({maintenanceMode:true,maintenanceScope:'single_node',candidateHash:'a'.repeat(64),csrf:'token',busy:false,topologyDistributionSupported:true,topologySynchronized:false}),false);
  assert.equal(canActivateCandidate({maintenanceMode:true,maintenanceScope:'single_node',candidateHash:'a'.repeat(64),csrf:'token',busy:false,topologyDistributionSupported:true,topologySynchronized:true}),true);
  assert.ok(deploymentScopeMessage(true,'single_node').includes('1台だけ'));
  assert.equal(canActivateCandidate({maintenanceMode:true,maintenanceScope:'local_only',candidateHash:'a'.repeat(64),csrf:'token',busy:false}),false);
  assert.ok(deploymentScopeMessage(true,'local_only').includes('反映を待っています'));
  assert.equal(canActivateCandidate({maintenanceMode:false,maintenanceScope:'single_node',candidateHash:'a'.repeat(64),csrf:'token',busy:false}),false);
  assert.equal(canActivateCandidate({maintenanceMode:true,maintenanceScope:'follower',candidateHash:'a'.repeat(64),csrf:'token',busy:false}),false);
});

test('active content can play while the project still has unsaved edits',()=>{
  assert.equal(canPlayActive({playbackSupported:true,activeHash:'a'.repeat(64),editorHash:'a'.repeat(64),busy:false}),true);
  assert.equal(canPlayActive({playbackSupported:true,activeHash:'a'.repeat(64),editorHash:'b'.repeat(64),busy:false}),false);
  assert.equal(canPlayActive({playbackSupported:false,activeHash:'a'.repeat(64),editorHash:'a'.repeat(64),busy:false}),false);
  assert.equal(canPlayActive({playbackSupported:true,activeHash:'a'.repeat(64),editorHash:'a'.repeat(64),busy:true}),false);
});

test('one display action stops, safely replaces, and starts the chosen content',async()=>{
  let state={mode:'cue',running:true,blackout:false};
  let maintenance=false;
  const calls=[];
  const readStatus=async()=>{calls.push('read');return {state:{...state},maintenance,maintenanceScope:'single_node'}};
  const command=async(op,args)=>{
    calls.push(op==='blackout'?`blackout:${args.enabled}`:op);
    if(op==='stop')state={mode:'stopped',running:false,blackout:true};
    if(op==='go')state={mode:'cue',running:true,blackout:true};
    if(op==='blackout')state.blackout=args.enabled;
  };
  const result=await publishSequence({
    readStatus,command,
    enterMaintenance:async()=>{calls.push('enter');maintenance=true;return {maintenanceScope:'single_node'}},
    sendPackage:async()=>{calls.push('send');return {packageHash:'new-hash',maintenanceScope:'single_node'}},
    waitForPeers:async()=>calls.push('wait'),
    activatePackage:async()=>calls.push('activate'),
    exitMaintenance:async()=>{calls.push('exit');maintenance=false},
    cueId:'cue-1',
  });
  assert.deepEqual(calls,['read','stop','read','enter','send','wait','activate','exit','go','blackout:false','read']);
  assert.equal(result.status.state.running,true);
  assert.equal(result.status.state.blackout,false);
  assert.equal(result.packageHash,'new-hash');
});

test('failed display update restores the previous project and keeps its brightness',async()=>{
  let state={mode:'cue',running:true,blackout:false,cueId:'old-cue',sceneId:'',userBrightness:19};
  let maintenance=false;
  const command=async(op,args={})=>{
    if(op==='stop')state={...state,mode:'stopped',running:false,blackout:true,cueId:'',sceneId:''};
    if(op==='go')state={...state,mode:'cue',running:true,blackout:false,cueId:args.cueId};
    if(op==='pause')state={...state,running:false};
    if(op==='play_scene')state={...state,mode:'scene',running:true,blackout:false,sceneId:args.sceneId,cueId:''};
    if(op==='blackout')state={...state,blackout:args.enabled};
  };
  await assert.rejects(()=>publishSequence({
    readStatus:async()=>({state:{...state},maintenance,maintenanceScope:'single_node'}),
    command,
    enterMaintenance:async()=>{maintenance=true;return {maintenanceScope:'single_node'}},
    sendPackage:async()=>{throw new Error('transfer failed')},
    activatePackage:async()=>{},
    exitMaintenance:async()=>{maintenance=false},
    cueId:'new-cue',
  }));
  assert.equal(maintenance,false);
  assert.equal(state.running,true);
  assert.equal(state.cueId,'old-cue');
  assert.equal(state.blackout,false);
  assert.equal(state.userBrightness,19);
});

test('display button states the hardware reason when the panel cannot light',()=>editor(async({$})=>{
  await waitFor(()=>$('#deploymentStatus').textContent.includes('パネルの表示方式'));
  assert.equal($('#publishProject').disabled,true);
  assert.ok($('#deploymentStatus').textContent.includes('機器タブで接続と設定を確認'));
  assert.equal($('#advancedDeployment')?.open??false,false);
}));

test('low nightlight is reported truthfully and can be stopped',()=>editor(async({$,win})=>{
  const nativeFetch=win.fetch.bind(win);
  win.fetch=async(input,init)=>{
    const response=await nativeFetch(input,init);
    const url=new URL(typeof input==='string'?input:input.url,win.location.href);
    if(url.pathname!=='/api/v2/status')return response;
    const body=await response.json();body.panelOutput='dim_nightlight';
    return new win.Response(JSON.stringify(body),{status:response.status,headers:response.headers});
  };
  await waitFor(()=>$('#deviceState').textContent==='弱く点灯中');
  assert.equal($('#stopLive').disabled,false);
  assert.equal($('#stopLive').textContent,'停止して消灯');
  assert.equal($('#liveCurrent').textContent,'低照度で点灯中');
  assert.ok($('#liveHint').textContent.includes('そのまま切り替わります'));
}));

test('panel test requires a supported Master in stopped blackout outside maintenance',()=>{
  const safe={csrf:'token',busy:false,supported:true,role:'master',mode:'stopped',running:false,blackout:true,maintenance:false};
  assert.equal(canRunPanelTest(safe),true);
  for(const change of [{csrf:''},{busy:true},{supported:false},{role:'follower'},{mode:'playing'},{running:true},{blackout:false},{maintenance:true}])
    assert.equal(canRunPanelTest({...safe,...change}),false);
});

test('device page exposes the gated full-panel RGB test',()=>editor(async({$})=>{
  await waitFor(()=>$('#panelTestButton')&&$('#panelTestStatus').textContent.includes('消灯'));
  assert.equal($('#panelTestButton').textContent,'点灯を確認（30秒）');
  assert.ok($('#panelTestStatus').textContent.includes('消灯'));
  assert.equal($('#panelTestButton').disabled,true);
  assert.ok($('#panelConnectionInfo').textContent.includes('別のパネルを追加する'));
  assert.ok($('#panelConnectionInfo').textContent.includes('PCで2台ともUSB接続として認識'));
}));

test('generated package stages and activates while output stays dark',()=>editor(async({$,win,click})=>{
  await openAdvanced($,click);
  await click('#compile');
  await waitFor(()=>win.lastLpk?.byteLength>0);
  await waitFor(()=>!$('#maintenanceToggle').disabled);
  await click('#maintenanceToggle');
  await waitFor(()=>$('#maintenanceToggle').textContent==='更新準備を終える');
  await waitFor(()=>!$('#deployCandidate').disabled);
  await click('#deployCandidate');
  await waitFor(()=>!$('#activateCandidate').disabled);
  await click('#activateCandidate');
  await waitFor(()=>$('#activateCandidate').disabled);
  await waitFor(()=>!$('#maintenanceToggle').disabled);
  assert.ok($('#deploymentStatus').textContent.includes('パネルの表示方式'));
  await click('#maintenanceToggle');
  await waitFor(()=>$('#maintenanceToggle').textContent==='更新の準備');
}));

test('editor preview starts blink and scrolling when their settings are selected',()=>editor(async({$,win,click,save,upload})=>{
  assert.ok($('#design').textContent.includes('この画面で動きも確認できます'));
  const litPixels=()=>{
    const canvas=$('#canvas'),data=canvas.getContext('2d').getImageData(0,0,canvas.width,canvas.height).data;
    let count=0;for(let i=0;i<data.length;i+=4)if(data[i]+data[i+1]+data[i+2]>0)count++;
    return count;
  };
  const liveLitPixels=()=>{const canvas=$('#liveCanvas'),data=canvas.getContext('2d').getImageData(0,0,canvas.width,canvas.height).data;let count=0;for(let i=0;i<data.length;i+=4)if(data[i]+data[i+1]+data[i+2]>0)count++;return count};
  const seek=tick=>{$('#scrub').value=String(tick);$('#scrub').dispatchEvent(new win.Event('input',{bubbles:true}))};
  await click('#newRect');$('#period').value='30';$('#effect').value='blink';$('#effect').dispatchEvent(new win.Event('change',{bubbles:true}));await new Promise(r=>setTimeout(r,25));
  assert.ok(litPixels()>0);assert.ok(liveLitPixels()>0);assert.ok(/一時停止/.test($('#play').textContent));await waitFor(()=>litPixels()===0&&liveLitPixels()===0);
  const saved=await save();await upload('#projectFile',new File([saved.bytes],'blink.ledproj'));seek(0);
  assert.ok(/一時停止/.test($('#play').textContent));assert.ok(litPixels()>0);await waitFor(()=>litPixels()===0);
  await click('#layerList [data-layer]');assert.ok(/一時停止/.test($('#play').textContent));assert.ok(litPixels()>0);await waitFor(()=>litPixels()===0);
  await click('#play');assert.ok(/再生/.test($('#play').textContent));seek(0);assert.ok(litPixels()>0);seek(15);assert.equal(litPixels(),0);
  seek(0);await click('#play');await waitFor(()=>Number($('#scrub').value)>=15);assert.equal(litPixels(),0);await click('#stop');
  await click('#layerList [data-layer]');assert.ok(/一時停止/.test($('#play').textContent));assert.ok(litPixels()>0);await waitFor(()=>litPixels()===0);await click('#stop');
  $('#effect').value='none';await click('[data-action="effect"]');$('#motion').value='left';await click('[data-action="motion"]');
  assert.ok(/一時停止/.test($('#play').textContent));seek(0);assert.equal(litPixels(),0);await waitFor(()=>litPixels()>0);
}));

test('loading a scene with delayed animation starts at the first animated layer',()=>editor(async({$,win,click,change,save,upload})=>{
  await click('#newRect');await change('[data-p="startTick"]','60');$('#period').value='120';
  await change('#effect','blink');
  const saved=await save();await upload('#projectFile',new File([saved.bytes],'delayed-blink.ledproj'));
  assert.ok(Number($('#scrub').value)>=60&&Number($('#scrub').value)<120);
  assert.ok(/一時停止/.test($('#play').textContent));
  $('#scrub').value='60';$('#scrub').dispatchEvent(new win.Event('input',{bubbles:true}));
  const pixels=$('#canvas').getContext('2d').getImageData(0,0,$('#canvas').width,$('#canvas').height).data;
  assert.ok([...pixels].some((channel,index)=>index%4!==3&&channel>0));
}));

test('video extraction decodes real frames, quantizes timing, and supports cancellation',async()=>{
  const file=new File([await (await fetch('/tests/browser/video-fixture.webm')).arrayBuffer()],'fixture.webm',{type:'video/webm'});
  const frames=await extractVideo(file,{width:32,height:32},{duration:1,fps:10});
  assert.equal(frames.length,10);assert.equal(frames.reduce((n,f)=>n+f.ticks,0),30);assert.equal(frames[0].width,32);assert.equal(frames[0].height,16);
  for(const [frame,channel] of [[frames[0],0],[frames[8],2]]){const img=await decodeImage(new Blob([frame.data]));const pixel=scaleImage(img,1,1).getContext('2d').getImageData(0,0,1,1).data;assert.ok(pixel[channel]>200)}
  const controller=new AbortController();controller.abort();await assert.rejects(()=>extractVideo(file,{width:32,height:32},{signal:controller.signal}));
});

test('video import saves sequence frames, reloads and compiles them',()=>editor(async({$,win,click,change,upload,save})=>{
  await change('#layoutSelect','horizontal');
  await click('#importVideo');const file=new File([await (await fetch('/tests/browser/video-fixture.webm')).arrayBuffer()],'fixture.webm',{type:'video/webm'});
  await upload('#videoFile',file);$('#videoDuration').value='1';await click('#startVideo');
  const before=await save(),l=before.project.scenes[0].variants.horizontal.layers[0];assert.equal(l.kind,'sequence');assert.equal(l.frames.length,10);
  await upload('#projectFile',new File([before.bytes],'video.ledproj'));const after=await save();assert.deepEqual(after.project,before.project);
  await click('#compile');await waitFor(()=>win.lastLpk);const runtime=await decodeLpk(win.lastLpk);assert.equal(runtime.manifest.scenes[0].variants.horizontal.layers[0].kind,'sequence');assert.equal(runtime.manifest.assets.length,10);
}));
test('editor loads its modules and supports rectangle add/delete', async () => {
  const frame = document.createElement('iframe');
  frame.src = '/';
  const loaded = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Editor load timeout')), 5000);
    frame.onload = () => { clearTimeout(timer); resolve(); };
  });
  document.body.append(frame);
  try {
    await loaded;
    const doc = frame.contentDocument;
    assert.ok(doc.querySelector('#sceneList [data-scene]'));
    doc.querySelector('#newRect').click();
    await new Promise(r=>setTimeout(r,20));
    assert.equal(doc.querySelectorAll('#layerList [data-layer]').length, 1);
    doc.querySelector('#deleteLayer').click();
    await new Promise(r=>setTimeout(r,20));
    assert.equal(doc.querySelectorAll('#layerList [data-layer]').length, 0);
  } finally { frame.remove(); }
});


test('layout choice copies an empty design and persists the transmitted layout',()=>editor(async({$,click,change,save,upload,win})=>{
  await click('#newRect');
  const original=(await save()).project;
  await change('#layoutSelect','vertical');
  const vertical=await save();
  assert.equal(vertical.project.defaultLayout,'vertical');
  assert.equal(vertical.project.scenes[0].variants.vertical.layers.length,1);
  assert.equal($('#canvas').height,64);
  await change('#layoutSelect','horizontal');
  await change('#layoutSelect','vertical');
  assert.deepEqual((await save()).project.scenes[0].variants.vertical,vertical.project.scenes[0].variants.vertical);
  await upload('#projectFile',new win.File([vertical.bytes],'vertical.ledproj'));
  assert.equal($('#layoutSelect').value,'vertical');
  assert.deepEqual((await save()).project.scenes[0].variants.panel_64x32,original.scenes[0].variants.panel_64x32);
  await click('#compile');
  await waitFor(()=>win.lastLpk);assert.equal((await decodeLpk(win.lastLpk)).manifest.defaultLayout,'vertical');
}));


test('a horizontal-only imported project gains vertical and keeps its original design',()=>{
  const project=createBlankProject();
  project.layouts=project.layouts.filter(layout=>layout.id==='horizontal');
  project.defaultLayout='horizontal';
  project.scenes=[{variants:{horizontal:{layers:[{id:'test',kind:'rect',x:128,y:31,width:8,height:1,tracks:{x:[{tick:0,value:128},{tick:60,value:-8}]}}]}}}];
  ensureDisplayLayouts(project,createBlankProject().layouts);
  selectDisplayLayout(project,'vertical');
  const layer=project.scenes[0].variants.vertical.layers[0];
  assert.equal(layer.x,64);assert.equal(layer.y,62);
  assert.equal(layer.tracks.x[0].value,64);assert.equal(layer.tracks.x[1].value,-8);
  assert.equal(project.scenes[0].variants.horizontal.layers[0].x,128);
});
