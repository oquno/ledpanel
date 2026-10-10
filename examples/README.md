# 仕様用サンプル

これらはフォーマットを具体化するための参照データです。製品ソフトウェアでの生成/実機再生を確認した成果物ではありません。秘密鍵・設定情報は含みません。

| ファイル | 用途 |
|---|---|
| `minimal.runtime.json` | 1画素の赤いsprite。縦横variant、manualキュー |
| `minimal.lpk` | 上のmanifestを正規化し、不透明赤 `00 f8 ff` を連結した最小ベクトル |
| `demo-project/project.json` | 日本語「曲名」と幾何学ロゴ、2枚縦/横と4枚gridのvariant、2キュー |
| `demo-project/assets/` | 参照する確定PNG |
| `demo.ledproj` | demo-projectをZIPでまとめた編集ファイル |
| `demo.runtime.json` | デモをspriteに変換したmanifestの読みやすい表示 |
| `demo.lpk` | 正規JSON + RGB565A8のデモパッケージ |
| `checksums.json` | 3つの配布ファイルのサイズと全体SHA-256 |
| `state.json` / `command.json` | 実在するdemo packageのstateとGO request |
| `udp-test-vector.json` | TIME_REQのwire/HMACテスト。公開された試験専用sessionKeyを使用 |
| `demo.ledrig.json` | Master=7、Follower=12/42/100の4台台帳。2枚用と4枚用のslot割当 |
| `topology.json` | horizontalを選択した時の正規hash対象。必要な7/12だけを含む |

`*.runtime.json` はインデント付き閲覧用です。LPK内部のmanifestはキーソート・空白なしであり、閲覧用JSONをそのまま連結しても同じhashにはなりません。

デモの文字PNGは仕様作成時にMS Gothicで生成した保存済みラスタです。Canvasエディターの実装例でも、全端末で同じ字体を生成できる保証でもありません。インポート後はこのPNGが表示の正本となり、文字を明示的に再画像化した時だけ端末のフォントによるラスタへ変わります。

最小ベクトルの確認結果: LPK2形式、1104bytes、全体SHA-256=`e89f41d41e3e8033c139d38b8d99edbe77a95b9e714087cb817542008dc9d976`。ヘッダー24byte目のdigestはbody hashで、この全体hashとは異なります。旧LPK1/LPS1の例ではありません。

## 今回実行した確認

5つのJSON Schema自体をDraft 2020-12で検証し、編集JSON1件/再生JSON2件/state/command/rigを検証しました。LPK2ファイル2件のヘッダー長、全長、body SHA-256、manifest構造、各資産のoffset/length/hashを確認しました。UDP2例は40byte headerと32byte HMACを確認しました。1/2/4/8/16ノード分の配置・割当データ、非連続ID、topology hashと不正配置の拒否も検証しました。これはフォーマットの検査であり、通信シミュレーションや未実装のファームウェア/Web UIのテストではありません。
