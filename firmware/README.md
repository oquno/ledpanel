# ESP32-S3ファームウェア

対象はSeengreat RGB Matrix HUB75 S3（N16R8）、P5 64×32パネルです。ESP-IDF **5.5.5**でビルド確認しています。HUB75ドライバーは `components/hub75/` に同梱し、上流のコミットと変更点を [UPSTREAM.md](components/hub75/UPSTREAM.md) に記録しています。

## ビルド

ESP-IDF 5.5.5をインストールし、ESP-IDFのシェル／環境を有効にします。リポジトリのルートから実行してください。

```sh
idf.py -C firmware build
```

対象ESP32-S3、16MB Flash、8MB PSRAMの設定は `sdkconfig.defaults` にあります。Web UIはビルド時にソースから埋め込みます。ビルド結果は `firmware/build/` に生成され、Gitに含めません。

## 初回書込み

```sh
idf.py -C firmware -p COM3 flash
```

COM3を実際のポートに置き換えます。初回や自動リセットできない接続では、BOOTを押したままUSBを挿し直して書込みモードに入れる必要があります。パーティション構成を変更する場合は、保存データとの互換性を確認してください。

## 設定を残してアプリだけ更新する（Windows）

既に本プロジェクトの `partitions.csv` を使っている機器向けです。ESP-IDF環境で、リポジトリのルートから実行します。

```powershell
./ApplyFirmware.ps1 -Port COM3 -DryRun
./ApplyFirmware.ps1 -Port COM3
```

`firmware/build/ledpanel_firmware.bin` をアプリ領域 `0x20000` へ書き込みます。初回書込みには使わないでください。保存したWi-Fi・表示データを保持します。イメージを変える場合は `-Image` を指定します。Pythonは `-Python` で指定できます。

設定とペアリングは [START_HERE.md](../START_HERE.md)、表示・本体操作は [TWO_PANELS.md](../docs/TWO_PANELS.md) を参照してください。

## 表示と同期

描画は親機の共通時計による30fpsです。2台のUSB描画番号比較では最大1フレームの差でした。光学的な走査位相を測定した結果ではありません。時刻同期が失効した追加パネルは再同期まで黒を出します。

HUB75信号クロックは8MHz、最低更新頻度は240Hzです。低位階調の点灯時間を圧縮するため暗部階調との交換条件があります。撮影時の縞の軽減量は未測定です。

表示・明るさ・消灯は操作が落ち着いてから保存します。操作後3秒待って電源を切ってください。2台構成の復元・片側再起動を確認済みです。本体スイッチの実際の押下は機種・配線に依存するため、導入時に確認してください。
