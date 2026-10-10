# 06. 受け入れ条件・実装順・未検証事項

## 1. 完成の意味

このリポジトリの現段階は仕様策定完了。以下は次の実装で満たす条件であり、今回すでに合格したという意味ではない。実装者は実行したテストと未実施を分け、使用機材、依存版、commit、測定条件を記録する。

M0が実機不足で未完でもM1/M2と通信シミュレーターは進められる。実機がないのに「実機対応完了」と報告しない。

## 2. 実装順序

| 段階 | 成果物 | 終了条件 |
|---|---|---|
| M0: 基板立上げ | board HAL、IDF/DMA固定、USB設定、低輝度診断 | 64×32単体で四隅/色/走査確認、Flash/PSRAM一致、USB設定成功 |
| M1: データ・描画 | Schema検証、LPK codec、C++ core、WASM、golden | 同入力がnative/WASMで同じRGB、例が正しくdecode |
| M2: 編集Web | テキスト/ロゴ/動き、縦横、保存、キュー | 日本語を含むプロジェクトを保存→別端末で同じLPK生成 |
| M3: 単体再生・保存 | 内蔵UI、auth、slot更新、再生API | 最大パッケージ、電源断復旧、無認証拒否 |
| M4: 可変台数同期 | handshake/clock/state/配布、障害暗転 | 試作2台実機 + 1/4/8/16台シミュレーション、境界、損失、増設、復帰、2時間試験 |
| M5: 運用・更新 | live UI、OTA、診断、書込み成果物 | 対象browser、B→A更新/rollback、納品手順で再現 |

音反応/SD/増設を先行してM0–M5を遅らせない。完了判定はR-01–R-10をすべて満たすこと。

## 3. 仕様・データのテスト

| ID | 入力/条件 | 必須の結果 |
|---|---|---|
| D-01 | examplesのproject/runtime JSON | Schemaと意味検証に合格 |
| D-02 | `minimal.lpk` | ヘッダー、digest、全体hash、赤spriteの1画素を一致確認 |
| D-03 | LPK先頭/長さ/hash/末尾1byteを破損 | reject、旧active維持 |
| D-04 | asset offset重なり、領域外、乗算overflow | 書込み/描画前にreject |
| D-05 | duplicate JSON key、未知version/property、未参照scene | reject、JSON Pointerを返す |
| D-06 | ZIP traversal/重複/巨大展開/巨大画像 | 上限内で処理停止、draftを保持 |
| D-07 | キーtick重複/降順、end範囲外、variant不足 | reject |
| D-08 | 同じ .ledproj を2回コンパイル | LPK全bytesとpackageHash一致 |
| D-09 | 別OSで保存済み日本語ラスタを再コンパイル | 同LPK。明示的な文字再画像化後だけ変更可 |
| D-10 | 2.5MiB境界±1、資産画素上限±1 | 境界の採否が仕様通り |

## 4. 共通描画golden

最低限、黒、RGB、alpha0/128/255、opacity、重なり順、クリップ、負x/y、180度回転、縦横境界、loop/hold、sequenceの累積時間境界、負方向linear補間、step、開始/終了半開区間の出力byte比較を行う。

確定ベクトル:

- RGB565A8 `00 f8 ff` はRGB888 `[255,0,0]`。
- 赤 `00 f8 80` を黒へopacity255で重ねると `[128,0,0]`。
- 赤 `00 f8 ff` を青 `[0,0,255]` へopacity128で重ねると `[128,0,127]`。
- xキー(0,0,linear)→(3,-2,step)はtick1で-1、tick2で-2、tick3で-2。
- layer `[start=2,end=5)` はtick1で非表示、2/4で表示、5で非表示。
- horizontalの仮想(63,0)はA(63,0)、(64,0)はB(0,0)。verticalの(0,31)はA、(0,32)はB。
- B rotation180でhorizontalの仮想(64,0)はBの物理(63,31)。
- 30fpsは1,000,000μsで30tick、999,999μsで29tick。
- sequence durations[2,3]のloopはtick0/1がframe0、2/3/4がframe1、5がframe0。

実装時にRGB出力をraw binaryで保存し、WASM/nativeの差分を0byteとする。LEDの輝度や光学色の完全一致は別試験。

## 5. 同期と障害のテスト

| ID | 操作/障害 | 必須結果 |
|---|---|---|
| S-01 | 2台正常、GOを100回 | 両hash/scene一致、p95視覚差≤1frame、通常操作p95≤700ms |
| S-02 | +100ppm/-100ppm時計、1時間仮想走行 | 単調時間維持、sceneTickが仕様の同期目標内 |
| S-03 | UDP 5%損失、0–20ms遅延、重複/順序逆転 | revision後退なし、二重NEXTなし、未確定は暗転 |
| S-04 | PREPARE/READY/COMMIT/ACK各点で遮断 | 片側採用を成功扱いにしない、期限で障害暗転 |
| S-05 | Bの電源断/AP切断 | heartbeat消失1秒でA/Bが可能な範囲で黒、UI故障表示 |
| S-06 | A再起動 | Bは1秒で暗転、新bootIdへ再同期、勝手に点灯しない |
| S-07 | browser終了 | 再生継続、leaseのみ期限切れ |
| S-08 | 古いboot/nonce/seq/HMAC不正 | state変更なし |
| S-09 | Bのみ別LPK | CONTENT_MISMATCH、再生禁止、再配布で復帰 |
| S-10 | 暗転とGOを同時操作 | 暗転revisionが勝ち、古いCOMMITで点灯しない |
| S-11 | auto→manual→autoキュー | 累積時刻の評価がA/Bで一致、manualで待機 |
| S-12 | 停止/暗転/一時停止の違い | 時間進行が04通り、次回操作の位置が一致 |

