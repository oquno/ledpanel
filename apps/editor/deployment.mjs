export function canActivateCandidate({
  maintenanceMode,
  maintenanceScope,
  candidateHash,
  csrf,
  busy,
  topologyDistributionSupported,
  topologySynchronized,
}) {
  return maintenanceMode === true && maintenanceScope !== 'follower' &&
    maintenanceScope !== 'local_only' &&
    Boolean(candidateHash) && Boolean(csrf) && busy !== true &&
    (topologyDistributionSupported !== true || topologySynchronized === true);
}

export function canPlayActive({ playbackSupported, activeHash, editorHash, busy }) {
  return playbackSupported === true && Boolean(activeHash) &&
    activeHash === editorHash && busy !== true;
}

export async function publishSequence({
  readStatus,
  command,
  enterMaintenance,
  sendPackage,
  waitForPeers,
  activatePackage,
  exitMaintenance,
  cueId,
  onStep = () => {},
}) {
  let activated = false;
  let originalState = null;
  try {
    let status = await readStatus();
    originalState = { ...status.state };
    onStep('表示を止めて更新しています…');
    await command('stop');
    status = await readStatus();
    if (!status.state.blackout) {
      onStep('画面を更新できる状態にしています…');
      await command('blackout', { enabled: true });
      status = await readStatus();
    }
    let scope = status.maintenanceScope;
    if (!status.maintenance) {
      onStep('安全に更新する準備をしています…');
      const result = await enterMaintenance();
      scope = result.maintenanceScope || scope;
    }
    if (scope === 'local_only')
      throw new Error('追加パネルとの接続を確認できません。機器タブを開いてください。');
    if (scope === 'follower')
      throw new Error('親機の画面で操作してください。');
    onStep('本体へ送っています…');
    const staged = await sendPackage();
    if (staged.maintenanceScope === 'local_only')
      throw new Error('追加パネルとの接続を確認できません。機器タブを開いてください。');
    if (waitForPeers) await waitForPeers(staged.packageHash);
    onStep('表示内容を切り替えています…');
    await activatePackage(staged.packageHash);
    activated = true;
    onStep('再生を始めています…');
    await exitMaintenance();
    await command('go', { cueId });
    await command('blackout', { enabled: false });
    status = await readStatus();
    if (!status.state.running || status.state.blackout)
      throw new Error('本体は更新されましたが、表示が始まりませんでした。機器タブでパネル状態を確認してください。');
    return { status, packageHash: staged.packageHash };
  } catch (error) {
    error.activated = activated;
    try {
      let status = await readStatus();
      if (status.maintenance) {
        onStep('表示を戻しています…');
        await exitMaintenance();
        status = await readStatus();
      }
      if (activated) {
        await command('go', { cueId });
        await command('blackout', { enabled: false });
      } else if (originalState) {
        if (originalState.cueId) {
          await command('go', { cueId: originalState.cueId });
          if (!originalState.running) await command('pause');
        } else if (originalState.sceneId) {
          await command('play_scene', { sceneId: originalState.sceneId });
          if (!originalState.running) await command('pause');
        } else {
          await command('stop');
        }
        await command('blackout', { enabled: originalState.blackout === true });
      }
    } catch (restoreError) {
      error.restoreError = restoreError;
    }
    throw error;
  }
}

export function deploymentScopeMessage(maintenanceMode, maintenanceScope) {
  if (!maintenanceMode) return '送信前に「変更を準備」を押してください。';
  if (maintenanceScope === 'single_node')
    return 'この1台だけに反映します。';
  if (maintenanceScope === 'local_only')
    return 'もう1台との接続を確認できないため、反映を待っています。';
  if (maintenanceScope === 'follower')
    return 'この画面では変更できません。親機の画面を開いてください。';
  return 'ほかのパネルへ送っています。すべての確認後に反映します。';
}

export function canRunPanelTest({ csrf, busy, supported, role, mode, running, blackout, maintenance }) {
  return Boolean(csrf) && busy !== true && supported === true && role === 'master' &&
    mode === 'stopped' && running === false && blackout === true && maintenance === false;
}
