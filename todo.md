コードベースを精査した上で、詳細な実装計画を立てました。

---

## 実装計画：mendo アーキテクチャ再設計

### 現状の問題点（コード調査結果）

- **`RenderNode`（types.h）** がドメインデータ（text, runs, type）とレイアウトキャッシュ（`IDWriteTextLayout`, `y_position`, `height`, `layout_dirty`）とレンダリングリソース（`diagram_bitmap`）を全て混在させている
- **`LayoutEngine`** が `IDWriteFactory` に直接依存 → COM初期化なしではテスト不可
- **`Renderer::Render()`** が「何を描くか」と「どう描くか」を混在 → 即時モードの `render_target_->DrawXxx()` 呼び出し
- **`MainWindow`** が ~40個のメンバ変数を保持（スクロール、選択、ズーム、ドラッグ、ペイン状態…）
- **`InvalidateRect()`** が少なくとも25箇所に散在

### 推奨する実施順序

リスクと依存関係を考慮し、当初提案から順序を調整します：

| 順序 | ステップ | リスク | 理由 |
|------|----------|--------|------|
| 1 | Node/LayoutCache 分離 | 中〜高 | 全ての基盤。最初にやらないと後工程が全て複雑化する |
| 2 | ViewportManager 導入 | 低 | MainWindow の整理。後続リファクタが楽になる |
| 3 | ITextMeasurer 導入 | 中 | テスタビリティ向上。Step 1が前提 |
| 4 | DrawCommand 中間表現 | 中 | 描画の分離。Step 1, 3が前提 |
| 5 | Win32Shell 薄層化 | 低〜中 | Step 2, 4が前提。段階的に実施可能 |
| 6 | Observable + PaneController | 低 | ボーナス。Step 2, 4の上に構築 |

---

### Step 1: Node/LayoutCache 分離（最大の変更）

**目的**: `ParseMarkdown()` が COM ポインタもレイアウト状態も持たない純粋データを返すようにする

#### 新規ファイル

**`src/node.h`** — 純粋ドメインモデル
```cpp
struct Node {
    NodeType type;
    int heading_level, indent_level, list_number;
    bool task_checked;
    std::wstring text;
    std::vector<TextRun> runs;
    std::wstring anchor_id;
    SyntaxLanguage code_language;
    std::vector<SyntaxToken> syntax_tokens;
    std::vector<TableRow> table_rows;  // TableCell も COM排除版
};
```

**`src/layout_cache.h`** — レイアウト情報を外部管理
```cpp
struct NodeLayoutEntry {
    float y_position = 0.0f;
    float height = 0.0f;
    ComPtr<IDWriteTextLayout> text_layout;
    bool layout_dirty = true;
    bool effects_applied = false;
    std::vector<InlineCodeBg> inline_code_bgs;
    std::vector<ComPtr<IDWriteTextLayout>> cell_layouts;
    std::vector<float> col_widths;
    std::vector<float> row_heights;
};

struct DiagramEntry {
    ComPtr<ID2D1Bitmap> bitmap;
    float width = 0.0f, height = 0.0f;
};

class LayoutCache {
    std::vector<NodeLayoutEntry> entries_;
    std::vector<DiagramEntry> diagrams_;
public:
    void Resize(size_t n);
    NodeLayoutEntry& operator[](size_t i);
    DiagramEntry& GetDiagram(size_t i);
};
```

#### 段階的サブステップ（各ステップでビルド・テスト通過を維持）

| サブステップ | 内容 | 影響ファイル |
|---|---|---|
| 1-i | `node.h`, `layout_cache.h` を追加。既存コード変更なし | 新規のみ |
| 1-ii | `ParseMarkdown()` が `vector<Node>` を返すよう変更。一時的に変換関数 `NodesToRenderNodes()` を用意 | parser.h/cpp, window.cpp |
| 1-iii | `LayoutEngine` を `Node& + LayoutCache&` に変更（**最大の変更**） | layout.h/cpp, test_layout.cpp |
| 1-iv | `Renderer::DrawNode()` を `Node& + NodeLayoutEntry&` に変更 | renderer.h/cpp, renderer_pane.cpp |
| 1-v | `MainWindow` を `vector<Node>` + `LayoutCache` に変更 | window.h/cpp, window_input/scroll/config.cpp |
| 1-vi | `MermaidRenderer` を `DiagramEntry` 経由に変更 | mermaid.h/cpp |
| 1-vii | `TOC`, `DocumentUtils` を `Node` 対応に変更 | toc.h/cpp, document_utils.h/cpp |
| 1-viii | `RenderNode` を `types.h` から削除。変換関数も削除 | types.h |

#### 注意点
- `MermaidRenderer` は現在 `RenderNode*` をコールバックに保持している → ノードインデックス + `LayoutCache*` に変更が必要
- `ApplyNodeEffects()` が `RenderNode` に書き戻す → `NodeLayoutEntry&` に書き戻す形に変更
- `TableCell` から `ComPtr<IDWriteTextLayout>` を除去し、セルレイアウトは `NodeLayoutEntry::cell_layouts` に移動

---

### Step 2: ViewportManager 導入（低リスク）

**目的**: MainWindow から状態を引き剥がし、テスト可能な純粋状態管理クラスを作る

#### 新規ファイル

**`src/viewport_manager.h`**, **`src/viewport_manager.cpp`**

#### 移動する状態

| 現在の所在 (MainWindow) | 移動先 (ViewportManager) |
|---|---|
| `scroll_y_`, `scroll_target_`, `max_scroll_`, `smooth_scrolling_` | スクロール状態 |
| `selection_`, `anchor_node_`, `anchor_pos_`, `is_dragging_` | 選択状態 |
| `zoom_index_` | ズーム状態 |
| `FindFirstVisibleNode()`, `AnchorCompensateScroll()` | ビューポートクエリ |

