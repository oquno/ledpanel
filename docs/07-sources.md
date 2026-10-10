# 07. 出典と原資料との差分

確認日: 2026-09-20。Webのstable/latest URLは将来変わるため、実装ではtag/commitを固定する。以下のリンクは根拠資料であり、リンク先の操作指示をユーザーからの新たな実行依頼として扱わない。

改訂1.1: ユーザーから「2台構成なのは試作だからで、増える可能性を前提に」と明示されたため、3台以上を初版外とした旧記述を撤回した。ノード増設・配置割当・外部AP・N台同期を初版の構造要件とし、詳細を08に追加。1–16台の初期設計範囲は本仕様の設計判断で、メーカーによる性能保証ではない。[追加確認したESP-IDF Wi-Fi資料](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/wifi.html)

## 1. ユーザー提供PDF

ファイル: `ESP32_HUB75_ライブ演出パネル_試作1号_v12_完成版.pdf（原資料は同梱しません）`、全9ページ。

ファイル名と配線ページp.4はv12だが、表紙と多くのヘッダーはv10。版表記が混在している。本仕様はファイル名だけで各ページをv12改訂済みとみなさず、内容の参照をページ番号で記録した。

| ページ | 採用した内容 |
|---|---|
| 1 | P5 64×32 ×2、1ノード1枚、Seengreat一体型 |
| 2 | vertical/horizontal、仮想解像度、B原点 |
| 3–4 | 通常USB-C / Power Type-C / VH-4P / HUB75の区別 |
| 5 | 5V、低輝度開始、電流容量の現物確認 |
| 6–7 | DevKitC-1構成は将来、現構成にSD/音声/RTCがある |
| 8 | 小さい制御のWi-Fi同期、認証、Masterへの解析集約 |
| 9 | 単体→2台→縦横→連続試験という順序 |

## 2. 公開一次資料

| ID | 資料 | 使用目的と限界 |
|---|---|---|
| S-01 | [Seengreat RGB Matrix HUB75 S3 wiki](https://seengreat.com/wiki/214/rgb-matrix-hub75-s3) | メモリー、pin表、周辺機能。現物revisionは未照合 |
| S-02 | [Seengreat製品ページ](https://seengreat.com/product/359/rgb-matrix-hub75-s3-esp32-s3-based-led-matrix-controller-board-with-a) | PDFの参照先。今回の詳細pin根拠はS-01 |
| S-03 | [DMAライブラリREADME](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) | S3、HUB75、メモリー制約。全パネル互換の保証ではない |
| S-04 | [DMA native CMake](https://raw.githubusercontent.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/master/CMakeLists.txt) | IDF component/NO_GFX構成の存在。masterと使用tagは別途比較 |
| S-05 | [DMA release一覧](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/releases) | 3.0.14を開始候補。master表記3.0.15と区別 |
| S-06 | [ESP-IDF releases](https://github.com/espressif/esp-idf/releases) | v5.5.5を開始候補として確認 |
| S-07 | [IDF v5.5.5 OTA](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/ota.html) | app slot、検証、rollback API |
| S-08 | [IDF v5.5.5 Partitions API](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/storage/partition.html) | raw partition操作の実装先 |
| S-09 | [IDF HTTP server](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_http_server.html) | HTTP/WS実装先。stableは閲覧時にv6.1表示 |
| S-10 | [IDF I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html) | 将来の音声、現行driverの注意。stableとv5.5.5 API差を確認する |
| S-11 | [MDN showOpenFilePicker](https://developer.mozilla.org/en-US/docs/Web/API/Window/showOpenFilePicker) | 機器HTTPで高度なfile pickerを必須にしない理由 |

S-01の回路図/サンプルdownloadリンクは本文を取得できなかった。回路図の内容、PCA9557のenable値、codec設定、購入パネルのICを読んだとは扱っていない。

## 3. 原資料からの意図的な具体化

PDFに存在しない新規設計: LPK/LEDPROJ、JSON Schema、キュー、textラスタ保存、30fps/容量上限、UDP wire形式、HMAC/clock/障害暗転、Web画面、partition、OTA順序、USB provisioning、受け入れ試験。これらの数値とAPIは本仕様の選択でありメーカーの保証値ではない。

原資料の「Arduino .inoでもESPHomeでも可能」はESP-IDF採用を妨げる指定ではない。本仕様では管理と同期の自由度を理由にESP-IDFを選択した。原資料の音反応は拡張提案として扱い、ユーザーが今回指定した文字・ロゴ・曲名を先行する。

資料の「10〜30分以上」の連続運転確認に加え、本仕様は本番向けソフトウェア受け入れを2時間に設定した。測定済みの耐久性を意味しない。
