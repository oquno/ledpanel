# 02. 編集データ・再生データ・描画仕様

## 1. 共通規則

文書改訂1.1、データ形式major=2、minor=0。JSONはUTF-8、BOMなし、重複キー禁止。全数値は整数。浮動小数、NaN、Infinity、指数表記を生成しない。受信側もSchemaと同じ整数範囲で検証する。指定されない追加プロパティは拒否する。JSON SchemaはDraft 2020-12。

IDは `[a-z][a-z0-9_-]{0,31}`、シーン・キュー・資産・レイヤーでそれぞれ一意。レイヤーIDはvariant内で一意。UUIDは小文字標準表記。文字列にNULを含めない。表示用文字はNFCに正規化、改行はLF。パスはASCIIの資産IDから組み立て、ユーザーの元ファイル名をパスに流用しない。

初版実装はversion=2、runtimeMinor=0だけを受け入れる。不明な版や機能を部分表示しない。将来版をインポートすると元ファイルを保持し、対応エディターが必要と表示する。

## 2. 編集形式 `.ledproj`

通常ZIP（ZIP64なし）を用い、圧縮method=0または8を受け入れる。MIMEは `application/vnd.ledpanel.project+zip`。構成は次のとおり。

```text
project.json
assets/<assetId>.png
assets/<assetId>.jpg
```

`project.json` が正本の編集情報。内部資産以外のURL参照、シンボリックリンク、絶対パス、`..`、重複パス、暗号化ZIP、多重ZIPは拒否する。ブラウザーで展開前にサイズを検査し、展開中も総量をカウントする。圧縮ファイル20MiB、展開総量64MiB、エントリー1025、project.json 1MiB、単一画像の復号4096×4096または16,777,216画素を上限とする。後述のruntime用ラスタにはさらに厳しい寸法制限がある。

JSONの構造は `schemas/project.schema.json`。概要は次の表を正本Schemaと併用する。

| フィールド | 意味 |
|---|---|
| version | 2固定 |
| projectId | UUID。新規プロジェクト時に発行、通常保存では維持 |
| name | 1–80文字のプロジェクト名 |
| layouts | 1–16個のlayout定義。id/name/width/height/slots、詳細08 |
| defaultLayout | layouts内のid |
| assets | id、path、mime、role、width、height、sha256 |
| scenes | id、name、durationTicks、playback、variants |
| cues | id、name、sceneId、advance、holdTicks |

role=`original`は編集での再加工用、`raster`は表示用に確定したPNG。originalはPNG/JPEGのみ、rasterはPNGのみ。sha256はZIPから取り出したファイルそのもののSHA-256小文字16進64桁。画像ヘッダーの実寸とJSONの寸法を照合する。未参照originalは保持できる。runtimeへは参照されたrasterだけを出力する。

GIFは取込時にフレーム合成とdisposal処理を完了してRGBA PNG列へ正規化する。元GIFを `.ledproj` に残さず、sequenceとPNGで再編集可能にする。透明色・フレーム部分矩形を合成した各時点の完全画像を保存する。GIF delayは各フレームについて `max(1,floor(delayMs*30/1000+0.5))` tickへ量子化する。0msは1tick。変換後の総時間の差をUIに表示する。512資産/パッケージ上限を越えれば取込を確定しない。

## 3. シーン・variant・レイヤー

scene.durationTicksは1–18,000。scene.playbackは`loop`または`hold`。シーン開始後の整数elapsedTickに対し、sceneTickはloopなら `elapsedTick % durationTicks`、holdなら `min(elapsedTick,durationTicks-1)`。負のelapsedTickは描画せず黒。

variantsは任意layoutIdをキーとするオブジェクト。vertical/horizontalは2台用プリセットIDで特別な分岐を持たない。プロジェクトが宣言した各layout用variantを全シーンが必ず持つ。未宣言layoutのvariantは拒否する。UIの下書きでは一時的な欠落を許容してもよいが、保存時には「未完成」と区別し、配布・正式エクスポートを禁止する。v1の正式 `.ledproj` は完全なものだけを書き出す。

variant.layersは0–16個。配列の先頭が最背面。空variantは黒。キャンバスは毎フレーム黒で初期化する。

共通プロパティ:

