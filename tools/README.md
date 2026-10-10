# 開発ツール

`python tools/lpk.py decode examples/demo.lpk` はLPK2を再検証してmanifestとpackageHashを表示します。`validate_examples.py` はサンプルのheader、ハッシュ、asset境界を検証します。

`python tools/provision.py create-group --output provisioning-secrets/group.private.json` でAP PSK、管理password、groupKeyを生成します。先に保存先ディレクトリを作ってください。生成ファイルはリポジトリへコミットしません。USB通信には `python -m pip install -r tools/requirements-hardware.txt` が必要です。初回書込みとアプリだけの更新は [ファームウェア手順](../firmware/README.md) を参照してください。

開発用Web UI: `python tools/dev_server.py`。テスト: `python -m unittest discover -s tests -p "test_*.py"` と `python tools/test_browser.py`。Node.js不要。詳細は [技術選定](../docs/09-technology-selection.md)。

外部Wi-Fiを使う場合は `create-group --network-mode infrastructure --ssid "Wi-Fi名" --output provisioning-secrets/group.private.json` で作成します。Wi-Fiパスワードは非表示で入力します。`configure` に `--network-mode infrastructure` を指定し、追加側では `--master-address` と `--master-node-id` を指定してください。親機にはDHCP予約などで安定したIPを割り当てます。`pair-follower` にも同じネットワーク方式と親機アドレスを渡します。

公開準備: `python tools/check_publication.py` で作業ツリーを検査し、`python tools/export_public.py` で公開用ZIPを生成します。[公開手順](../docs/PUBLISHING.md) を参照してください。
