# 今回の仕様成果物の検証記録

実施日: 2026-09-20。対象は仕様文書と参照データ。製品ファームウェア/エディターの実装・ビルド・実機書込みは行っていない。

## 実行して確認したこと

- ユーザー提供PDF全9ページのテキストを抽出。構成・配線・ソフトウェアに関係するページを画像でも確認。
- Seengreat、DMAライブラリ、ESP-IDF等の公開一次資料を確認。リンク先を本文に記録。
- project/runtime/state/command/rigの5 Schema自体をDraft 2020-12で検証（改訂1.1）。
- 編集JSON1件、再生JSON2件、state1件、command1件が各Schemaに適合。
- シーン/キュー/資産の参照、ID一意性、variant集合、トラック時刻、レイヤー表示期間をサンプルで照合。
- `.ledproj` 内のファイルが展開版と一致し、PNGの寸法・SHA-256と編集JSONが一致。
- LPK2件のヘッダー64bytes、長さ、body hash、manifest、資産offset/length/hashを独立再読込みで照合。
- stateのpackageHashがdemo.lpk全体のhashに一致。
- UDP参照packetの40byte header、32byte HMAC、payload/全長を検算。
- 未知フィールド、未知version、manual cueの不正時間、opacity超過、mode/ID不整合、数値型tick、command不正引数の7ケースをSchemaが拒否。
- Flash partitionの重複なし、16MiB内への収まり、予約領域開始位置を検算。
- 日本語ラスタの拡大画像を確認。

改訂1.1では1/2/4/8/16台のlayout/rigデータ、Master=7を含む非連続ID、4枚grid、slotと実機の分離、topologyHashを検証した。slot重複/範囲外/二重割当/hardwareId衝突の4ケースが意味検証で拒否されることを確認。データ形式2、同期wire2へ参照例を再生成した。ネットワークのN台シミュレーションは未実施。

使用した検証環境はバンドルPython、Pillow、jsonschema 4.26.0。これらは仕様作成の補助環境であり、製品の依存関係が確定したことを意味しない。

## 未実施

ファームウェア/フロントエンドのビルド、USB接続、走査・GPIO・電流・輝度確認、2台のWi-Fi同期、OTA、実browserでの編集、WASMとnativeの比較、会場リハーサル。実施が必要な条件は `06-verification.md` に定義した。

今回の検証から主張できるのは、**仕様の機械可読部分と添付例が整合していること**であり、実機性能や全実装の正しさではない。


## 2026-09-24 エディター拡張検証

Python unittest 5件、ヘッドレスEdgeのブラウザーテスト20件、既存LPKのheader/hash/asset境界検証が成功。文字色の実画素、四方向・反復/単発・最終部分周期、点滅/グローの整数補間、sequenceの境界、元画像からの再縮小、複製の画像分離、Undo/Redo、保存/読込、破損読込時復元、RGB565A8コンパイルを検証。自作1秒WebMの赤/青フレーム抽出と動画保存/読込/コンパイルも成功。

ブラウザーの仮想時間はメディア復号を待てないため、標準ライブラリのローカルCDPドライバーへ変更。テストは45秒でタイムアウトし、失敗・未完了は非0終了。UIを1440×1000と390×844で目視確認。これらはホスト側の検証であり、ESP32の再生・色一致・同期・DMA出力の検証ではない。