| フィールド | 型・範囲 | 意味 |
|---|---|---|
| id/name | ID / 1–80文字 | 編集対象の識別 |
| kind | rect/text/image/sequence | 編集レイヤー種別 |
| x,y | -4096–4096 | 左上位置。負座標を許可 |
| opacity | 0–255 | 255が不透明 |
| startTick,endTick | 0以上、end>start | `[start,end)`の間だけ表示 |
| tracks | x,y,opacityの任意サブセット | 空オブジェクト可 |

start/endはシーン内時間、end≤durationTicks。トラック時刻もシーン内時間で0–durationTicks。描画領域はキャンバスへクリップ。中心基準・自動折返し・ノードごとの座標解釈は存在しない。

### 3.1 rect

`width,height`は1–2048 / 1–2048、`color`は大文字 `#RRGGBB`。矩形の整数座標領域 `[x,x+w) × [y,y+h)` を塗る。輪郭・角丸・回転はv1なし。

### 3.2 text

`text`は1–256 Unicodeコードポイント、LFによる改行可。`fontFamily`はsans-serif/serif/monospace、`fontSizePx`は6–64、`fontWeight`は400/700、`color`は`#RRGGBB`、`align`はleft/center/right、`lineHeightPx`は6–96、`letterSpacingPx`は0–16、`boxWidth`は1–2048、`rasterAssetId`は確定したraster PNGを指す。

ブラウザーのCanvas2Dで画像化する。システムフォントを使い、外部CDN・Google Fontsへの実行時アクセスをしない。日本語書体は端末に存在する書体になる。フォントプレビューを見て確定する運用とする。一般的な日本語端末での入力を受け入れ条件に含める。

1文字はUnicodeの拡張書記素単位で扱う。絵文字・結合文字をコード単位で分割しない。各行を `Intl.Segmenter` で書記素分割し、Canvas.measureTextのadvanceとletterSpacingPxで順に配置する。行幅は各advance合計+隙間*(文字数-1)。left=0、center=floor((boxWidth-lineWidth)/2)、right=boxWidth-lineWidth。CanvasのtextBaseline=`top`、textAlign=`left`、fillTextへmaxWidthは渡さない。各行のy=lineIndex*lineHeightPx。ラスタ高さは行数*lineHeightPx。右端・下端で明示的にクリップする。

自動折返しなし。UIで改行を編集する。文字が枠を越えたときは枠の拡張またはスクロールを案内する。文字描画のブラウザー差は許容するが、**確定ラスタを保存した後の再生差は許容しない**。

フォント指定・本文・色・枠幅・行高・字間を編集した時だけrasterを再生成する。既存プロジェクトの読込み、座標変更、スクロール調整だけでは再画像化しない。端末を替えた再編集で「文字画像を再生成」した場合はプレビューの差を表示する。保存済みPNGが欠けているtextは検証エラー。フォントがなくても保存済みラスタから再コンパイルできる。

runtimeではtextはspriteになる。UTF-8文字列、フォント名、フォントエンジンはESP32へ送らない。これによりフラッシュ容量を日本語フォントへ割かず、元テキストを失わずに再編集できる。

### 3.3 image

`assetId`がrasterを参照し、`originalAssetId`はoriginalまたはnull。JPEGのEXIF orientationは取込時に適用、色はsRGB、ICC変換はブラウザーのデコード結果をPNGへ固定する。

拡大縮小・トリミング・fit操作はエディターでPNGへ焼き込む。nearestまたはbilinearの結果をプレビューし、生成PNGを新しいrasterとして保存する。runtimeは1:1の画素描画しか行わない。透明PNGロゴはアルファを保持。JPEGは不透明。

### 3.4 sequence

`frames=[{assetId,ticks},...]`、1–512フレーム、各ticks=1–18,000、総ticks≤18,000、全資産の寸法は同一。`repeat=loop|hold`。ローカル時間は `sceneTick-startTick`。loopは総ticksで剰余、holdは総ticks-1でクランプし、累積区間 `[sumBefore,sumAfter)` に対応するフレームを選ぶ。シーン境界で初期状態へ戻る。

動画の試験取り込みはブラウザー対応コーデックをPNG列へ復号し、このsequence形式を使用する。元動画・音声は編集ZIPへ含めない。入力100MiB、区間6秒、120フレーム、取り込み1回700000画素を上限とし、フレーム時間を `max(1,round(durationSeconds*30))` で量子化する。元の画角を保って選択layout内へ縮小する。取り込み上限とは別に、コンパイル時に全体の資産数・画素数・パッケージ容量を検査する。動画取り込みはv1完成の必須条件ではない。

