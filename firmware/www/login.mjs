import { randomUuid } from '/packages/formats/browser.mjs';

const $ = id => document.getElementById(id);
const setTab = id => { for (const panel of ['login', 'setup', 'status']) $(panel).hidden = panel !== id; };
let revision = 0;
const api = async (path, options = {}) => {
  const response = await fetch(`/api/v2${path}`, { cache: 'no-store', credentials: 'same-origin', ...options });
  if (!response.ok) {
    const body = await response.text();
    let code = '';
    try { code = JSON.parse(body).error || ''; } catch (_) {}
    const messages = {
      INVALID_CREDENTIALS: '管理パスワードが違います。もう一度確認してください。',
      LOGIN_RATE_LIMIT: 'ログイン試行が続いたため一時停止中です。1分待ってから再試行してください。',
      UNAUTHENTICATED: 'ログイン状態を確認できません。もう一度ログインしてください。',
      ORIGIN_FORBIDDEN: '接続元を確認できません。パネルのWi-Fiから開き直してください。',
      UNPROVISIONED: '機器の初期設定が完了していません。',
    };
    throw new Error(messages[code] || code || `HTTP ${response.status}: ${body.slice(0, 160) || response.statusText}`);
  }
  return response.status === 204 ? null : response.json();
};
async function refresh({ reportStatusError = false } = {}) {
  const health = await api('/health');
  const setup = await api('/provisioning');
  $('connectionText').textContent = health.ok ? '機器に接続中' : '接続できません';
  if (!setup.provisioned) { $('tabLogin').hidden = true; $('logout').hidden = true; setTab('setup'); return; }
  try {
    const status = await api('/status');
    revision = status.revision;
    const panelLabel = status.panelOutput === 'dim_nightlight' ? '弱く点灯中' :
      status.panelOutput === 'diagnostic_rgb_bars' ? '点灯テスト中' :
      status.panelOutput === 'content_playback' ? '表示中' : '消灯中';
    $('connectionText').textContent = 'パネル' + panelLabel;
    $('tabLogin').hidden = true; $('logout').hidden = false;
    $('deviceState').textContent = '親機 ' + status.nodes[0].nodeId + ' · ' + panelLabel;
    setTab('status');
    $('loginError').textContent = '';
    return true;
  } catch (error) {
    $('tabLogin').hidden = false; $('logout').hidden = true;
    setTab('login');
    if (reportStatusError) $('loginError').textContent = `ログイン後の状態取得に失敗しました: ${error.message}`;
    return false;
  }
}
$('loginForm').addEventListener('submit', async event => {
  event.preventDefault(); $('loginError').textContent = '';
  const button = $('loginForm').querySelector('button[type="submit"]');
  button.disabled = true; button.textContent = '認証中…';
  $('loginError').textContent = '認証しています。応答までしばらくお待ちください。';
  try {
    const auth = await api('/auth/login', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({password: $('password').value})});
    window.portalCsrf = auth.csrfToken;
    $('password').value = '';
    await refresh({reportStatusError: true});
  }
  catch (error) { $('loginError').textContent = error.message; }
  finally { button.disabled = false; button.textContent = 'ログイン'; }
});
const loginButton = $('loginForm').querySelector('button[type="submit"]');
loginButton.disabled = false;
loginButton.textContent = 'ログイン';
$('loginError').textContent = '';
$('logout').onclick = async () => { try { await api('/auth/logout', {method: 'POST', headers: {'Content-Type': 'application/json','X-CSRF-Token':window.portalCsrf||''},body:'{}'}); } finally { location.reload(); } };
    $('blackout').onclick = async () => { try { const response=await api('/commands', {method: 'POST', headers: {'Content-Type': 'application/json', 'X-CSRF-Token': window.portalCsrf || ''}, body: JSON.stringify({requestId: randomUuid(), expectedRevision: revision, op: 'blackout', args: {enabled: true}})});revision=response.acceptedRevision;$('connectionText').textContent = '消灯中'; } catch (error) { $('connectionText').textContent = error.message; } };
$('refresh').onclick = () => refresh().catch(error => { $('deviceState').textContent = error.message; });
refresh().catch(error => { $('connectionText').textContent = error.message; setTab('setup'); });
