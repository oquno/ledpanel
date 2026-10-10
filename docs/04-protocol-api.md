# 04. 同期・API・セキュリティ

## 1. 権威・時刻・再生位置

本章のA/BはMasterと1台のFollower間の説明用表記。1–N台の参加集合、全員ACK、topology照合、ノード追加、外部APは08が規範。2台だけに分岐する実装は禁止。

設定されたMasterだけが本番の状態revisionを発行する。BrowserのDate.now、RTC、NTP、Followerの壁時計を再生時刻に使わない。Masterの単調増加microsecond時計を採用し、起動ごとにbootIdを変える。bootId変更は既存の予約をすべて無効にする。

時刻と64bit累積tickはJSONで10進文字列 `"0"`〜`"18446744073709551615"` とする。JSではBigInt、C++ではuint64_tで読み、算術はoverflowを検査する。JSON数値にして精度を失わない。revision/seqはuint32、0から開始しwrap前に新session/再接続へ移る。

running=trueの状態で、masterNow≥anchorUsなら `elapsedTicks=positionTicks+floor((masterNow-anchorUs)*30/1,000,000)`。running=falseならpositionTicksで停止。描画スケジュールは各tickの絶対時刻 `anchorUs+ceil(k*1,000,000/30)` から求め、33msの足し算を繰り返さない。

sceneモードは指定sceneを02のloop/holdで評価。cueモードではcueIdを開始キューとしてelapsedTicksを評価する。autoキューはholdTicksを消費して次へ進む。manualへ到達するとそこでsceneを無期限再生する。キューの配列末尾を越えたら出力黒、実効モードSTOPPED。周期内シーンtickとキュー滞在elapsedは混同しない。再接続時も同じ経過時間から同じキューを再構成できる。

NEXT/PREVは現在の実効キューの隣へ進み、位置0から再生。sceneモードやSTOPPEDからNEXTは先頭、PREVは末尾。末尾NEXTはSTOP、先頭PREVは先頭を再開始。GO(cueId)は対象キューの先頭から開始。pauseはキュー開始基準からの累積positionTicksを固定、resumeはそこから再開。STOPは位置0に戻し黒。blackoutは出力だけを黒にし、時間進行は継続。復帰で現在時点を表示する。

ユーザーの暗転と障害暗転は別ラッチ。暗転中のplay/GOは時間だけ開始し、明示 `blackout{enabled:false}` まで点灯しない。faultLatchは正常化後の `recover` で解除しREADY_BLACKOUTへ戻す。recoverだけで点灯しない。

通常操作の次stateを作るときは、採用時刻Tにおける旧stateの累積elapsedTicksを先に計算する。brightness/暗転解除/layoutはその値をpositionTicksへ置き、anchorUs=Tとして時間を継続する。pauseもT時点の値で固定する。resumeは保存positionTicksを維持。play_scene/GO/NEXT/PREVのみ位置0へ戻す。即時暗転はnowをTとして同じ規則で時間を維持する。これにより輝度調整でスクロールや自動キューが先頭へ戻らない。

## 2. 時計同期

Followerから1秒ごとにNTP型4時刻交換。t1=B送信、t2=A受信、t3=A送信、t4=B受信。offset=`((t2-t1)+(t3-t4))/2`（A-B）、RTT=`(t4-t1)-(t3-t2)`。全値をsigned64で計算し、負RTT、RTT>20ms、応答が500ms以上遅れた標本を棄却する。

起動時は100ms間隔で8回実施、直近8個の有効標本からRTTが小さい3個のoffset中央値を初期推定とする。最低3個揃うまでUNSYNCED。以後1Hzで更新し、offset差を最大1ms/秒でslew。表示中に時刻を後戻りさせない。差の絶対値>20msならUNSYNCEDにして暗転し、暗転中に再推定する。最後の有効標本から5秒でUNSYNCED。

時計の健康と接続の健康は別。パケット到達していても時刻が未確定なら再生不可。診断のoffset/RTTはソフトウェア推定値であり実LEDの走査位相差ではない。無線はhard realtimeを保証しない。

## 3. UDP wire形式

unicast UDP port=42175。全ノードが同じportをbindする。broadcast/multicastに秘密や同期を依存させない。パケット全長≤1200bytes。全数値フィールドはlittle endian。