### 3.5 キーフレーム

tracksの各配列は `[{tick,value,ease},...]`、1–64要素、tickは厳密昇順、同一tick禁止。ease=`linear|step`はそのキーから次のキーへの区間に適用。最後のキーのeaseはstepに固定する。

キーより前は最初のvalue、最後の後は最後のvalue。`[t0,t1)` においてstepはv0、linearは `v0 + floor((v1-v0)*(t-t0)/(t1-t0))`。floorは負数でも負の無限大方向であり、C++の整数除算の0方向丸めをそのまま使わない。中間計算は符号付き64bit。該当トラックがあると共通プロパティの初期値を置き換える。

スクロールプリセットは左/右をx、上/下をyトラックへ展開する。2026-09-24のエディター更新では速度指定に代わり周期Pを整数tick（4–18,000）で指定する。左/上はキャンバス幅/高さから負の画像幅/高さまで、右/下はその逆へ移動する。表示開始をsとして、sで画面外開始位置、s+P-1で画面外終了位置を置き、繰返し時はs+Pで開始位置へ戻す。終了位置から次の開始位置まではstep、移動区間はlinear。表示終了で周期が途切れる場合も、その時刻までの補間値をキーとして保存する。シーン期間・playbackはプリセット適用では変更しない。シーン境界では既存loop/hold規則に従う。

点滅はopacityのstepキーで周期前半をレイヤーopacity、後半を0とする。グロー点滅は周期開始0、floor(P/2)でレイヤーopacity、P-1で0のlinearキーへ展開する。これは明滅であり、ぼかし・加算合成の拡張ではない。移動と点滅は併用可能。プリセットの繰返しは表示区間内へキーを展開し、既存64キー/トラック・512キー/variant制限を超える場合は変更を確定しない。新しいレイヤーフィールドやruntime形式の追加はない。

v1の曲切替はcut。フェード表現はopacityトラックで作る。シーン間crossfadeは将来拡張。

## 4. キュー

`cues`は1–128個で配列順が曲順。id、name、sceneId、advance、holdTicksを持つ。advance=`manual`ならholdTicks=null、`auto`なら1–108,000。デフォルトはmanual。sceneのループとキューの滞在時間は別物である。

キューの自動送りは04の時刻基準で計算する。最後のキューを越えたらSTOPPEDになり黒。自動で先頭にループしない。ライブで1曲を何分表示するか不明でもmanualなら表示が維持される。

## 5. 再生形式 `.lpk`

MIMEは `application/vnd.ledpanel.package`。ZIPではなく、64byteヘッダー + manifest JSON + asset blobの連続バイト列。JSON構造は `schemas/runtime.schema.json`。

| offset | byte長 | 内容 |
|---|---|---|
| 0 | 4 | ASCII `LPK2` = 4c 50 4b 32 |
| 4 | 2 | schemaMajor=2、little endian |
| 6 | 2 | schemaMinor=0、little endian |
| 8 | 4 | headerSize=64、little endian |
| 12 | 4 | manifestLength、little endian |
| 16 | 4 | blobLength、little endian |
| 20 | 4 | totalLength=64+manifestLength+blobLength |
| 24 | 32 | SHA-256(manifest bytes + blob bytes) |
| 56 | 4 | flags=0 |
| 60 | 4 | reserved=0 |

サイズ計算は64bitで行い、ファイル長と一致しない場合は拒否。manifestLength≤131,072、totalLength≤2,621,440。末尾への付加データも拒否。ヘッダー24のdigestは内容の整合性、`packageHash`は**LPKファイル全体**のSHA-256。別物として扱い、APIと同期にはpackageHashを使う。ハッシュは認証を代替しない。

manifestにはversion=2、runtimeMinor=0、renderer=`lp-raster-1`、fps=30、projectId、layouts、defaultLayout、assets、scenes、cuesを保存。nameはシーン/キューにも保持し本番UIで使用する。runtime layerは共通のnameを削除し、rect/sprite/sequenceだけを許可する。spriteはassetIdを持つ。textとimageの違いはruntimeへ残さない。

runtime資産はid、width、height、offset、length、sha256を持つ。offsetはblob先頭から、length=`width*height*3`。画素順は左上からrow-major。各画素はRGB565 little endianの2byte、その直後にstraight alpha8の1byte。画像全体の色面とalpha面を分離しない。stride/paddingはない。