実測はフォトダイオード/ロジックアナライザーが望ましい。無ければ十分高fpsの動画で同一色バー境界を100イベント比較し、撮影fpsによる分解能を明示する。30fps撮影で5ms誤差を測定できたと主張しない。

## 6. Flash・OTA・認証

LPK消去中/途中/検証後/NVS pointer更新前後、B採用直後/A採用前の各地点で電源断を注入する。再起動で旧または新の完全なデータだけを選び、不一致時は黒。slotの壊れたデータを表示しない。

OTAは途中切断、誤board、誤SHA、過大image、merged imageの誤投入、自己検査失敗、B更新後A更新前の電源断を試す。旧appへ戻れること、設定が保持されること、版不一致がUIに出ることを確認する。

未ログインの変更、無CSRF、他Origin/Host、WebSocket cross-origin、lease競合、login連打、node HTTP MAC不正/nonce再利用を拒否。診断・LPK・release成果物・WebログにgroupKey/password/session tokenが含まれないことを確認する。

## 7. Web受け入れシナリオ

W-01: PCで「今夜の一曲」という日本語、透明ロゴ、左スクロールを作り縦横を調整。manualキュー3つ作成、保存して再読込み。内容/ラスタ/曲順を保持。

W-02: スマートフォンで同じ `.ledproj` を読み、文字を再画像化せずLPK生成。PC版と同hash。1文字だけ修正してプレビューし保存するとhashが変化。

W-03: 2台へ配布、採用後は黒、GOと暗転解除で表示。NEXTを連打しても1操作1キュー。暗転は転送/予約待ちでdisabledにならない。

W-04: Wi-Fiを切ってもパネルはブラウザーから独立して動作。UIは接続断を表示。再接続時に勝手にGOしない。

W-05: 360px幅、200%文字拡大、キーボードのみで主要操作。入力中のSpace/矢印が本番を誤操作しない。

W-06: HTTP/IP直アクセス、外部通信を遮断、IndexedDB無効、WASM失敗、破損ZIP、Workerキャンセル。必要機能のfallbackまたは明確なエラーを確認。

## 8. 実機受け入れと計測

実際の電源・ケーブル・パネルを記録し、低輝度から四隅/単色/境界を確認。電流を測定しcapを設定。全面白を無条件に最大輝度で表示しない。起動/異常時の意図しない発光も確認。

最大許容資産/16レイヤー/両layout/スクロール/GIFを含むパッケージで2時間。render p95≤33.3ms、scan≥200Hz、内部heap/PSRAM予算を満たす。通信通常時のフレーム落ち率≤1%、watchdog reset=0、heapの継続減少なし。scan不足のときは色深度/CLK/blanking/DMAを調整し、設定と測定を記録する。

仕様上の最大入力で予算を満たせない場合は黙って限界を下げず、原因と代替案をADRに残して上限変更を本文/Schema/能力API/UI/例へ一括反映する。

## 9. 未検証事項と判断方法

| ID | 未確定/未検証 | 現時点の扱い | 実装時の解消方法 |
|---|---|---|---|
| G-01 | P5のscan/driver IC | 64×32・1/16/SHIFTREGを候補 | 現物刻印/購入仕様/行テスト。非互換ならHAL修正 |
| G-02 | Seengreat基板revision/enable回路 | 公開pin表の候補 | 基板刻印と公式回路図を照合 |
| G-03 | IDF5.5.5 + DMA3.0.14 native | 推奨開始点、未ビルド | M0 compile、必要patch固定 |
| G-04 | DMA内部メモリー/scanHz | 予算と目標値のみ | heap/DMA/走査を実測 |
| G-05 | 電源と輝度cap | 初期10%、上限約25% | 電流測定して機器別cap確定 |
| G-06 | USB setupの物理確認操作 | 03の要件を満たす手順を選ぶ | ROM downloadと通常起動を現物で区別 |
| G-07 | 混雑会場での無線品質 | 専用AP、近距離2台 | 設置距離/干渉条件を含む会場リハーサル |
| G-08 | 各端末の日本語文字ラスタ | 保存後はPNGが正本 | Windows/Android/iOSでW-01/W-02 |
| G-09 | 音入力codec/SD/RTC | v1外 | 拡張時に新しい能力と試験を追加 |

これらは仕様書作成を止める確認事項ではなく、実装時の検証ゲート。ユーザーへ再確認が必要になるのは、現物が根本的に非対応、用途を満たすために台数/データ上限/ネットワーク運用を変える場合など。

## 10. 最終納品チェックリスト

初版の完成条件に08のN-01–N-10を含める。2台の試作試験だけで増設対応の完了とはしない。16台の実機未検証は明示し、台数増加ごとに会場条件で実機検証する。

- firmware/editor/core/provisioningの全ソース、依存固定、ライセンス一覧。
- 初回USB書込み、任意IDのMaster/Follower設定、追加/交換、AP接続、編集/保存/配布/本番、復旧手順。
- bin/ota.bin/release.json/checksums、実際のoffsetによるflashコマンド。
- 単体テストとbrowser testの実行結果、実機試験の条件/測定値/未実施。
- Schema/例/本文の整合性、破壊的仕様変更と移行説明。
- 秘密を含まない診断サンプル、既知制約、次版候補。