| offset | 長さ | 内容 |
|---|---|---|
| 0 | 4 | ASCII `LPS2` |
| 4 | 1 | protocolMajor=2 |
| 5 | 1 | messageType |
| 6 | 2 | payloadLength≤1128 |
| 8 | 16 | groupId UUIDのバイト順（UUID表示のhexを左からdecode） |
| 24 | 4 | senderNodeId=1–65535（roleと独立） |
| 28 | 8 | senderBootId。CSPRNGで起動時に生成 |
| 36 | 4 | seq |
| 40 | payloadLength | UTF-8 JSON payload |
| 40+payloadLength | 32 | HMAC-SHA256(authenticationKey, header+payload) |

全長=72+payloadLength。authenticationKeyはtype1–4でgroupKey、type5以降で後述のsessionKey。HMACはconstant-time比較。検証前にJSONをparseしない。groupId、size、versionの安価な検査→HMAC→boot/seq→payload意味検証の順。失敗packetを大量にログ出力しない。

| type | 名称 | 必須payload |
|---:|---|---|
| 1 | HELLO | nonceB:32hex、protocolMajor:2、hardwareId:UUID、renderer、activeHash:hashまたはnull |
| 2 | OFFER | nonceB、nonceA:32hex、masterBootId:16hex、masterNodeId、followerNodeId |
| 3 | ACCEPT | nonceB、nonceA、masterBootId、masterNodeId、followerNodeId |
| 4 | SESSION_OK | nonceB、nonceA、masterBootId、masterNodeId、followerNodeId |
| 5 | TIME_REQ | exchangeId:uint32、t1Us:string |
| 6 | TIME_RESP | exchangeId、t1Us、t2Us、t3Us |
| 10 | HEARTBEAT | revision:uint32、topologyRevision:uint32、topologyHash:hash、activeHash:hash/null、nowUs:string、health:ready/unsynced/fault、faultCode:string/null、clockSynced、clockOffsetUs:signed decimal string、clockRttUs:uint32、clockSampleAgeMs:uint32/null |
| 11 | PREPARE | commandId:UUID、nextState:State |
| 12 | READY | commandId、revision、topologyRevision、topologyHash、ok:boolean、error:string/null |
| 13 | COMMIT | commandId、nextState:State |
| 14 | ACK | commandId、revision、topologyRevision、topologyHash、phase:committed/applied、ok、error |
| 15 | ABORT | commandId、revision |
| 16 | STATE | state:State |
| 17 | BLACKOUT | revision、reason:user/fault、nowUs |

hashは64桁小文字hex。nonceは16bytesのhex。bootIdは8bytesのhex表示で、headerのLE uint64を16桁数値表記したもの。規定以外のプロパティは拒否する。messageTypeごとの単体decoderとfuzzテストを用意する。

### 3.1 起動/再接続の認証

共有groupKeyはUSBで全対象ノードへ事前設定済み。Bが新しいnonceBでHELLO、AがnonceBを返し新nonceAを含むOFFER、Bが両nonceを返すACCEPT、AがSESSION_OK。すべてHMAC必須。Bは直近5秒以内の自分のnonceBへの返答だけを採用。Aも自分が発行したnonceAへのACCEPTだけを採用する。

完了後に `sessionKey=HMAC-SHA256(groupKey, ASCII("LPS2/session") || nonceBの16bytes || nonceAの16bytes || masterBootIdのLE8bytes || followerBootIdのLE8bytes)` を導出する。区切り文字やNULは追加しない。以後のUDPとnode HTTPはsessionKeyで認証し、groupKeyの直接使用はhandshakeに限定する。新session成立前は既存sessionを破棄しない。新session成立時には暗転し、相手bootIdとseqの64packet replay windowを登録。

古いboot/seq、同一packetの再送はアプリ処理へ通さない。アプリ上の再試行は新seq、同commandIdで送る。再接続には必ず新nonceを使用し、過去のsession packet再生で再開しない。同一bootで再接続してseqが0に戻ってもsessionKeyが変わるため旧packetを受理しない。通信元のnodeId/役割も固定照合し、BのSTATE/COMMIT/BLACKOUTをAの制御命令として採用しない。BのfaultはheartbeatでAへ通知する。

