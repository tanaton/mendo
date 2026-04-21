# リファクタリング計画（持ち越し）

本書は 2026-04-20 のリファクタリングセッションで積み残した作業項目をまとめたもの。
再着手時はこのファイルを起点に進行できる。

## 1. 完了済み（参考）

| 項目 | 内容 |
|---|---|
| C-0 | `SaveAnchorFromState` を `src/app/reducer.cpp` から `src/app/app_state.cpp`（新規）へ分離 |
| E.1 / E.2 | `tests/test_hit_test_service.cpp` を新規追加（20 テスト）。`NavButtonHitTest` / `HitTest`（DirectWrite 非依存経路）/ `HitTestTable`（線形フォールバック・行外）/ `SaveButtonHitTest` の4経路を網羅。`CopyButtonHitTest` は `tests/test_copy_button.cpp` で既に網羅済み |
| D-0 | Dead mouse action code 削除：`LButtonDownAction` 等 9 アクション struct、`reducer.cpp` の 9 分岐、`effect::HandleMouseEvent` / `HandleContextMenu` とそれぞれの `MouseEventType` enum、`Callbacks::handle_mouse_event` / `handle_context_menu`、`app_init.cpp` のコールバック登録、`test_side_effect_executor.cpp` の対応テスト |
| D-1 | `MdPaneNavHoverAction` を導入し、`state_.interaction.nav_hover` 直変更（`app_mouse_md_pane.cpp:207-233`）を `Dispatch` 経由に変更。Reducer 側で `nav_hover` 更新・`hovered_copy_node/save_node` リセット・`InvalidateWindow` 発行を集約。`test_reducer.cpp` に 3 テスト追加 |
| D-2 | `MdPaneButtonHoverChangedAction` を追加し、`app_mouse_md_pane.cpp:243-277` の `hovered_copy_node` / `hovered_save_node` 直変更を Dispatch 経由に変更。ハンドラは距離スロットリングと `last_*_hit_pos` 更新のみに限定。`test_reducer.cpp` に 3 テスト追加 |
| D-3 | `SplitterDrag{Started,Moved,Ended}Action` を追加し、`app_mouse.cpp` の Down / Move / Up 3 経路のスプリッタ状態直変更を Dispatch 経由に変更。`SplitterDragEndedAction` は `effect::ReleaseCapture{}` + `effect::PerformResizeEnd{}` を発行（Win32 `OnResize` 往復を回避）。`test_reducer.cpp` に 7 テスト追加 |
| D-5 | `SearchInputDrag{Started,Moved,Ended}Action` を追加。Down（`app_mouse_md_pane.cpp`）/ Move・Up（`app_mouse.cpp`）の `state_.search.search_bar_ctrl.StartDrag/EndDrag` 直呼びと直接 `PostMessage` を Dispatch に置換。`effect::PostMessage` で Win32 メッセージ発行を副作用化。`app_constants.h` から `app_msg` / `app_param` を `app_messages.h` に切り出し（Reducer が WebView2 依存ヘッダを巻き込まないように）。`test_reducer.cpp` に 4 テスト追加 |
| D-6 | `FilePane{Directory,File}ClickedAction` を追加し、`app_mouse_side_pane.cpp::HandleFilePaneClick` のディレクトリ／ファイルクリック処理を Dispatch 化。`GetFileAttributesW` の I/O 検証はハンドラに残し、成功時のみ Dispatch。`test_reducer.cpp` に 2 テスト追加 |
| D-7 | `TocItemClickedAction { anchor_id }` を追加し、`app_mouse_side_pane.cpp::HandleTocPaneClick` の `PushNavHistory` + `NavigateToAnchor` 直呼びを Dispatch 化。Reducer は `FindAnchorIndex` → `layout_cache` 参照で target_y を算出して `viewport.ScrollTo` + `InvalidateWindow` / `BitmapManage` を発行。`test_reducer.cpp` に 2 テスト追加 |
| D-4 | `TextSelection{Started,Moved,Ended}Action` を追加。MD ペイン Down（`app_mouse_md_pane.cpp`）/ Move・Up（`app_mouse.cpp`）の `viewport.SetClickStart/SetAnchor/SetDragging/SetSelection` 直変更を Dispatch 化。`SetCapture` は `effect::SetCapture` に置換。小クリック時のリンクハンドリングはハンドラ側に残置（`HandleLinkClick` が Win32 依存のため）。`test_reducer.cpp` に 8 テスト追加 |
| D-8 | `RightClickGesture{Started,Moved,Completed}Action` を追加。`app_mouse_gesture.cpp` の `state_.interaction.gesture.On*` 直呼び・`SetCapture/ReleaseCapture` 直呼び・`OnContextMenu` 直呼びを Dispatch 化。Reducer がステートマシン（`OnRButtonDown/OnMouseMove/OnRButtonUp`）を内部呼び出しし、`GestureResult` に応じて `effect::ReleaseCapture` + `effect::ShowContextMenu` / `ReduceNavigateBack`/`Forward` + `InvalidateWindow` を発行。`effect::ShowContextMenu { screen_x, screen_y }` を新設し、executor 経由で `OnContextMenu` を呼ぶ。`test_reducer.cpp` に 7 テスト追加、`test_side_effect_executor.cpp` に 1 テスト追加 |
| P1-a | `core/document_utils.cpp` から `<windows.h>` を除去。`IsCharAlphaNumericW` を ASCII 英数字+`_` 判定へ置換し、既存テスト（CJK は単語境界に含めない）の意味論を維持 |
| P1-b | `src/core/types.h` を `text_types.h`（`TextRun` / `TextSelection`）と `document_types.h`（`Node` とその支援型）に分割。17 ファイルのインクルード元を `document_types.h` に置換し、`types.h` を削除 |
| P1-c | `app_events.h` から `<windows.h>` を除去。`UINT` / `UINT_PTR` / `RECT` を `uint32_t` / `uintptr_t` / 新設 `PixelRect` に置換。`App::OnDpiChanged` 境界で `RECT → PixelRect` 変換。直接依存は解消（`hit_test_service.h` 経由の `<dwrite.h>` 依存は残存） |
| P1-d | `app_file.cpp` の reload diff 分岐を `AnalyzeReloadDiff(old_utf8, new_utf8)` 純粋関数に集約。`ReloadOp { NoChange, DeferPrefixShrink, PrefixGrowth, FullReload }` 4 分岐で `OnParseComplete` / `DoReloadCurrentFile` を統一。`App::ShouldDeferForTruncateRewrite` を削除。`test_document_utils.cpp` に `AnalyzeReloadDiff` テスト 10 件追加 |
| P2-a | `src/render/command_gen_table.cpp` を `command_generator.cpp` 末尾に統合（3 メソッド + `<ranges>` インクルード）。`CMakeLists.txt` から削除。render/ の物理分離を 1 ファイル解消 |
| P2-b | `src/render/README.md` を新規追加。「本文=Command / クローム=Draw\*」の分担と追加時の判断フローを記述 |
| P2-c | `renderer.cpp`（676 行）から D2D リソース生成（`RecreateBrushes` / `InvalidateBrushes` / `CreatePaneFormat` / `RecreatePaneFormats` / `LoadAppIconBitmap`）を `renderer_resources.cpp`（225 行）へ分離。renderer.cpp は 457 行に縮小。`Init` / `RecreateRenderTarget` / 描画本体は renderer.cpp に残置 |
| C-1 | `src/app/app_mouse*.cpp` を 5 ファイル → 3 ファイルに責務別再編。`app_mouse.cpp`（321 行・Win32 エントリ + TitleBar クリック + gesture/Xボタン + 共有ヘルパー `HitTest`/`GetLinkAtHit`/`UpdateTooltip`/`ClearTooltip`）、`app_mouse_click.cpp`（224 行・MD/File/TOC の各クリック→Dispatch と `TryHandlePaneScrollbarClick`/`RefreshFilePane`）、`app_mouse_hover.cpp`（345 行・`OnMouseHover`/`HandleMdPaneHover`/`IsOverMdScrollbar` の Win32 カーソル/ツールチップ更新）に整理。削除：`app_mouse_gesture.cpp` / `app_mouse_md_pane.cpp` / `app_mouse_side_pane.cpp` |
| P1-e | `NavButtonHover` enum を `src/input/nav_button.h` に切り出し、`app_events.h` の `#include "hit_test_service.h"` を `#include "nav_button.h"` に置換。`HitTestService` 内のクラス enum を削除し、全箇所 `HitTestService::NavButtonHover` → `NavButtonHover` に一括置換（`reducer.cpp` / `app_state.h` / `app.h` のエイリアス / テスト 2 ファイル）。これで `app_events.h` 経由での `<dwrite.h>` / `<d2d1.h>` 引き込みが解消され、AppAction 定義ヘッダがプラットフォーム非依存化。`app_state.h` は `LayoutCache` / `HitTestService` を直接メンバとして持つため依然として `<dwrite.h>` を含むが、これは別タスク |
| D-9a | `MdScrollbarDrag{Started,Moved,Ended}Action` を追加し、`app_mouse_click.cpp:85` の `SetCapture` 直呼び・`app_mouse.cpp:245-255` の Move 時計算・`app_mouse.cpp:167-182` の `ReleaseCapture`+`OnResize`+`ScheduleBitmapManage` を Dispatch 化。Reducer 側で `ComputeScrollInfo`/`ComputeThumbY`/`ScrollFromThumbY` を使って thumb 計算・`ScrollTo` を行い、`effect::SetCapture`/`ReleaseCapture`/`PerformResizeEnd`/`BitmapManage`/`InvalidateWindow` を発行。`test_reducer.cpp` に 6 テスト追加 |
| D-9b | `PaneScrollbarDrag{Started,Moved,Ended}Action`（`PaneTarget` フィールド付き）を追加し、File/Toc スクロールバードラッグを Dispatch 化。`HandleScrollbarClick`/`HandleScrollbarDrag`/`HandleSidePaneScrollDrag`/`TryHandlePaneScrollbarClick` を削除し、`IsOverPaneScrollbar` 静的当たり判定のみ残置。`app_mouse.cpp:OnLButtonUp` の `ReleaseCapture()` 直呼び・`OnResize` 往復を完全排除。合わせて `ReduceTextSelectionStarted` を `node_index<0` で `SetCapture` を発行しないように挙動変更（対応 `ReleaseCapture` 漏れを解消）。`effect::InvalidatePaneCache{ PaneZone }` で pane キャッシュ無効化を副作用化。`test_reducer.cpp` に 7 テスト追加＋既存テスト 1 件を挙動変更に合わせて更新 |
| 4.2 | ツールチップ更新を Dispatch 化。`UpdateTooltipAction { TooltipTarget, px, py }` / `ClearTooltipAction` を新設し、`app_mouse_hover.cpp`（10 箇所）と `app_scroll.cpp::InvalidateHitPositions` の `UpdateTooltip` / `ClearTooltip` 直呼びを Dispatch に置換。`App::UpdateTooltip` / `App::ClearTooltip` メソッドと `app.h` 宣言を削除。Reducer は `effect::ShowTooltip` / `effect::ClearTooltip` を発行（既存 executor がタイマー＋`tooltip.Update`/`Hide` を実行）。`TooltipTarget` を `tooltip.h` から新ヘッダ `src/ui/tooltip_target.h` に切り出し、`app_events.h` が Win32 を引き込まずに参照できるようにした。`test_reducer.cpp` に 3 テスト追加 |
| P1-f | `src/input/hit_test_service.h` から `<dwrite.h>` 直 include と `#include "layout_cache.h"` を除去し、`LayoutCache` / `NodeLayoutEntry` を前方宣言化。`MdPaneHitContext::cache` および `HitTestTable(..., const NodeLayoutEntry&, ...)` は参照型のため前方宣言で十分。実体は `hit_test_service.cpp` が `layout.h` 経由で解決。あわせて未使用の `<optional>` / `<string>` を削除。これで `hit_test_service.h` を include するだけでは `<dwrite.h>` が波及しなくなり、Redux 層ヘッダから dwrite 参照を 1 経路閉塞。`app_state.h` は依然 `layout_cache.h`（`<dwrite.h>`）と `context_menu.h`（`<windows.h>`/`<dwrite.h>`/`<d2d1.h>`）を直接 include しており、完全除去には PIMPL 化が必要（保留）。フルテスト 1832 件回帰なし |

