# 08. 増設・配置・ノード管理（改訂1.1）

## 1. 2台は試作条件

ユーザーの追加指示: **2台構成なのは試作のためで、増えることを前提にする。** 本章は増設を初版の構造要件として定義する。01–07にあるA/BはMasterと単一Followerの通信例、2台の試験は初期の実機試験である。集合全体への操作は本章に従い、変数名、UI、配列、ACK判定、ファイル形式に2台固定を埋め込まない。

初期設計・シミュレーション範囲は1–16ノード（Masterを含む）。これは無制限の性能保証をせずに実装予算を決めるための暫定上限で、ユーザーの最終台数を16と決めるものではない。MAX_ACTIVE_NODES、maxSlotsPerLayout等を一箇所で管理し、capabilitiesに公開する。16超へ拡張するときはメモリー、通信、配置の能力上限を検証して引き上げる。nodeId空間とwire形式に16を埋め込まない。

初版で必須: 動的ノード台帳、任意ID、任意slot配置、実機割当、複数Follower、外部AP、全参加者の準備確認、部分配布失敗と交換復旧。実機受け入れ開始は手元の2台、1/4/8/16台の挙動はシミュレーターで確認する。自動Master選挙、複数Master分散、90度回転、異解像度パネルは初版外。

## 2. デザインのslotと実機nodeを分離

`layouts` は文字列enumの配列から、次のオブジェクト配列へ変更する。project/runtimeの両方で同じ定義を持つ。1–16 layout、idは通常ID、defaultLayoutはそのid。

```json
{
  "id": "grid_2x2",
  "name": "4枚・2行2列",
  "width": 128,
  "height": 64,
  "slots": [
    {"id":"panel_1","x":0,"y":0},
    {"id":"panel_2","x":64,"y":0},
    {"id":"panel_3","x":0,"y":32},
    {"id":"panel_4","x":64,"y":32}
  ]
}
```

各slotは正面から見た64×32画素の配置枠で、物理MACやnodeIdを含まない。1–16 slot、同layout内id一意。x/yは0以上、slotの右下がcanvas内に収まること。canvas幅/高さは1–4096、面積≤262,144画素。slot同士の面積の重複は禁止、隙間は許可。覆われない画素はプレビューだけに存在し、実機では表示されない。90/270度は不可。

grid生成は列C・行Rでwidth=64*C、height=32*R。横列N枚なら64*N×32、縦列なら64×32*N。プリセットhorizontal/verticalは初期2枚の例であり固定enumではない。各scene.variantsはlayoutIdをキーにした辞書。宣言した全layoutのvariantを用意する。

資産とrectの寸法上限は幅/高さとも2048へ一般化する。資産1個の画素数≤65,536、資産合計≤800,000、LPK≤2.5MiBは維持。canvasが大きくてもESP32は自分の64×32だけ描く。全面canvasのRGBバッファを各ノードに確保しない。16枚の密配置全体は32,768画素であり、16ノードへ同じLPKを配布しても各ノードの容量が16倍にはならない。配布総通信量と所要時間は台数に応じて増える。

## 3. ノード台帳とrig

nodeIdは1–65535、0は予約。USBツールがgroup内で重複しないIDを発行する。hardwareIdは端末ごとのUUIDで初回設定時に生成し、通常書込み/再起動で維持。IDは表示順、配列index、IP、Master役割と独立。MasterをnodeId=1と決め打ちしない。A/B等は編集可能なlabel。

台帳は最大64件、enabled=trueは最大16件。Masterは台帳に1件だけ存在しenabled=true。同一nodeId/hardwareIdの衝突は登録拒否し、既存の接続先を上書きしない。Master選択はUSB設定で行い、運転中のWeb変更や自動選挙をしない。

rigはデザインと別の機器設定で、`schemas/rig.schema.json` を契約とする。