## 4. Stateの完全スナップショット

```json
{
  "revision": 12,
  "topologyRevision": 1,
  "topologyHash": "0000000000000000000000000000000000000000000000000000000000000000",
  "packageHash": "0000000000000000000000000000000000000000000000000000000000000000",
  "layoutId": "horizontal",
  "mode": "cue",
  "sceneId": null,
  "cueId": "song_01",
  "running": true,
  "positionTicks": "0",
  "anchorUs": "15000000",
  "effectiveAtUs": "15000000",
  "blackout": false,
  "userBrightness": 26
}
```

上のhashは説明用placeholderで実在コンテンツを指さない。modeはstopped/scene/cue。stoppedでは両ID=null、running=false、positionTicks="0"。sceneではsceneIdだけ非null、cueではcueIdだけ非null。packageHashは未配布のstoppedでのみnull可。layoutIdはパッケージ内layoutのid。stateにtopologyRevision（uint32）とtopologyHash（64hex）を必須とする。brightnessは0–255。effectiveAtUsが採用境界、anchorUsがpositionTicksに対応する時刻。通常コマンドでは両時刻を一致させる。

revisionはユーザー操作の採用ごとに増加。自然なautoキュー遷移でrevisionを増やす必要はなく、GET statusに実効キューとsceneTickを併記する。完全stateを保存し、scene番号だけの差分を送らない。

## 5. 通常コマンドの予約と失敗

1. Aが認証/編集権/expectedRevision/能力/hash一致/時計を検査。
2. Aが次stateを作り、実行時刻を少なくとも500ms先へ予約。未処理通常コマンドは1個まで。
3. PREPAREをBへ送り、100ms以内のREADYを待つ。Bはpackage/variant/参照/時刻を検証しRAMへ準備する。
4. READY後、COMMITを送る。再送間隔50ms。T-150msまでにcommitted ACKが来なければABORTと障害BLACKOUTを送信し、Aも暗転する。
5. ACK済みなら各ノードはTに採用し、Bはapplied ACK。Aのoperationは全対象の採用を確認してappliedになる。

未実行のABORTは予約を削除。重複commandIdは同じ結果を返し、二度NEXTしない。Tを50ms以上過ぎて届いたCOMMITは適用せず、LATE_COMMANDとして暗転/再同期する。

250msごとに双方向HEARTBEATとAの現在STATEを送る。予約がある間は現在STATEと予約COMMITを別packetで送る。古いrevisionのSTATEで予約を上書きしない。受信STATEは既にcommitを知る同一revisionなら整合確認、知らないrevisionなら暗転下で復旧用に保存し、いきなり点灯しない。

Wi-Fi上で複数台の完全な原子切替は保証できない。COMMIT/ACKの片方向消失などでは一時的な片側適用があり得る。本仕様は未来時刻予約と確認で通常の同期を実現し、不一致を検知したら全対象を暗転する。成功応答を受信できないことを「未実行」と断定しない。UIはoperation照会で結果を解決する。

BLACKOUTは通常予約を待たずに最優先でローカル適用し、新revisionで50ms間隔10回送る。予約済み操作を取り消す。毎250msのstateでも暗転を保持。新revisionより古いCOMMITで解除できない。

最後の正常HEARTBEATから1秒でPEER_LOST。Bは単独で暗転、AもBのack/heartbeat途絶で暗転する。5秒以内の有効時計がない場合も暗転。ネットワーク切断中の遠隔暗転は即時到達を保証せず、この1秒watchdogが上限の設計値となる。ブラウザー切断は暗転理由にしない。

## 6. パッケージ配布・activate

本番と配布の状態を分離する。アップロード前に `maintenance.enter` が必要で、全対象ノードをSTOPPED/blackoutにする。peer不通でメンテナンスを開始した場合、Aへのstageはできるが全対象activateは不可。