---

## 2. D 完了後のクリーンアップ候補

2026-04-21 時点で候補は実質完了。新規の機械的クリーンアップは見送り。

- ~~`src/app/app_mouse*.cpp` を責務別に 3 ファイルへ統合~~（C-1 として完了。上表参照）
- ~~`reducer.cpp` の行数~~（D-9 完了後は約 900 行。論理分割済みのため機械分割の便益は薄く、見送り確定。ここから更に肥大するなら再判断）
- ハンドラ側に残る `state_` 直変更は `hover_throttle.last_*` のスロットリング用キャッシュのみで、D-2 で意図的に残置した箇所。Dispatch 経由にするとホットパスのオーバーヘッドになるため現状維持

---

## 3. 前回構造レビューからの残項目

2026-04-20 初回セッションで提示した優先度表のうち、D 以外で未着手のもの。

### 高優先度（P1）

※ 2026-04-20 の次セッションで P1-a〜P1-d を完了（上表参照）。残項目：

| 項目 | 対象 | 狙い | リスク | 作業量 | 状況 |
|---|---|---|---|---|---|
| `app.h` の public/internal 分離 | `src/app/app.h`（264 行） | public facade と internal handler を別ヘッダに分ける | 低 | 1 時間 | 評価済み。concrete member 構成のため PIMPL 化しないと真の分離が困難で見送り。必要になれば再検討 |
| Redux 層の Win32 完全除去 | `app_state.h` → `layout_cache.h` / `context_menu.h` 経由の Win32 グラフィクス依存が残存 | Redux 層を完全 platform-agnostic に | 中〜高（PIMPL 化による波及あり） | 数時間〜 | P1-e・P1-f により `app_events.h` と `hit_test_service.h` 経路は dwrite フリーに。残るのは `app_state.h` 自体が値メンバとして抱える `LayoutCache` / `ContextMenu` / `FileExplorer` などの Win32 依存コンポーネント。PIMPL（`unique_ptr` 化）もしくは分割ヘッダ導入が必要で、`app.h` の public/internal 分離とも絡むため保留 |

