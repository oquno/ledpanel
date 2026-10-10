# 09. Node.jsを必須にしない技術選定

> 設計時の選定案を含みます。現在の実装はESP-IDF 5.5.5と同梱の `esp-hub75` です。実際のビルド手順・依存関係は [ファームウェアREADME](../firmware/README.md)、確認範囲は [実装状況](IMPLEMENTATION_STATUS.md) を参照してください。

更新: 2026-09-20。ユーザーの「Nodeはできれば避けたい」という希望を反映。旧React/Vite/pnpm案より優先する。表示内容、データ形式、増設・同期・認証・描画一致の要件は変更しない。

## 採用構成

| 対象 | 技術 | 実行場所 |
|---|---|---|
| Web UI | HTML / CSS / JavaScript ES modules、Canvas、ブラウザ標準API | 操作端末のブラウザ |
| 開発用配信・模擬API | Python 3.12標準ライブラリ | 開発PCのみ |
| データ検証・書込み補助 | Python | 開発PCのみ |
| ファームウェア | C++17 / ESP-IDF / CMake | ビルドはPC、実行はESP32-S3 |
| JSテスト | ブラウザ内テストページ。PythonからヘッドレスEdge/Chromeを起動可能 | 開発PC |
| Pythonテスト | unittest | 開発PC |
| 共通描画コア（未実装） | C++。Web用はClang/LLDによるstandalone WASMと手書きのJS呼出し層 | ESP32 / ブラウザ |

Node.js、npm、pnpm、Vite、React、TypeScript変換を必須にしない。JSはビルドせず配信できるES modulesにし、外部CDNに依存しない。ブラウザ内JavaScriptは引き続き使用する。

本番ではMaster ESP32のHTTPサーバーに同じ静的Web資産を組み込み、PCのサーバーを常駐させない。Python開発サーバーをESP32上で実行する構成ではない。Web資産の組込み・圧縮処理はPython/CMakeで実装する（現時点では未実装）。

共通C++描画コアはlibc/OS依存を避け、WASMのメモリと呼出し関数を明示し、Node.jsを要求する生成ランタイムに依存しない。Clang/LLDのバージョン固定・ビルド・C++とWASMのgolden一致は今後の実装ゲート。現在のCanvas描画は代替の試作実装。

## 代替案との比較

- Pythonを採用: 既存の検証・書込み補助もPythonで、開発用HTTP配信とJSON APIなら標準ライブラリで足りる。追加サーバーフレームワークは導入しない。
- Go / Rust: PC用単一実行ファイルの配布が必要になった場合の候補。今は追加言語・ビルド環境を増やす利点が小さい。
- Deno / Bun: Node以外のJS実行環境だが、この用途では追加JSランタイム自体が必要ないため採用しない。
- 単純な静的配信だけ: UI配信には足りるが、実機なしの操作確認用に小さな模擬APIも残す。

## 開発手順

リポジトリ直下で実行。サーバーと今回のテストはpip追加インストール不要。

```powershell
python tools/dev_server.py
# ブラウザで http://127.0.0.1:4173 を開く
python -m unittest discover -s tests -p "test_*.py"
python tools/test_browser.py
python tools/validate_examples.py
```

`test_browser.py` はインストール済みEdge/Chrome/Chromiumを使う。見つからなければ `--browser <実行ファイル>` を指定するか、開発サーバーの `/tests/browser/index.html` を手動で開く。失敗・タイムアウトは非0終了。ブラウザの自動ダウンロードやNode実行はしない。

開発サーバーは127.0.0.1に限定。`--port` で変更可能。`--rig <秘密を含まない台帳JSON>` を指定でき、既定はnodeId 7/12/42/100の4ノード例。ノード数を2に固定しない。台帳の完全な仕様検証や実機同期は未実装。

模擬APIはメモリ内のhealth/status/nodes/rig/capabilitiesと一部コマンドのみ。再起動で状態は消える。機器への転送・制御や本番の認証/lease/ACK/将来時刻同期は行わない。未対応コマンドはエラーとし、GOで暗転を自動解除しない。

`packages/formats/index.mjs` はNode専用BufferからUint8Array/Web Cryptoに変更。`sha256`、`hmacSha256`、`encodeLpk`、`decodeLpk`、`compileRuntime` はPromiseを返すため `await` が必要。

ESP32のHTTP配信ではWeb Cryptoのsecure context要件を満たさない場合がある。`randomUUID` は `crypto.getRandomValues()` に、SHA-256/HMACはJavaScript実装にフォールバックし、Web Cryptoが使える場所では引き続きネイティブAPIを優先する。ESP32のHTTP originで`crypto.subtle`がundefinedのEdge実ブラウザーからダイジェストを検証済み。

## 根拠

- [Python公式 http.server](https://docs.python.org/3/library/http.server.html): 開発用配信に使用する標準ライブラリ。本番公開用サーバーにはしない。
- [MDN JavaScript modules](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Guide/Modules): ブラウザからES modulesを直接読み込める。