1. Browser→Aのinactive slotへLPKをstream。Content-LengthとX-Content-SHA256を必須とする。
2. AがFlashから独立検証し、candidateHashを記録。
3. BがAの候補を認証付きHTTPでpullし、inactive slotへ保存・再検証。Aは同時1転送、16KiB chunkで送る。
4. 全対象のcandidateHash一致でdeployment=ready。まだactiveは旧データ。
5. activate APIでA/BのNVS pointerを更新し、全対象のactiveHash一致を確認。新データでSTOPPED/blackoutへ。
6. メンテナンス終了後READY_BLACKOUT。本番点灯にはGO/Playと暗転解除が必要。

転送再開はv1なし。中断/timeout30秒でuploadをfailedにし、次回はinactive slotへ先頭から再送。activeを消さない。deployment状態が不明ならGET照会。A再起動で未完了jobはfailed/restart、実slotを検査し状態を再構成する。

## 7. Browser HTTP API共通

Base=`<Masterの現在origin>/api/v2`（内蔵APならhttp://192.168.4.1）、Content-Type=`application/json`（明記したbinary以外）。APIの応答はCache-Control:no-store。標準エラー:

```json
{"error":{"code":"REVISION_CONFLICT","message":"状態が更新されました","path":"/expectedRevision","requestId":"...","retryable":true}}
```

HTTP: 400形式不正、401未認証、403認可/CSRF、404存在なし、409状態競合、413容量超過、422意味検証不正、429頻度超過、503node未準備。JSON body上限16KiB。binary例外はLPK/OTA上限。追加引数は拒否。

requestIdはUUID。command/activate/OTA/maintenance等の変更はIdempotency-Keyにも同じ値を付ける。RAMで最近128件/10分を保持。同key/同bodyは元結果、同key/異bodyは409。再起動をまたぐkey保持は保証しないため、bootId変更時はstatusを照会し再実行をユーザーに提示する。

同期API変更は `{requestId,expectedRevision,...}`。expectedRevision不一致なら再取得し、操作を自動で繰り返さない。通常コマンド1個処理中なら409 BUSY。暗転はrevision競合でも優先受理する。

### 7.1 認証・状態・制御

| method/path | request | response / 意味 |
|---|---|---|
| GET /health | なし、未認証可 | `{ok,bootId,apiMajor:2}`のみ。識別秘密は出さない |
| POST /auth/login | `{password}` | 200 `{csrfToken,expiresInSec:43200}` + session cookie |
| POST /auth/logout | 空JSON | 204、session無効化 |
| POST /control/claim | `{requestId,force:false}` | `{leaseId,expiresInSec:30}` |
| POST /control/renew | `{leaseId}` | 30秒延長 |
| POST /control/release | `{leaseId}` | 204 |
| GET /capabilities | なし | profile、version、renderer、limits、layouts、audioInput:false、features（panelDiagnosticTestを含む） |
| GET /status | なし | bootId、revision、state、effectiveCueId、effectiveSceneId、sceneTick、nodes、maintenance、faultLatch、pendingOperationId |
| POST /commands | 下表 | 202 `{operationId,acceptedRevision,scheduledAtUs}` |
| GET /operations/{id} | なし | `{id,status,code,revision,scheduledAtUs}` |
| GET /events | WebSocket Upgrade | 1Hz status、即時operation/fault event |
| GET /diagnostics | なし | 秘密を除いたJSON、heap/FPS/RTT/boot/slot/hash/エラーカウンター |

status.nodesはnodeId、role、online、firmwareVersion、activeHash、candidateHash、syncState、offsetUs、rttUs、lastSeenAgoMs、freeInternalHeap、freePsram、renderFps、scanRefreshHz、faultCodeを返す。未測定値はnull。認証者だけが参照可能。

POST /commands bodyは `{requestId,expectedRevision,op,args}` と `X-Control-Lease`。op/args:

| op | argsの全フィールド | 動作 |
|---|---|---|
| play_scene | `{sceneId}` | 位置0から再生 |
| go | `{cueId}` | 指定キュー位置0から再生 |
| next / prev | `{}` | 前述の境界規則 |
| pause / resume / stop | `{}` | 前述の時刻規則 |
| blackout | `{enabled:boolean}` | trueは即時、falseは全対象ノード正常時に予約 |
| brightness | `{value:0..255}` | capを越えた実効値をstatusへ返す |
| layout | `{layoutId}` | メンテナンス/STOPPED/暗転時だけ。08の新topology配布後に採用 |
| recover | `{}` | 全対象正常・hash/時計一致時に障害解除、STOPPED/暗転 |