### 中優先度（P2）

※ 2026-04-20 の再着手セッションで P2-a〜P2-c を完了（上表参照）。残項目：

| 項目 | 対象 | 狙い | リスク | 作業量 |
|---|---|---|---|---|
| `mermaid.cpp` テスト追加 | `src/mermaid/mermaid.cpp`（787 行） | WebView2 連携のサイレント失敗を検出する | 高（モック必要） | 1日以上 |

### 検討中（判断が割れた項目）

| 項目 | 備考 |
|---|---|
| Reducer をアクション族で分割 | D-9 完了後は約 920 行。論理分割済みなので機械分割の便益は薄いが、ここから更に肥大するなら再判断 |
| `renderer_*.cpp` を `src/render/chrome/` へ移動 | 二重化は誤認と確認済み。物理的な再配置は優先度低 |

---

## 4. 共通のオープン質問・確認事項

今後 P1/P2 / クリーンアップに着手する際の検討ポイント：

1. ~~**`SetCapture` / `ReleaseCapture`** のうち Reducer に寄せきれていない呼び出し~~（D-9a/D-9b で完了。`app_mouse.cpp`/`app_mouse_click.cpp` の直呼びは全て effect 経由に置換済み）
2. ~~**ツールチップ** の更新（`UpdateTooltip` / `ClearTooltip`）はハンドラに残しているが、`effect::ShowTooltip` / `ClearTooltip` が既にあるため段階的に寄せられる~~（4.2 として完了。上表参照）
3. **`InvalidateMdPane(md_rect)` vs `InvalidateWindow{}`**：前者は特定エリアだけ無効化、後者は全画面。機能的には同等、パフォーマンス差は軽微。D-1 以降は `InvalidateWindow{}` に統一している。区別のためには `effect::InvalidateRect{rect}` を追加する選択肢あり

