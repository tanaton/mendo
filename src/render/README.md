# render/ — 描画パイプライン

## 責務分担

本ディレクトリは **「本文 (Markdown ノード) の描画」** と **「クローム (UI 付帯要素) の描画」** を別経路で扱う。
新規機能を追加する際は、まずどちらに属するかを判別すること。

### 本文パス：Command 型

- 対象: MD ペインに表示される Markdown コンテンツ（見出し、段落、コードブロック、表、Blockquote、リスト、Mermaid ビットマップ等）
- フロー: `CommandGenerator::GenerateMdPane(...)` が `DrawCommandList` を生成 → `CommandExecutor::Execute(...)` が D2D 呼び出しに変換
- 利点:
  - `DrawCommandList` は構造化データのため、スナップショットテスト / 差分比較が可能
  - `frame_resource_`（pmr monotonic buffer）で毎フレーム確保を O(1) に
  - 生成と実行の分離により、将来的に異なるバックエンド（GPU コマンドバッファ等）への差し替えが容易

| ファイル | 役割 |
|---|---|
| `draw_command.h` | `FillRectCmd` / `DrawLineCmd` / `DrawTextLayoutCmd` 等の variant 型 |
| `command_generator.{h,cpp}` | Markdown ノード → `DrawCommandList`。テーブル／引用群／検索ハイライト等のサブジェネレータも集約 |
| `command_executor.{h,cpp}` | `DrawCommandList` → D2D API 呼び出し |

### クロームパス：Draw\* メソッド群

- 対象: サイドペイン（ファイル/TOC）、タイトルバー、スプリッタ、スクロールバー、検索バー、ジェスチャー軌跡、トースト等
- フロー: `Renderer` のメンバー関数 `DrawSidePanes` / `DrawTitleBar` / `DrawSearchBar` 等が直接 D2D を呼ぶ
- 理由: クロームは入力ヒットテストとの座標一致が優先され、再利用性より即時性が有利。ノード列のような構造を持たないのでコマンド化の旨味が薄い

| ファイル | 役割 |
|---|---|
| `renderer.{h,cpp}` | `Renderer` 本体。初期化 / ブラシ管理 / フォーマット管理 / MD ペイン合成 |
| `renderer_pane.cpp` | ファイルエクスプローラ / TOC / MD スクロールバー / スプリッタ |
| `renderer_titlebar.cpp` | タイトルバーとウィンドウ制御ボタン |
| `renderer_overlay.cpp` | ナビゲーションオーバーレイ / ジェスチャー軌跡 / トースト |
| `renderer_search.cpp` | 検索バー本体（入力欄 / 件数表示 / アイコン） |

### バックエンド分離

| ファイル | 役割 |
|---|---|
| `render_backend.h` | `IRenderBackend` インタフェース（差し替え点） |
| `d2d_render_backend.{h,cpp}` | Direct2D 実装。`ID2D1HwndRenderTarget` / `ID2D1Factory` / `IDWriteFactory` / `IWICImagingFactory` をラップしデバイスロストを検出 |
| `render_params.h` | `RenderParams` / `PaneRect` / `SidePaneState` / `TitleBarRenderState` 等の描画入力 POD |

## 追加時の判断フロー

1. 新しく描く対象は **Markdown 本文ノードに付随する装飾** か？
   - Yes → `command_generator.cpp` にサブジェネレータを追加し、`DrawCommandList` へ emplace
   - No → クロームパスへ
2. クロームの場合：
   - 既存 Draw\* の責務に収まるなら同一ファイルへ追加
   - 収まらないなら新規 `renderer_<name>.cpp` を作り `renderer.h` に `Draw<Name>` を宣言

## 注意

- 本文パスとクロームパスは同一ウィンドウに描画されるが、`Renderer::Render` は本文パス（`CommandExecutor::Execute`）の前後でクローム用の `Draw*` を呼び分けており、クリップ矩形で分離されている。どちらかに紛れ込ませると描画順序バグの温床になる
- 本文パスの pmr リソース (`frame_resource_`) はフレーム開始時にリセットされる前提。`CommandGenerator::GenerateMdPane` 呼び出し外で `DrawCommandList` を保持しない