pause/resumeの不適切状態は409。layoutは対応variantを検証して変更。0レイヤーsceneは正常な黒として扱う。

operation.statusはaccepted/preparing/scheduled/applied/failed/cancelled。appliedは全対象ノードの採用確認済み。status/eventだけから成功を推測せずoperationを確定源にする。WebSocketはイベントの補助であり、切れたらGET /statusを1Hz polling。再接続後は必ず全statusを取得する。[ESP-IDF HTTP/WebSocket機能](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_http_server.html)

### 7.2 メンテナンス・データ・更新

以下は全てログイン+CSRF+control lease。変更はrequestIdとexpectedRevisionをJSONまたはHTTP header `X-Request-Id`/`X-Expected-Revision`で渡す。

| method/path | request | response |
|---|---|---|
| POST /maintenance/enter | `{requestId,expectedRevision}` | 202 operation。STOP/暗転確認後maintenance=true |
| POST /maintenance/exit | 同上 | 202、転送/OTA処理中は409 |
| POST /deployments | binary LPK + Length/SHA/Request/Revision headers | 202 `{deploymentId,status:"validating"}` |
| GET /deployments/{id} | なし | state、progress、nodes[]ごとの候補hash/phase/error、errors[] |
| POST /deployments/{id}/distribute | `{requestId,expectedRevision}` | 202、B pull開始 |
| POST /deployments/{id}/activate | 同上 | 202 operation。全対象候補一致が条件 |
| DELETE /deployments/{id} | 同上 | 204、未activateの候補だけ破棄 |
| GET /content/active | なし | active LPKをattachmentで取得。秘密情報なし |
| POST /ota/{nodeId} | binary app-only + Length/SHA/Request/Revision + X-Firmware-Version + X-Board-Profile + X-Partition-Hash | 202 updateId、指定nodeIdへstream |
| GET /ota/{id} | なし | receiving/verifying/rebooting/healthy/rolled_back/failed、progress、code |
| PATCH /settings | `{requestId,expectedRevision,nodeId,rotation,brightnessCap}` | 200 settings。両設定必須、maintenance限定 |
| POST /diagnostics/pattern | `{requestId,expectedRevision,pattern,target}` | 202 operation。pattern=black/corners/rgb/seam/node_id、target="all"またはnodeId整数、maintenance限定、10秒で暗転 |

試作実装のMasterには、停止・暗転中にログイン+CSRFで呼ぶ `POST /api/v2/panel-test {}` もある。機器タブのボタンは`features.panelDiagnosticTest`と直前に取り直したstatusを確認し、停止・暗転中・保守外の場合だけ実行できる。64×32/16-scanの配線を仮定した低デューティのHELLOを5秒だけ出し、自動でOE HIGHへ戻す診断専用経路である。P5のdriver ICや信号タイミングを特定する機能ではなく、通常の描画・blackout解除には使わない。

物理USBからは `{id:1,op:"panel_test"}` を送って同じ診断を実行できる。ブラウザー認証やBOOT設定セッションを使わず、条件確認・5秒・最大5% duty・自動暗転はWeb経路と共通である。

deployment states: receiving→validating→staged→distributing→ready→activating→active、分岐failed/cancelled。受信中にPOST /deploymentsへ別送信すると409。transfer進捗はbytesReceived/totalBytes、検証はphase表示。UIに「転送完了」と「本番採用」を別表示する。

API password変更は `POST /auth/password {oldPassword,newPassword}`、maintenance限定、12–128文字、変更後全session/lease失効。AP PSK/groupKey/channelの変更はUSB再設定をv1正規経路とする。Webから不用意に接続先を変えない。

### 7.3 Node間HTTP

Browser cookieを使わない。B→Aの `GET /node/v2/candidate` は現在配布対象のLPK、`POST /node/v2/result` は `{packageHash,phase:staged|active|failed,code}`。A→Bの `POST /node/v2/activate` は `{packageHash,generation}`、`POST /node/v2/ota` はapp-only stream、`GET /node/v2/health` は更新状態、`PATCH /node/v2/settings` はrotation/cap。LAN公開された汎用ファイルサーバーにはしない。

