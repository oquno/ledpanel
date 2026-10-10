# 03. ファームウェア・ハードウェア・書込み

> 設計時の選定案を含みます。現在の実装はESP-IDF 5.5.5と同梱の `esp-hub75` です。実際のビルド手順・依存関係は [ファームウェアREADME](../firmware/README.md)、確認範囲は [実装状況](IMPLEMENTATION_STATUS.md) を参照してください。

## 1. 技術構成と互換性ゲート

ファームウェアはESP-IDF + C++17、ターゲットesp32s3。初期検証候補はESP-IDF **v5.5.5**とESP32-HUB75-MatrixPanel-DMA **3.0.14**。release tagと完全commit SHAを取得して固定する。調査時のmasterには3.0.15表記があるが、masterの追従はしない。この組合せのビルドと実機動作は今回未検証。[IDFリリース](https://github.com/espressif/esp-idf/releases)、[DMAリリース](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/releases)、[masterの版表記](https://raw.githubusercontent.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/master/library.properties)

DMAをESP-IDF componentとして取り込み、NO_GFXを使用、Arduino runtimeはv1で依存しない。ライブラリにnative IDF用CMake構成があることは確認したが、使用tagのCMakeとIDF private APIの互換性はM0で検証する。失敗したらパッチまたはIDFの対応patch版を固定し、変更理由とビルドログをADRに記録する。[component CMake](https://raw.githubusercontent.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/master/CMakeLists.txt)

WebはHTML/CSS/JavaScript ES modulesを直接配信し、開発用HTTP/APIと補助ツールはPythonで統一する。Node.js/npm/pnpm/React/Viteを必須にしない。nativeテストはCMake/CTest、Webはブラウザ内テストをPythonから実行する。共通描画コアはC++とstandalone WASMで共有し、Clang/LLDの版固定とビルド検証は実装時のゲートとする。詳細は [09-technology-selection.md](09-technology-selection.md)。

## 2. ボード定義

profileId=`seengreat_hub75_s3_n16r8_v1`。公開仕様のモジュールはESP32-S3-WROOM-1-N16R8、Flash16MiB/PSRAM8MiB。現物の刻印と起動時検出値を照合する。16MiB未満、PSRAM検出失敗なら通常再生を有効化せず診断画面へ進む。PSRAMを単に容量だけでDMA対応とみなさない。[公式wiki](https://seengreat.com/wiki/214/rgb-matrix-hub75-s3)

HUB75ピンの初期値は以下。基板版の照合なしで別ボードに転用しない。

| 信号 | GPIO | 信号 | GPIO |
|---|---:|---|---:|
| R1 | 5 | G1 | 4 |
| B1 | 6 | R2 | 15 |
| G2 | 7 | B2 | 17 |
| A | 8 | B | 18 |
| C | 10 | D | 9 |
| E | 16 | CLK | 12 |
| LAT | 11 | OE | 13 |

出典: [Seengreat公式ピン表](https://seengreat.com/wiki/214/rgb-matrix-hub75-s3)。64×32・1/16 scanが確認できたprofileではE=-1として未使用。メーカーの64×64サンプルを寸法だけ変えずに流用しない。

パネル設定はwidth=64、height=32、chainLength=1。IC/scan/colorOrderは実物確認が必要。scanは1/16候補、driverはSHIFTREG候補、RGB順候補としてM0で試す。FM6126A等のICが判明したらその初期化をHALへ固定する。未対応scanをUIから選べるようにしない。

v1はSD、RTC、音声codec、PCA9557のキーを初期化しない。未使用スピーカーアンプはGPIO3をLOWに保つ。基板のチップenableやOEに影響するPCA9557設定が必要なら、公式回路図で確認した値をHALに追加する。この回路図本文は今回未取得なので、初期化値を推測しない。

## 3. 起動・描画・タスク

起動順序:

1. 出力を暗転、診断ログ開始、NVSと設定version確認。
2. Flash/PSRAM/partition/ボードprofile確認。必要なバッファを確保。
3. active slotをヘッダーから全文ハッシュまで検証。失敗時は直前slotを試し、結果を記録。
4. HUB75 DMAを黒バッファで起動。検証されていないpinを駆動しない。
5. Wi-Fi、認証、HTTP、同期を開始。AはAP、BはSTA。
6. 同期・データ一致を確認してREADY_BLACKOUT。ユーザーの再生操作を待つ。

未設定ならUNPROVISIONEDとしてUSB設定待ち。共通デフォルトAPは作らない。初期画面が見えなくてもUSBシリアルで診断可能にする。

| タスク | Coreの初期案 | 責務 |
|---|---|---|
| render | 1、高優先 | 検証済みstate snapshotを読み30fpsで描画・DMA反映 |
| control/sync | 0、中高 | UDP、時計推定、予約状態、heartbeat |
| http | 0、中 | 認証・API・静的UI、長い処理はjobに委譲 |
| storage/update | 0、低 | メンテナンス時だけ検証/Flash操作 |
| diagnostics | 任意、低 | 1秒統計、リングログ、watchdog監視 |

Wi-FiやIDFシステムタスクのpriorityを上書きしない。renderへネットワークcallbackから直接書き込まず、immutable snapshotのatomic交換/小さいロックで渡す。同期タスクが遅れてもDMA走査は維持。描画に遅れたら古いフレームを追い描きせず現在時刻へジャンプする。

DMAは内部のDMA可能SRAMを第一候補にする。ダブルバッファ、色深度6bit/channelから検証、200Hzを満たしメモリーに余裕があれば上げる。PSRAM DMAはOctal PSRAMとライブラリ制約を別途検証した場合のみ採用。DMAの配置、色深度、CLK、blanking値を診断へ出す。[DMA対応とメモリー制約](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA)

## 4. メモリー予算

最大LPKをPSRAMへ読込み、manifestを上限付きで解析する。runtime資産はblobを参照し重複コピーしない。JSON DOMは読込み時だけ使用し、検証後の描画モデルへ変換したら解放。

| 対象 | 予算/扱い |
|---|---|
| package raw | PSRAM ≤2.5MiB |
| manifest解析/モデル | PSRAM ≤512KiB、一時DOMを含むピーク≤1MiB |
| 2×RGB888ローカルbuffer | 12,288 bytes |
| DMA | 内部SRAMで実測。初期予算≤160KiB |
| UDP受信 | 1200bytes×8上限、古いstateを集約 |
| HTTP upload buffer | 16KiB×1、package全体を内部SRAMに置かない |
| 診断ログ | 32KiB固定リング、Flashへ常時書込まない |
| 起動後の空き内部heap | ≥48KiB、最大連続ブロック≥16KiBを目標 |
| 空きPSRAM | 最大コンテンツ時≥2MiBを目標 |

heap不足で色やレイヤーを黙って減らさない。メンテナンス状態でエラーを返す。アップロード前に再生モデルを解放できるが、active slotは消さない。成功/失敗後にactiveを再ロードして暗転待機する。

## 5. 16MiB Flash partition案

partition tableは0x8000。appの64KiB alignmentを維持する。以下は実装開始時の確定案であり、容量変更時はSchema上限も含め影響を検証する。

| name | type/subtype | offset | size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x6000 |
| otadata | data/ota | 0xF000 | 0x2000 |
| phy_init | data/phy | 0x11000 | 0x1000 |
| nvs_keys | data/nvs_keys | 0x12000 | 0x1000 |
| ota_0 | app/ota_0 | 0x20000 | 0x400000 |
| ota_1 | app/ota_1 | 0x420000 | 0x400000 |
| content_0 | data/0x40 | 0x820000 | 0x300000 |
| content_1 | data/0x41 | 0xB20000 | 0x300000 |
| rig_0 | data/0x42 | 0xE20000 | 0x10000 |
| rig_1 | data/0x43 | 0xE30000 | 0x10000 |
| reserved | 未割当 | 0xE40000 | 0x1C0000 |

0x13000–0x1FFFFはalignment gap。reservedを自動的にFATとしてマウントしない。UIはapp内へgzipで埋込み、JS/CSS/WASM合計gzip≤1.5MiB、app全体≤4MiB。超過はビルド失敗とする。

content partitionはファイルシステムを使わずLPKをoffset0から保存する。アプリメタデータのactiveSlot、previousSlot、generationは1つのversioned NVS blobへまとめ、commitを完了させて切り替える。slotの内容は常に検証しNVS pointerだけを信じない。

1. inactive slotを消去しストリーム書込み（headerも保存）。
2. Flashから全文再読込みしLPK検証。完全検証前はcandidate扱い。
3. 全対象ノードの候補hash一致を確認し、04のactivateへ進む。
4. NVS active pointerを更新。旧slotは次の更新まで保持。

途中電源断は旧activeを選ぶ。pointer更新済みでも不正LPKなら旧slotへ戻り暗転。全対象ノードのpointer更新は原子的ではないため、再起動後のhash不一致時は全対象暗転し、Aのactiveを基準に再配布する。暗黙に片側だけ新しい曲を表示しない。

## 6. 通信・永続設定

内蔵APモードのMasterは `LEDPANEL-<groupId末尾6桁>`、IP=192.168.4.1/24、DHCP=192.168.4.10–30、2.4GHz AP。初期channel=6、設定可能値1/6/11。地域設定は日本、Wi-Fi省電力を無効化。内蔵APの最大station数=6（最大3 Follower + 操作端末3台）。外部APモードも初版必須で、全ノードをSTAとして同一専用LANへ接続する（08）。NATとInternet公開はなし。mDNSは `ledpanel.local` を補助提供し、IP直アクセスを必須経路にする。

各Followerは選択したネットワークSSIDへ接続し、DHCPでアドレス取得、Aへ署名付き登録。再接続backoff=1,2,4,8,16,30秒、上限30秒、±20%jitter。UDP/TCPの再接続で表示を勝手に再開しない。

NVS設定: configVersion=2、boardProfile、nodeId(1–65535)、hardwareId(UUID)、label、masterNodeId、role(master/follower)、groupId(UUID)、SSID、AP PSK、groupKey(32bytes)、admin password verifier（Aのみ）、rotation(0/180)、brightnessCap、channel、active metadata。nodeIdとroleは独立。group内IDをUSBツールが割当て、再起動で維持する。台帳と配置割当は08。

初期userBrightness=26/255（約10%）、brightnessCap=64/255。再生輝度はRAM、capは設定で保存。cap変更はメンテナンスかつ管理者操作。capは電流を実測して決めるための設定であり、電流センサーや電気的保護の代わりではない。電流/温度の計測機能を搭載しているとUIに表示しない。元資料の5V系・低輝度からの試験を守る。

## 7. USB初期設定と書込み成果物

同じボードprofileなら全ノードに同じbinを書く。別profileのbinは同じprotocol/rendererを満たすこと。役割だけのためにソースを変更して再ビルドしない。Windows向け手順を必須にし、macOS/LinuxでもCLIが動く構成にする。

実装時の成果物:

- `firmware-<version>-seengreat.bin`: factory merged image。実際のビルドoffsetを反映。
- `firmware-<version>.ota.bin`: app-only OTA image。merged imageと明確に区別。
- `release.json`: firmwareVersion、gitSha、boardProfile、protocolMajor=2、renderer、partitionTableSha256、各fileのsize/sha256、IDF/component lock情報。
- `flash.ps1` とPython CLI: ポート列挙、chip/flash確認、初回書込み、monitor、USB設定。

CLIの契約:

```text
ledpanel-provision create-group --output <private-file>
ledpanel-provision flash --port COM7 --release <release-directory>
ledpanel-provision configure --port COM7 --group <private-file> --node-id 1 --role master
ledpanel-provision configure --port COM8 --group <private-file> --node-id 2 --role follower
ledpanel-provision inspect --port COM7
```

create-groupはOSのCSPRNGでUUID、AP PSK（24文字以上）、admin初期password（24文字以上）、groupKey32bytesを作る。秘密設定ファイルはユーザー指定場所へ保存し、リポジトリ/デザインZIP/診断ZIPへ含めない。CLIは画面にgroupKeyを出さず、AP接続情報と管理passwordは明示的な表示/印刷操作のみで示す。

USB CDCコンソールは115200、UTF-8 JSON Lines、1行最大4096bytes。requestは `{id:1,op:"configure",config:{...}}`、応答は `{id:1,ok:true,rebootRequired:true}`。機能はhello、inspect（秘密マスク）、configure、factory_reset、panel_test。configure/factory_resetは物理操作で開いた専用USB設定セッションでのみ許可する。通常起動後、BOOTを離した状態から3秒長押しして設定モードへ入る方式を初期案とする。リセット時にBOOTを押し続ける操作はROM download用であり、アプリの設定モードと混同しない。GPIO0の入力と基板の実配線を確認し、設定モードは60秒無操作で終了、表示は黒、Webから開く操作は提供しない。手順を現物で確定するまでは成功したと扱わない。

`{"id":1,"op":"panel_test"}` は設定モードを開かずに使える、明示的なUSB診断操作である。Master・停止・暗転・保守外・同期処理なしをWeb診断と同じ条件で確認し、HELLO信号を最大5秒・最大5% pixel dutyで出してからOEをHIGHへ戻す。設定/NVSは変更しない。P5のドライバーや電源状態を特定する試験ではない。

通常起動中にシリアル入力しただけで秘密設定を書き替えない。USB transportの具体的な物理確認手順はM0ゲート。端末にログとJSONを混在させず、JSON以外のログは別CDCチャネルまたはprefix付きにしてCLIで分離する。

factory_resetは設定/鍵/コンテンツを削除し、UNPROVISIONEDへ戻る。全eraseは初回または明示resetだけ。通常flash/OTAで設定を消さない。BOOT/ENと通常USB-Cの使用方法を納品手順に写真またはメーカー図の参照付きで記載する。

## 8. OTAと復旧

OTAはメンテナンス状態、暗転、認証済み管理者から開始。Aが受けたファイルを一度永続stageする空き領域をv1では設けないため、**FollowerのOTAはMasterが接続を中継して対象のinactive appへストリームし、各ノード向けに同じFile/Blobから順次再送**する。04のnode-target APIを使う。UIはFile/Blobを保持して対象台数分を順次送信する。

target/size/SHA/firmware metadata/partition hashを確認し、app-onlyのみ受け入れる。`esp_ota_begin/write/end`系とrollbackを使用し、bootloaderとpartition tableはOTA対象にしない。IDF rollback有効化と新imageのvalid markingを実装する。[Espressif OTA](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/ota.html)

対象FollowerをnodeId昇順に1台ずつ更新→各health確認→最後にMaster更新の順。初回起動30秒以内にNVS読出し、heap、黒DMA、HTTP/同期初期化、content読出しの自己検査を行いvalidをマークする。BがAPへ接続できないこと単独ではrollbackしない。自己検査が失敗/クラッシュ/タイムアウトなら旧appへrollback。更新中に操作端末が切れたら完了済みのノードだけを記録し、暗転を維持。

同一protocolMajorかつrenderer一致が確認されるまで再生不可。v1用OTAは同じ形式を扱う互換リリースに限定。新しいmajorへの移行やpartition変更はUSB手順で行う。NVS/contentの破壊的移行を新appの自己検査中に行わない。

コンテンツ未配布やUNPROVISIONEDは初期状態として正常であり、それだけでOTA自己検査失敗にしない。検証対象は読み取り処理が正常に動き、未設定/未配布を正しく扱えることである。

v1は署名付きSecure Boot/eFuse anti-rollbackの製品化を必須としない。OTA hashは改ざん元の正当性を保証しない。信頼したreleaseファイルを管理者が専用APで適用する脅威モデルを04に明記する。

## 9. 音反応の将来追加

全章のA/B表記は1組の通信例。複数Followerへのファンアウト・参加集合・更新順・失敗条件は08を適用する。

v1の `capabilities.audioInput=false`。マイク解析を無効にしても文字・画像表示が完成することを保証する。将来のv1.x/2ではES7210のMaster入力、音量/FFTの時刻付き制御値、専用renderer featureを追加する。PCMを常時Wi-Fi転送しない。音声による曲名認識や録音は別要件。

オンボード音声のI2Sピンは公式表があるが、チャンネル割当、I2Cアドレス、codec reset/enable、サンプル形式は回路図とサンプルで照合が必要。GPIOだけで一般的なI2Sマイクとして初期化しない。S3ではLCD側DMAと音声I2Sのリソース競合・帯域も検証する。[公式ボードwiki](https://seengreat.com/wiki/214/rgb-matrix-hub75-s3)、[ESP-IDF I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)
