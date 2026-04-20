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

---

## 2. D 完了後のクリーンアップ候補

- ~~`src/app/app_mouse*.cpp` を責務別に 3 ファイルへ統合~~（C-1 として完了。上表参照）
- `reducer.cpp` の行数（D-9 完了後は約 920 行）。アクション族別分割（C-2 相当）は論理分割済みのため機械分割の便益は薄い。ここから更に肥大するなら再判断

---

## 3. 前回構造レビューからの残項目

2026-04-20 初回セッションで提示した優先度表のうち、D 以外で未着手のもの。

### 高優先度（P1）

※ 2026-04-20 の次セッションで P1-a〜P1-d を完了（上表参照）。残項目：

| 項目 | 対象 | 狙い | リスク | 作業量 | 状況 |
|---|---|---|---|---|---|
| `app.h` の public/internal 分離 | `src/app/app.h`（264 行） | public facade と internal handler を別ヘッダに分ける | 低 | 1 時間 | 評価済み。concrete member 構成のため PIMPL 化しないと真の分離が困難で見送り。必要になれば再検討 |
| Redux 層の Win32 完全除去 | `app_events.h` → `hit_test_service.h` 経由の `<dwrite.h>` 依存が残存 | Redux 層を完全 platform-agnostic に | 中（`NavButtonHover` enum を共有ヘッダへ移す等の派生整理が必要） | 1〜2 時間 | P1-e として `app_events.h` 経路は完了（上表参照）。`app_state.h` が `LayoutCache` / `HitTestService` を値メンバとして持つための `<dwrite.h>` 依存は未解消。PIMPL もしくは前方宣言 + unique_ptr 化が必要で、`app.h` の public/internal 分離とも絡むため保留 |

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

推奨順：

1. P2 残項目（`mermaid.cpp` テスト追加）に着手するか、D 完了後のクリーンアップ（マウスファイル統合 / reducer 分割）へ進む
2. または P1 の残項目（Redux 層の `<dwrite.h>` 依存除去）に着手

各ステップで：
- 変更後にフルテスト（`build/tests/Release/mendo_tests.exe --gtest_brief=1`）で回帰確認
- `test_reducer.cpp` に新アクションのテスト 2〜3 件追加
- `app_mouse_*.cpp` から直変更が 1 箇所ずつ消えるのを確認