試作実装ではMasterの `POST /node/v2/topology` が `{"topology":<完全配置>,"topologyHash":<sha256>}` を受け付ける。HTTP本文は16KiB以下とし、Followerは停止・暗転中であること、自身のnodeId/hardwareId/role、全slot割当、ローカルactive/candidate package内のlayout一致、正規JSONのSHA-256を確認してから適用応答を返す。MasterはRigとpackage layoutからtopologyを構築し、現在のrequired peer全台が同revision/hashを確認した後だけローカル同期済みへ進める。Rig保存後の配布は保守中に行い、起動後はactive packageとRigからMasterの配置状態を再構築する。Followerの候補pullは有効登録ノードを対象に5秒間隔で行い、Master側は保守中にだけ候補binaryを返す。candidateのstageは不活性slotへの保存で、採用はMasterの別操作と全対象照合を必要とする。実機2台間は未検証。

A→Bの `POST /node/v2/maintenance {enabled:boolean}` は、同期stateがstopped/blackoutかつ予約なしの時だけ受理する。`POST /node/v2/distribute {packageHash}` でBのpull jobを開始する。メンテナンス入口は先に通常state更新で停止/暗転を確認し、その後全対象ノードをmaintenanceへ切り替える。node healthはmaintenance、activeHash、candidateHash、firmwareVersion、update statusを含む。activate/settings/ota/distributeはmaintenance限定。実装の `POST /node/v2/activate` は `{packageHash,generation,stateRevision}` を受け取り、親機指定の再生state revisionを採用する。初回の追加基板（activeHashなし・generation=0）は、認証・停止暗転・maintenance・候補データと配置の照合が成立すれば、以前の表示データを持っていなくても採用できる。generationはuint32の単調増加値で、Aの次content世代を指定し、同generation/同hashの再試行は同じ結果を返す。OTA検証成功応答を返した後3秒待って再起動する。

ESP-IDFのdual-stack HTTP listenerが返すIPv4-mapped IPv6接続元（`::ffff:a.b.c.d`）はIPv4へ正規化し、UDPで認証済みの接続元IPv4と照合する。native IPv6や短いsockaddrは拒否する。候補の送信後は、候補の配置が全対象基板へ反映されるまで`topologySynchronized=false`を返す。

`POST /node/v2/diagnostics {pattern,durationMs:10000}` はmaintenance限定で組込みパターンを実行する。通常のblackout/faultは診断にも優先する。診断開始はユーザー暗転を一時的に診断出力へ置換する明示操作として扱い、10秒後は必ず暗転に戻る。A単体診断はB未接続でも許可する。全対象ノードパターンの開始時刻差は診断用途の許容範囲とし、同期精度の測定にはLPKの通常再生を使う。

全node HTTPは確立済UDP sessionが前提。headersはX-Node-Id、X-Boot-Id、X-Node-Seq（uint32の10進）、X-Request-Nonce（16bytes hex）、X-Body-SHA256、X-Node-MAC。MAC対象はUTF-8で `METHOD\nPATH\nGROUP_ID\nSENDER_BOOT_ID\nRECEIVER_BOOT_ID\nSEQ\nNONCE\nBODY_SHA256`、最終改行なし。HMAC-SHA256の鍵はsessionKey、表記は小文字hex。空bodyのhashも必須。HTTP seqはUDPとは別カウンターで、各方向1streamに直列化し、受理済み最大seq以下を拒否する。最大seqはsession中ずっと保持。nonceも相手ごと128件/10分保持して再利用拒否するが、古い再送の防止をcacheの保持期限だけに依存しない。session変更時は鍵とseqを更新しcacheを破棄。

応答にも同nonce、同seq、X-Body-SHA256、X-Node-MAC。応答MAC対象は `STATUS_CODE\nPATH\nSEQ\nNONCE\nBODY_SHA256`（STATUS_CODEは10進HTTP code）、鍵はsessionKey。binary応答も送信前の保存済hashをheaderへ置き、受信側はstream完了後hash/MACを照合して採用する。TLSはv1のnode通信にも必須でなく、これは認証・整合性であって暗号化ではない。

