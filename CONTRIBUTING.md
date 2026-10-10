# 開発への参加

Python 3.12以降とChrome・Edge・Chromiumを用意し、`python tools/dev_server.py` でエディターを起動します。ファームウェア開発にはESP-IDF 5.5.5を使います。

変更した範囲に応じて検証してください。

```sh
python -m unittest discover -s tests -p "test_*.py"
python tools/test_browser.py
python tools/validate_examples.py
python tools/check_publication.py
```

ファームウェアを変更した場合は `idf.py -C firmware build` も実行します。実機検証を報告するときはボード・パネル・台数と観察結果を記載し、USBやAPIの状態確認と目視確認を区別してください。秘密設定や実機バックアップは添付しないでください。

データ形式を変更する場合はSchema・参照例・読込処理の互換性をまとめて確認します。仕様の分割文書を編集した後は `python tools/build_spec.py` で統合版を更新してください。同梱ライブラリへの変更は `UPSTREAM.md` に記録します。

プロジェクト独自のソースは [MIT License](LICENSE) です。変更・再配布時もライセンスと著作権表示を保持してください。