資産はIDのASCII昇順に隙間なく連結する。offset=直前offset+length、最終末尾=blobLength。重複/重なり/隙間は禁止。sha256は当該資産のraw bytesを対象にする。合計画素≤800,000。資産を共有参照でき、同じ資産の複数配置で容量を増やさない。

manifestの正規生成は、キーをUnicodeコードポイント昇順に再帰ソート、配列順保持、空白なし、UTF-8文字を直接書込み、制御文字はJSON escape、`/`はescapeしない、整数を10進で出力。受信側は正規化をやり直さず受信バイトを検証・ハッシュする。ビルド日時やランダムIDをmanifestに追加しない。入力に変更がなければ同じLPK bytesを得る。

## 6. 色・合成・一致条件

入力ラスタはsRGB RGBA8 straight alpha。RGBAのRGB値を `(r>>3)<<11 | (g>>2)<<5 | (b>>3)` でRGB565化する。alpha=0のRGBは0に正規化。例: 不透明赤は `00 f8 ff`、不透明緑 `e0 07 ff`、不透明青 `1f 00 ff`、透明黒 `00 00 00`。

描画時の展開は `r8=(r5<<3)|(r5>>2)`、`g8=(g6<<2)|(g6>>4)`、`b8=(b5<<3)|(b5>>2)`。rect.colorも一度565へ量子化して同じ経路を通る。

layer alphaは `a=floor((assetAlpha*opacity+127)/255)`（rectはassetAlpha=255）。チャンネルごとに `out=floor((src*a+dst*(255-a)+127)/255)`。線形光への変換なし、sRGB値空間で整数合成。合成対象はRGB888の黒い64×32バッファ。透明度をキャンバスへ残さない。

最後に `userBrightness` と `brightnessCap` を0–255で比較し、`effective=min(userBrightness,brightnessCap)` をHUB75出力輝度へ設定する。画素値への二重乗算は禁止。ガンマ/CIE補正はv1論理描画では行わず、DMAライブラリの補正も無効化して試験する。実機の色差は別途校正し、Webプレビューを測光再現と呼ばない。

WASM/native/ESP32は同じC++17の `lp-raster-1` を使う。フレーム生成関数はmanifestの検証済みモデル、sceneId、layoutId、sceneTick、viewport、rotationを受け取りRGB888を返す。描画中のファイルI/O、ネットワーク、malloc、壁時計参照は禁止。

## 7. コンパイル処理

1. ZIP/JSON/画像を上限付きで検証。全参照・variant・キュー・時刻を意味検証する。
2. 確定rasterを読む。保存済みtextは再画像化しない。
3. 参照rasterをRGB565A8へ変換。sourceに不要資産があってもruntimeへ含めない。確定PNGはバンドルしたPNG decoderでstraight RGBAへ直接復号する。Canvas経由のpremultiply/unpremultiplyや端末依存の色管理を再度通さない。PNGは保存時にsRGB RGBA8へ正規化し、コンパイル時にICC再変換しない。
4. text/imageをspriteへ変換。座標やトラックは保持。
5. assetsをID順で配置、サイズとSHA-256計算、manifest正規シリアライズ。
6. ヘッダー・blobを連結。最終LPKを独立decoderへ通し、上限とハッシュを再検証。
7. **生成LPKそのもの**をWASMプレビューへ渡す。元Canvas表示だけで配布を承認させない。

Web Worker内で処理し、進捗/キャンセルを提供する。HTTPオリジンでも動くZIP/PNG/GIF/SHA-256のローカル実装をバンドルする。WebCrypto、SharedArrayBuffer、File System Access APIを必須にしない。

## 8. Schema以外の意味検証

全ID一意、layout/slot形状・重複・範囲の検証（08）、layout集合一致、defaultLayoutの所属、assetの存在/role、pathとmime拡張子一致、画像の実寸/ハッシュ、textラスタの幅=boxWidthと高さ=行数*lineHeight、runtime資産境界、キー昇順と値範囲、end≤duration、variant全トラック総数≤512、sequence合計時間/同寸法、キューscene参照、資産画素合計、manifest容量を検査する。

エラーは `{code,path,message}`、pathはJSON Pointer。配布時は最初の1件だけでなく最大100件を返す。途中ファイルはactiveコンテンツにならない。不正なレイヤーだけを飛ばす再生は禁止。