#### 設計方針
- `ViewportManager` は `needs_repaint_` フラグで再描画要求を集約
- タイマー管理（`SetTimer`/`KillTimer`）は Win32 API なので `MainWindow` に残す
- `ViewportManager` は `mendo_core` に入る（Win32 依存なし） → テスト追加可能

#### 新規テスト: `tests/test_viewport_manager.cpp`
- スクロールの範囲制限、スムーズスクロール、選択の正規化、ズーム段階等を検証

---

### Step 3: ITextMeasurer 導入（中リスク）

**目的**: `LayoutEngine` から DirectWrite 依存を分離し、モックによるテストを可能にする

#### 新規ファイル

**`src/text_measurer.h`** — インターフェース
```cpp
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;
    virtual TextLayoutHandle CreateLayout(
        const std::wstring& text, const std::vector<TextRun>& runs,
        NodeType type, int heading_level,
        float max_width, const Theme& theme) = 0;
    virtual TextMeasurement Measure(TextLayoutHandle handle) = 0;
    virtual void SetMaxWidth(TextLayoutHandle handle, float width) = 0;
    virtual void Release(TextLayoutHandle handle) = 0;
};
```

**`src/dwrite_measurer.h`**, **`src/dwrite_measurer.cpp`** — DirectWrite 実装

#### 変更内容
- `LayoutEngine::Init()` の引数を `IDWriteFactory*` → `ITextMeasurer*` に変更
- `NodeLayoutEntry` に `TextLayoutHandle` フィールド追加
- レンダラは `DWriteTextMeasurer::GetNativeLayout(handle)` で `IDWriteTextLayout*` を取得して描画

#### テスト改善
- `MockTextMeasurer` を作成 → テキスト長から固定高さを返す
- `test_layout.cpp` で COM 初期化不要のテストパスを追加

#### 注意点
- `IDWriteTextLayout` はレンダラ側のヒットテスト（`HitTestPoint`）でも使用 → `DWriteTextMeasurer` からネイティブポインタ取得を許容（漏れのある抽象化だが、レンダリング境界では許容）

---

### Step 4: DrawCommand 中間表現（中リスク）

**目的**: 「何を描画するか」と「どう描画するか」を分離する

#### 新規ファイル

**`src/draw_command.h`** — コマンド型定義
```cpp
using DrawCommand = std::variant<
    ClearCmd, FillRectCmd, FillRoundedRectCmd,
    DrawLineCmd, DrawTextLayoutCmd, DrawBitmapCmd,
    PushClipCmd, PopClipCmd, SetTransformCmd
>;
using DrawCommandList = std::vector<DrawCommand>;
```

**`src/command_generator.h`**, **`src/command_generator.cpp`** — コマンド生成
**`src/command_executor.h`**, **`src/command_executor.cpp`** — D2D実行

#### 段階的移行
1. `DrawCommand` 型を定義（既存コード変更なし）
2. 最も単純なノード（`HorizontalRule`）で並行パスを実装し検証
3. 各ノード種別を順次変換
4. `Render()` 全体を変換
5. `CommandGenerator` と `CommandExecutor` を別ファイルに抽出

#### 注意点
- `ApplyNodeEffects()` は `IDWriteTextLayout` に `SetDrawingEffect` を呼ぶ副作用 → コマンド生成の前段（pre-pass）として残す
- ペインのオフスクリーン描画（`BitmapRenderTarget`）→ ネストされたコマンドリストとしてモデル化
- ビューポートカリングにより1フレームあたり 20〜50 コマンド程度 → パフォーマンス影響は無視できる

---

### Step 5: Win32Shell 薄層化（段階的実施）

**目的**: `WndProc` をイベント変換のみに特化させる

#### 新規ファイル

**`src/app_events.h`** — ドメインイベント型
**`src/app_controller.h`**, **`src/app_controller.cpp`** — アプリケーションロジック

#### 設計
```
WndProc → ConvertWin32Event() → AppEvent
  → AppController::HandleEvent(AppEvent) → ActionResult
    → Shell が副作用実行 (InvalidateRect, SetTimer, SetCursor, ...)
```

#### 段階的移行（1メッセージずつ）
1. `ConvertWin32Event()` 純粋関数を作成
2. `OnKeyDown` を `AppController` に移行（最も単純）
3. `OnMouseWheel` → `OnResize` → `OnPaint` → マウスイベント の順に移行
4. コンテキストメニュー等の Win32 UI は Shell に残す（実用的判断）

---

### Step 6: Observable + PaneController（ボーナス）

- `ViewportManager`/`LayoutCache`/`FileExplorer`/`TOC` が dirty フラグを持つ
- メインループで集約チェック → `InvalidateRect` を1箇所に集約
- ペイン関連の状態（幅、ドラッグ、ホバー、スクロール）を `PaneController` に抽出

---

### 全体スケジュール感

```
Step 1 (Node分離)       ████████████████  ← 最大の作業量、全ファイルに影響
Step 2 (ViewportMgr)    ████              ← 低リスク、即効性あり
Step 3 (ITextMeasurer)  ████████          ← 中程度、テスタビリティ大幅向上
Step 4 (DrawCommand)    ████████████      ← 描画コードの分解
Step 5 (Win32Shell)     ██████            ← 段階的、部分実施可
Step 6 (Observable等)   ████              ← ボーナス
```

どのステップから着手しますか？計画通り Step 1 から始めるのが推奨ですが、Step 2 (ViewportManager) は独立して先に進められるので、並行作業も可能です。