node requestのbodyも宣言hashと実受信bodyのhashを比較する。認証headerの検査後にinactive領域へstreamできるが、全body検証前のactive変更/OTA boot選択は禁止。binary受信失敗は旧active/旧appを維持する。

## 8. 認証・ブラウザー境界

APは個別生成の長いWPA2-PSK。Web管理passwordは別値。password verifierはPBKDF2-HMAC-SHA256、salt16bytes、iterations=100,000、derived32bytes。検証はHTTP専用workerで行いレンダリングをブロックしない。Master起動時に平文passwordを保持/ログ出力しない。

ログイン失敗はIPごと5回/分、超過後60秒待ち、全体10回/分。session tokenはCSPRNG32bytes、RAMのみ、HttpOnly/SameSite=Strict/Path=/のcookie。HTTPなのでSecure属性は付けられない。12時間でexpire、logout/再起動で失効。4session上限。秘密をlocalStorage/URLへ置かない。

全変更HTTPはContent-TypeとOrigin完全一致、X-CSRF-Tokenを検証。Originが付かないCLIはbrowser APIでなくUSB/node APIを使う。WebSocketもcookie+Origin検証。HostはMaster自身の現在IPとledpanel.localの許可リスト（portも照合）。外部APでIPが変わったら実インターフェースから更新し、任意Hostを許可しない。CORSは許可しない。DNS rebinding/CSRFを抑制する。

control leaseは30秒、UIが10秒ごとrenew。最初のclaimだけ成功、別sessionは409。force=trueはUIに現在の所有端末と引継ぎ確認を出した管理者操作で可能。lease切れでも現在表示は続ける。暗転trueは認証済みならleaseなしで許可。その他の点灯/切替はleaseが必要。

静的UIは同一オリジンのみ。CSPはdefault-src 'self'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self' <許可したMasterのwsオリジン>; img-src 'self' blob: data:; style-src 'self'; object-src 'none'; frame-ancestors 'none' を基準に実装し、inline script/evalを使わない。アップロードをHTMLとして返さない。filename/content-dispositionを固定安全名にする。

専用AP内HTTPでは、APへ接続できる攻撃者・盗まれた管理session・盗まれたgroupKey・物理Flash読取りへの完全な防御は範囲外。SHA-256を署名の代わりにしない。強い通信秘匿、製品配布、外部LAN/Internet操作が必要ならHTTPSの信頼配布と署名OTAを別ADRで追加する。外部APも専用の信頼するLANだけを対象とし、公衆ネットワークへ公開しない。

## 9. 主なエラーコード

`INVALID_JSON`, `SCHEMA_UNSUPPORTED`, `INVALID_REFERENCE`, `LAYOUT_MISSING`, `PACKAGE_TOO_LARGE`, `ASSET_HASH_MISMATCH`, `PACKAGE_HASH_MISMATCH`, `STORAGE_WRITE_FAILED`, `NO_MEMORY`, `AUTH_REQUIRED`, `CSRF_REJECTED`, `LEASE_REQUIRED`, `REVISION_CONFLICT`, `BUSY`, `MAINTENANCE_REQUIRED`, `PEER_OFFLINE`, `UNSYNCED`, `CONTENT_MISMATCH`, `LATE_COMMAND`, `PROTOCOL_MISMATCH`, `OTA_TARGET_MISMATCH`, `OTA_VERIFY_FAILED`, `RESTARTED`, `FAULT_LATCHED`。

messageは日本語でユーザー向け、codeは安定ASCII、pathは該当JSON Pointerまたはnull、retryableは操作を再試行してよいか。自動再試行が二重GOを起こさないようrequestIdを維持する。

## 実装の描画時計（2026-10-02）

stateのanchorUs/effectiveAtUsはMaster clock基準。Followerは描画時にも `localNowUs + clockOffsetUs` を使う。stateと有効なclock estimateは同じmutex内で取得し、未認証・peer timeout・5秒以上古いclock estimateでは描画しない。描画taskは各台の処理終了から33ms待つ方式をやめ、共通anchorから算出した次の30fps境界をesp_timerで予約する。USB inspectのlastRenderedMasterUs/lastRenderedTick/lastRenderedRevisionは最後に成功した描画の読み取り専用記録。
