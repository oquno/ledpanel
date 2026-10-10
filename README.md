# LED Panel

ブラウザーで文字・画像・アニメーションを作り、Wi-Fi経由でHUB75 LEDパネルに表示するプロジェクトです。ESP32-S3ファームウェア、エディター、USB設定ツール、データ形式の仕様をまとめています。

## できること

- 文字・画像・簡単な図形、スクロール・点滅・グロー点滅の編集とプレビュー。
- 「この内容を表示」から転送・切替・再生まで一括実行。
- 2枚の横配置（128×32）と縦配置（64×64）、親機からの同期再生。
- 電源再投入時の表示・明るさ・消灯状態の復元。
- 親機の本体スイッチで明るさ、消灯・復帰、次のシーンを操作。

実機で確認した構成は **Seengreat RGB Matrix HUB75 S3（ESP32-S3 / Flash 16MB / PSRAM 8MB）とP5 64×32パネル、コントローラー2台**です。1台につきパネル1枚を接続します。最大16ノードは設計上の対象で、16台での実機動作を確認したものではありません。SD・音声・RTCや共有WASM描画コアは未実装です。スマートフォン撮影時の縞を完全に消す保証はありません。

## PCで試す

Python 3.12以降とChrome・Edge・Chromiumなどのブラウザーを使います。Node.jsは不要です。

```sh
python tools/dev_server.py
```

[http://127.0.0.1:4173](http://127.0.0.1:4173) を開きます。Windowsでは `start-editor.bat` でも起動できます。このサーバーは模擬機器で、実機へは転送しません。実機の操作は親機が配信する管理画面で行います。

## 実機で使う

[初回セットアップと表示手順](START_HERE.md) にビルド・USB設定・ログイン・表示の順序をまとめています。[2枚の配置と本体操作](docs/TWO_PANELS.md)、[詳しい編集操作](docs/EDITOR_GUIDE.md) も参照してください。

パネル用5V電源はPC接続用USBとは別に必要です。基板・パネルの電源仕様と配線を確認してから通電してください。対応外のドライバーICや走査方式では表示できない場合があります。

## 開発と検証

```sh
python -m unittest discover -s tests -p "test_*.py"
python tools/test_browser.py
python tools/validate_examples.py
python tools/check_publication.py
```

ファームウェアのビルドにはESP-IDF **5.5.5**を使います。手順は [firmware/README.md](firmware/README.md)。設計仕様と現在の実装範囲は [実装状況](docs/IMPLEMENTATION_STATUS.md) で区別しています。

| ディレクトリ | 内容 |
| --- | --- |
| `apps/editor/` | ブラウザーの編集・プレビュー・転送画面 |
| `firmware/` | ESP-IDFファームウェアと同梱HUB75ドライバー |
| `packages/` | データ形式・描画のJavaScriptモジュール |
| `tools/` | 開発サーバー、USB設定、検証・公開用出力 |
| `schemas/`, `examples/` | JSON Schemaと公開用参照データ |
| `tests/` | Python・ブラウザーのテスト |
| `docs/` | 操作説明と設計仕様 |

## GitHub公開について

[公開手順](docs/PUBLISHING.md) に従って `python tools/export_public.py` を実行すると、秘密設定・実機バックアップ・開発環境・古いGit履歴を含まない公開用ZIPを生成します。既存の履歴にはローカルの個人ファイルパスがあるため、新規リポジトリにはこの公開用一式を使ってください。

貢献方法は [CONTRIBUTING.md](CONTRIBUTING.md)、秘密情報の扱いは [SECURITY.md](SECURITY.md)。本プロジェクトは [MIT License](LICENSE) で公開できます。同梱ライブラリのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