- version=2、groupId、masterNodeId、revision、nodes、layoutsを持つ。
- nodesはnodeId、hardwareId、label、role、enabled、boardProfile、rotation、brightnessCap。
- layoutsはlayoutIdとbindings配列。bindingはslotIdとnodeIdだけを持つ。
- layout定義そのものはLPKから参照。同じlayout内では1slotに1台、1台に1slot。各slotへenabledな登録nodeを割り当てる。
- 全variantへの完全な割当は下書き保存では不要。選択layoutをactivate/playする時は全slotの割当が必須。
- 同じnodeを別layoutで異なる場所へ割り当ててよい。180度はnode校正設定なのでlayout間で共通。
- rigにpassword、groupKey、session、SSID credentialを含めない。デザインZIPへも同梱しない。運用バックアップは `*.ledrig.json` として明示出力する。

追加ノードは登録のみで未割当/standby。未割当のノードがオンラインになっても自動で表示領域に加えない。新しい台数を表示に使う時にlayout/variantとbindingを更新する。

## 4. 対象集合とtopology

選択layoutに割当たるnodeId集合をdisplayMembersと呼ぶ。同期操作のrequiredMembersは `{masterNodeId} ∪ displayMembers`。Masterが表示を担当しないlayoutでも制御参加者として必要。配列はnodeId昇順。無関係の登録済みstandbyが不在でも本番を止めない。台帳未登録のノードからのHELLOは接続候補として記録できるが、制御参加者にはしない。

`topologyRevision` はrigの単調増加revision。`topologyHash` は次のオブジェクトを02の正規JSON化してSHA-256したもの:

`{groupId,masterNodeId,topologyRevision,layout,members}`

layoutは現在LPKの完全layout定義、membersはrequiredMembersに含まれる完全node台帳レコードに `slotId` を加えた配列（Masterが非表示ならnull）。membersはnodeId昇順、layout.slotsはslotId昇順に正規化する。IP、bootId、健康状態はhash対象にしない。

Masterは完全topologyを認証付き `POST /node/v2/topology` で参加Followerへ配布し、各ノードは自身のhardwareId/role/slot/校正、package内layout、hashを検証してRAMへ準備する。JSON上限16KiB。各state、HEARTBEAT、READY、ACKへtopologyRevision/hashを含める。stateは `schemas/state.schema.json` 参照。hashが違う状態で再生しない。packetに全台帳を入れず、1200byte上限を維持する。

rig更新はメンテナンス/停止/暗転/予約なしでのみcommit。revisionを増やし、対象集合の新旧の和集合へ暗転を伝達し、新topologyを配布する。古い対象が通信不能なら最後のheartbeatから1秒以上待ち、同ノードのwatchdog暗転期限経過後に集合変更を完了する。再接続した除外ノードは新topologyを確認してstandbyのままにする。

Masterのrigは03の専用raw partition rig_0/rig_1へ二重保存する。1slot=64KiB、先頭40bytesはASCII `RIG2`、JSON長uint32 LE、JSONのSHA-256 32bytes、その後に正規rig JSON（最大32KiB）。inactiveへ書込み・再読込み検証後にNVSのrigSlot pointerをcommit。旧slotを次回更新まで保持する。ノード台帳全体を既存24KiB NVSへ押し込まない。起動時にrig/active LPK/自身の設定の整合が取れない場合は暗転待機。Followerは自身のUSB設定だけを永続化し、完全topologyは接続時にMasterから再取得して検証する。

初回でLPKがない状態はmode=stopped、packageHash=null、topologyHashは64桁の0、topologyRevision=0のbootstrap stateだけを許可する。点灯状態へ使わない。新layoutへの切替はメンテナンス/停止/暗転で、対象rig割当を検証しtopologyを配布してから採用する。rigを変えなくてもlayout変更でtopologyHashは変わる。

## 5. N台同期・暗転・鍵

Masterはpeer[nodeId]辞書を持ち、IP、hardwareId、bootId、個別sessionKey、UDP/HTTP seq、replay window、RTT/offset、準備状態、hash、最終応答を分離して保存する。groupKeyはUSB配布済みで、各Master/Followerペアのnonce/bootから異なるsessionKeyを導出する。台帳とhardwareIdの認証を結び付けるためHELLOにはhardwareId、OFFER/ACCEPT/SESSION_OKにはmasterNodeId/followerNodeIdを必須追加し、署名済みheaderのsenderと一致を照合する。

