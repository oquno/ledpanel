# GitHubへの公開手順

## 公開用一式を作る

リポジトリのルートで実行します。

```sh
python tools/check_publication.py
python tools/export_public.py
```

`output/ledpanel-public.zip` が生成されます。対象は追跡中とGitの除外規則に該当しない未追跡のソースです。`.git`、秘密設定、実機のFlashバックアップ、ビルド結果、開発用SDK、過去の内部記録は含めません。ZIPにはファイル一覧とSHA-256の `PUBLICATION_MANIFEST.json` を付けます。

この作業用リポジトリの古い履歴には個人ファイルパスが残っています。既存の `.git` をそのままpushせず、ZIPを別の空ディレクトリへ展開して新規公開用リポジトリを作ってください。元の開発履歴や秘密ファイルはローカルに保持します。

## 新規リポジトリを作る

展開先で次を実行します。

```sh
git init
git add .
git diff --cached --stat
git commit -m "Initial public source release"
git branch -M main
```

GitHubで空のリポジトリを作り、表示された自分のURLを登録します。

```sh
git remote add origin https://github.com/YOUR_ACCOUNT/YOUR_REPOSITORY.git
git push -u origin main
```

独自ソースはMITライセンスです。ルートの `LICENSE` と同梱HUB75ライブラリのMIT表記を保持してください。

## 公開前の確認

- READMEの対応機器・実装範囲が公開版に合っていること。
- ZIPに秘密鍵・実機設定・個人のプロジェクト・バックアップがないこと。
- テストが成功すること。CIのテストは模擬APIの検証です。
- MITの著作権表示と同梱ライブラリのライセンスが含まれていること。

公開用出力はローカルで生成するだけです。このコマンドがGitHubへアップロードすることはありません。