---

## 5. 再着手時の進め方

推奨順（2026-04-21 更新）：

1. **P1 残項目**: Redux 層の `<dwrite.h>` 完全除去（`app_state.h` の PIMPL 化）
2. **P2 残項目**: `mermaid.cpp` テスト追加（WebView2 モック設計）

各ステップで：
- 変更後にフルテスト（`build/tests/Release/mendo_tests.exe --gtest_brief=1`）で回帰確認
- `test_reducer.cpp` に新アクションのテスト 2〜3 件追加
- `app_mouse_*.cpp` から直変更が 1 箇所ずつ消えるのを確認

---

## 6. P1/P2 実施計画（2026-04-21 立案）

### 6.1 P1: Redux 層の `<dwrite.h>` 完全除去

#### 現状調査結果
`src/app/app_state.h` が値メンバとして抱える 5 コンポーネントが Win32 依存を波及。影響範囲は 7 ファイル（reducer.h/cpp、app.h、side_effect_executor.h/cpp、render_composer.h、テスト 2 件）。

| コンポーネント | Win32 依存の形 | PIMPL 化難度 |
|---|---|---|
| `FileExplorer` | `<windows.h>` include のみ。型は Win32 非依存 | 低 |
| `Tooltip` | `HWND` ポインタメンバのみ | 低 |
| `TitleBar` | `D2D1_RECT_F` を値で複数保持 | 中 |
| `ContextMenu` | D2D/DWrite factory 参照・ComPtr 値保持 | 中 |
| `LayoutCache` | `std::pmr::vector<NodeLayoutEntry>` が ComPtr を値保持。NodeLayoutEntry 再設計必須 | 高 |