handshake後は一組のsessionを上書きして全Followerに共用しない。各Followerの時計測定は1Hz、起動8標本/100msの基本は04どおり。定常送信はnodeId順に1秒内で分散し、同時送信バーストを抑える。

PREPARE/COMMITはrequiredMembersへの個別unicast。Masterローカルも同じ検証・採用状態を持つ。通常時は04の500ms以上先/READY100ms/ACK期限を全参加者に適用し、全員READY→全員committed ACK→全員applied ACKで初めてoperation成功。1台でも失敗なら多数決で続行せず参加集合全体を暗転。直列に各ノードの100ms待ちを足さず、全体期限を共用する。台数増加で間に合わないネットワークでは期限を黙って伸ばさず未達を記録し、設定・設置を再検証する。

HEARTBEATとSTATEは250ms、Masterから各required Followerへ個別送信。Masterが1台でも必須Followerのheartbeatを1秒失えば全体fault。FollowerはMasterとの通信が1秒途絶えれば自己暗転。明示除外はメンテナンスでrig/layoutを変更して行い、通信断を自動的な集合縮小と解釈しない。表示しないMasterは自身のHUB75を黒にする。

## 6. N台への配布・OTA

配布対象は選択layoutのrequiredMembers。候補LPKが新layoutの場合はその候補に対するrig割当を先に検証する。通常は全台に同じLPKを置くが、standbyや別layout専用nodeへの先行配布は個別stageとして扱い、本番成功条件へ混ぜない。

Masterへ1回アップロード→FollowerをnodeId順に1台ずつpull→全対象staged/hash一致→全対象activate→全activeHashとtopologyHash一致を確認→暗転待機。転送concurrencyは初版1、1転送16KiB buffer。operation/deploymentは対象集合を開始時に固定し、node別phase/bytes/hash/errorを返す。処理途中に台帳やlayoutを変えない。

1台失敗時は成功した候補を保持し、同packageHash/同topologyRevisionで未成功ノードだけ再試行できる。全体activate前に旧activeを保持する。activate途中の電源断は全体原子性を保証しないため不一致時に全体暗転。Masterの確定activeを基準に対象全員へ再配布する。

OTAは対象Followerを1台ずつ、最後にMaster。各nodeのboardProfileに合うappをrelease bundleから選ぶ。同profileなら同じFile/Blobを再送。未対応profileを検知したらそのノードを失敗とし、他profileのimageを押し込まない。mixed hardwareでも通信とrendererは共通、DevKitC-1の実機サポートはHALを追加して検証後に有効化する。

## 7. ネットワークの規模と探索

`networkMode=softap|infrastructure` をUSB設定で選ぶ。softapは初期製品範囲1–4 active node、AP station枠6（最大3 Follower + 3操作端末）。ハードウェアの最大接続数と、この製品の暫定運用上限は別である。ESP-IDFのAP接続上限には暗号化キー設定などの条件があるため実装時にsdkconfigと照合する。[Espressif Wi-Fi AP設定](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/wifi.html)

infrastructureは初版必須で、1–16 active nodeと操作端末を専用外部APへ接続。全ノードSTA、MasterがHTTP/同期の権威である点は同じ。Internet、クラウド、PC常駐サービスは不要。全端末間の通信を許す同一LANを用意し、client isolationは無効、MasterにDHCP予約を設定する。IPはシリアル診断/セットアップ画面で取得し `http://<Master-IP>` を使う。

FollowerのmasterAddressはUSB設定のIP/hostname、mDNSは補助。探索結果は鍵・groupId・masterNodeId・hardwareId照合前に信用しない。Master IP変更で接続できない場合は暗転して再探索/再設定を待ち、無認証APへ切り替えない。HTTPのOrigin/Host/WS接続先は自身の実インターフェースから許可リストを作り、192.168.4.1をハードコードしない。

## 8. API・Web UI・追加手順

追加APIはいずれもログイン/CSRF/control lease必須、変更はメンテナンス限定。rigのGET/PUTだけbody上限40KiB、内部rig JSON上限32KiBの例外とし、他は16KiB。requestIdとexpectedRevision（制御state）に加えexpectedTopologyRevision（rig）を検証する。サーバーが新rig.revision=旧revision+1を確認する。

