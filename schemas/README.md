# JSON Schema契約

すべてDraft 2020-12、追加プロパティ禁止です。

- `project.schema.json`: ZIP内project.json。
- `runtime.schema.json`: LPK内manifest。
- `state.schema.json`: UDP/APIで扱う完全state。
- `command.schema.json`: POST /api/v2/commandsのbody。
- `rig.schema.json`: 機器台帳・Master選択・layout内slotと実機の割当。秘密情報は含まない。

JSON Schemaは型・上限・構造を検証します。JSON重複キー、ID/パスの完全一致、参照、画像寸法、ハッシュ、時刻関係、uint64値、合計容量、ノード能力、認証、現在状態からの操作可否は本文に従った意味検証が別途必要です。JSON Schemaのdefaultでプロパティを補充する仕組みには依存しません。

`docs/02-data-format.md`、`docs/04-protocol-api.md`、`docs/08-scalability.md` が規範本文です。形式major=2、API=/v2。状態例と操作例は `examples/state.json` / `examples/command.json`。4台の機器割当例は `examples/demo.ledrig.json`。全APIを網羅したOpenAPIではなく、公開APIの全体契約は04/08の表と共通規則を参照してください。