Reducer と各コンポーネントの密結合は低い（`layout_cache` 3 箇所 / `file_explorer` 3 箇所 / `context_menu`・`hit_test` は reducer から未使用）。

#### 実施ステップ（難易度昇順）

| # | ステップ | 手法 | 工数 | リスク |
|---|---|---|---|---|
| P1-g | `FileExplorer` から Win32 依存を cpp 側へ | 実装を `.cpp` に移し、ヘッダから `<windows.h>` を除去 | 小 | 低 |
| P1-h | `Tooltip` の PIMPL 化 | `HWND` を `std::unique_ptr<Impl>` で隠蔽 | 小 | 低 |
| P1-i | `TitleBar` の PIMPL 化 | `D2D1_RECT_F` 値メンバを Impl に隠蔽 | 中 | 低 |
| P1-j | `ContextMenu` の PIMPL 化 | factory 参照・ComPtr を Impl 側へ。`app.h` public/internal 分離と絡む場合あり | 中 | 中 |
| P1-k | `LayoutCache` の PIMPL 化 | `NodeLayoutEntry` の再設計が必須。`render/` 配下への影響範囲大 | 大 | 中〜高 |

#### 検証
- 各ステップ後にフルテスト PASS（現状 1832 件）
- `grep -l "dwrite" src/app/*.h` が 0 に近づくことを確認
- `app_state.h` を include する側のコンパイル時間短縮を期待値として確認

#### 判断ポイント
- P1-g / P1-h / P1-i は独立・低リスク。**先行実施推奨**
- P1-j は `app.h` public/internal 分離と絡む可能性あり。実装時に再評価
- P1-k は `render/` への影響範囲が広いため**別セッションで検討**

---

### 6.2 P2: `mermaid.cpp` テスト追加

#### 現状調査結果
- `MermaidRenderer`（`src/mermaid/mermaid.cpp` 787 行）が `ICoreWebView2Environment` / `Controller` / `ICoreWebView2` を ComPtr で直接保有（95-96 行）
- `CreateCoreWebView2EnvironmentWithOptions()`（192 行）を直接呼ぶ（DI なし）
- 非同期コールバック経路: JS Promise → postMessage → WebMessage リッスン（253-336 行）→ `request_id` 照合
- 既存テスト: `test_mermaid_renderer.cpp`（159 行）が Init 状態遷移・キャッシュミス・冪等性のみ網羅。`test_mermaid_util.cpp` / `test_mermaid_file_cache.cpp` も存在

#### 実施ステップ（着手しやすさ順）

| # | ステップ | 手法 | 工数 | リスク |
|---|---|---|---|---|
| P2-d | JSON パーサ抽出 | `OnRenderResult()` の `find_num()`（622-640 行）を `mermaid_util` に抽出し単体テスト | 小 | 低 |
| P2-e | `RequestTracker` クラス抽出 | `request_counter_` / キュー管理 / キャンセル処理を純粋クラスに分離。モックなしで単体テスト可能 | 中 | 低 |
| P2-f | `ICoreWebView2Wrapper` インターフェース導入 | `ExecuteScript()` / `CapturePreview()` / WebMessage リッスンをラップ。gmock で代替実装を用意 | 大（1 日以上） | 中 |
| P2-g | エラー経路テスト追加 | `render-error` ハンドラ（294-300 行）・JSON `ok: false`（648-654 行）の失敗検出を P2-f のモックで再現 | 中 | 中 |

#### 判断ポイント
- P2-d / P2-e は WebView2 モック不要で先行実施可能。**コスト対効果が高い**
- P2-f は 1 日以上の工数でユーザー判断が必要（サイレント失敗検出の価値 vs 工数）
- P2-g は P2-f 完了後に初めて意味を持つ

---

### 6.3 推奨セッション分割

| セッション | 内容 | 特徴 |
|---|---|---|
| 次回 | P1-g / P1-h / P1-i / P2-d / P2-e | 小粒・モック不要・並行実施可能 |
| その次 | P1-j → P1-k | PIMPL の段階的適用 |
| 別途判断 | P2-f / P2-g | WebView2 モック導入工数の見合いで着手是非を決定 |