| method/path | request / 結果 |
|---|---|
| GET /api/v2/rig | 秘密なしrig、現在topologyHash、未割当/不在状態 |
| PUT /api/v2/rig | `{requestId,expectedRevision,expectedTopologyRevision,rig}`、全体意味検証後commit、202 operation |
| GET /api/v2/nodes | 台帳nodes[] + online/ready/standby/hash/version、固定2件ではない |
| GET /api/v2/capabilities | maxActiveNodes/maxRegisteredNodes/maxLayouts/maxSlotsPerLayout/maxCanvasWidth/Height/Pixels/networkMode/networkNodeLimitを追加 |

台帳への秘密の追加はUSB経由。CLIに `register-node --group <private-file> --label <name>`（ID/hardwareId割当）と `configure --node-id <id> --role follower`、`--network-mode`、`--master-address` を提供する。GUIは秘密なし台帳を取り込みrigへ登録し、設定済みhardwareIdと署名付きHELLOが一致した機器だけを割当可能とする。

Webは可変長ノード一覧、準備済み/required台数、各機器の点灯識別、未割当/欠損表示を必須とする。配置画面で行列/横列/縦列/自由座標を生成し、slotへ実機を割当。増設時にsceneを自動伸長せず新canvas用variantを複製・調整・再コンパイルする。

追加フロー: USB設定→台帳登録（まだstandby）→メンテナンス→layout/variant/割当を更新→全対象検証・配布→topology一致→暗転待機→明示GO/解除。

交換フロー: 故障機をdisabled→交換機へ新nodeId/hardwareId発行→同じslotへbindingを付け替え→校正・パッケージ配布→topology一致確認。同じslot/デザインを使うので交換のためのデザイン再作成は不要。古い機器が戻っても自動復帰させない。

## 9. メモリーと増設テスト

Masterのpeer情報はactive16件まで、詳細はregistered64件を上限付きで保持。peer/操作集約の初期追加予算はPSRAM96KiB、内部SRAM16KiB。UDP受信buffer1200×8を維持してキューを処理し、再送で回復する。大量同時受信のdrop数を診断へ出す。各peer HTTP nonce cacheは128件だが、合計をPSRAMへ配置して計測する。UIのstatusが16KiBを越える場合はGET /nodesをページ化し、UDPの上限を増やさない。

| ID | 検証 | 完成条件 |
|---|---|---|
| N-01 | 1/2/4/8/16台、Master id=7、Follower=12/42等 | 配列index/役割にID固定なし、全対象ACK |
| N-02 | 2×2、横8枚、縦8枚、隙間あり | canvas/slot座標と境界が正しい |
| N-03 | slot重複、範囲外、未割当、node二重割当 | 意味検証で拒否 |
| N-04 | 4台中1台通信断、standbyだけ通信断 | 前者は全対象暗転、後者は継続 |
| N-05 | 途中1台の配布/activate失敗 | 全体成功にしない、未完了だけ再試行 |
| N-06 | 2→4増設、4→3縮小、交換機追加 | topology改訂、古いstate拒否、同slot再利用 |
| N-07 | 同nodeId別hardwareId/不明Master | 認証/台帳照合で拒否 |
| N-08 | 外部APでMaster IP変更、AP停止 | 再探索中暗転、回復しても自動点灯なし |
| N-09 | 16台sim・5%損失・GO100回 | 全員ACKまたは全体fault、seq/hash混線なし |
| N-10 | 16台用LPK・最大peer表・順次OTA | 03のheap予算と08の追加予算を測定、Master最後 |

## 10. 版の扱い

文書1.1でデータmajor=2（LPK2）、同期major=2（LPS2）、browser/node API=/v2、configVersion=2へ変更。rendererの画素合成規則は変わらないためlp-raster-1を維持する。文書1.0の固定layout形式と混在させない。実装前なので旧形式を本番読込みする義務はなく、不対応として明示拒否する。

初版ユーザーの正式データがまだないため自動移行は必須にしない。旧試験ファイルを変換する場合だけ、horizontal/verticalを2slot定義へ展開し再コンパイルする。rigは明示的に作り、node割当をデザインから推測しない。
