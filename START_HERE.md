# セットアップと表示手順

## 1. ファームウェアを入れる

[ビルドとUSB書込み](firmware/README.md) に従って各コントローラーへ書き込みます。初回は `idf.py flash`、既にこのプロジェクトのパーティション構成を使っている機器の更新は `ApplyFirmware.ps1` が使えます。

## 2. 親機を設定する

Python環境にUSB通信ライブラリを入れます。以下のCOM3・COM4は例です。実際のUSBポートに置き換えてください。Linuxでは `/dev/ttyACM0` などになります。

```sh
python -m pip install -r tools/requirements-hardware.txt
```

秘密設定の保存先を作り、グループと親機を設定します。ディレクトリはWindowsではエクスプローラーやPowerShellで作れます。

```sh
mkdir provisioning-secrets
python tools/provision.py create-group --output provisioning-secrets/group.private.json
python tools/provision.py configure --port COM3 --group provisioning-secrets/group.private.json --node-id 1 --role master
```

設定ツールの案内が出たら、USBを挿したまま親機のBOOTを3秒押して離します。通常の表示操作ではBOOTは使いません。設定後は案内に従って再起動してください。`create-group` は既存ファイルを上書きしません。秘密ファイルは保存し、公開・コミットしないでください。

## 3. 2枚目を登録する（2枚の場合）

両方をUSBにつなぎ、接続確認を実行します。

```sh
python tools/provision.py pair-follower --master-port COM3 --follower-port COM4 --group provisioning-secrets/group.private.json --node-id 2 --label "Panel B" --dry-run
```

問題がなければ `--dry-run` を外して実行します。案内に従い、追加側、親機の順でBOOTを3秒押して離します。同じ秘密ファイルを両方に使います。各パネルへ個別に5Vを給電してください。

## 4. ログインして表示する

1. 設定ツールが案内した親機のWi-Fi（`LEDPANEL-` で始まる名前）へ接続します。Wi-Fiのパスワードは秘密ファイルの `apPsk` です。
2. [http://192.168.4.1](http://192.168.4.1) を開きます。
3. 秘密ファイルの `adminPassword` でログインします。
4. 「デザイン」で文字や画像を作り、プレビューを確認します。
5. 「この内容を表示」を1回押します。転送・切替・再生が自動で進みます。

2枚の場合は「機器」で2台の接続と表示位置を確認し、必要なら構成を保存します。[配置の説明](docs/TWO_PANELS.md) を参照してください。外部Wi-Fiを使う設定は [開発ツール](tools/README.md) を参照してください。

## 表示できないとき

- PC上の開発サーバーではなく、親機の画面を開いているか確認します。
- 表示ボタン下の理由を確認します。追加パネルの接続待ちなら、追加側の電源と同じグループへの登録を確認します。
- 「本番」で明るさを確認します。消灯状態で保存した場合は、次の起動も消灯です。「この内容を表示」で表示を再開できます。
- パネル専用電源とHUB75の入力側への接続を確認します。

最後の表示は電源を入れ直しても復元します。操作後3秒待ってから電源を切ってください。アニメーションは先頭から再開します。
