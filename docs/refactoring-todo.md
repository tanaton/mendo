# リファクタリング残課題

2026-04-15 のアプリ全体リファクタリングで発見されたが、スコープ外としてスキップした課題。

## 1. マウスアクション構造体の未使用 dip_x/dip_y フィールド削除

`app_events.h` の8つのマウスアクション構造体（`LButtonDownAction`, `LButtonUpAction`, `MouseMoveAction`, `MouseHoverAction`, `LButtonDblClkAction`, `RButtonDownAction`, `RButtonUpAction`, `RButtonMoveAction`）に `float dip_x, dip_y` フィールドがあるが、Reducer では `a.px, a.py` しか参照されていない。DIP変換は `App::OnLButtonDown()` 等の内部で `PixelToDip()` により行われるため、アクション構造体側のフィールドは不要。`AppAction` variant のサイズを不必要に肥大化させている。

**対処方針:** 8構造体から `dip_x`/`dip_y` を削除し `px`/`py` のみ残す。`App` 内の `Dispatch()` 呼び出し箇所で引数を修正する。

## 2. ShowToast / CopySelectionToClipboard の Reducer 統一

Reducer 経由と直接呼び出しの2つの経路が存在し、「Reducer が唯一の状態変更点」という設計原則が崩れている。

### ShowToast

- Reducer 経路: `effect::ShowToast` → `SideEffectExecutor` が `state_->interaction.toast.Show()` + `SetTimer` + `InvalidateRect`
- 直接経路: `App::ShowToast()` が同じ処理を直接実行（`app_file.cpp` の `DoLoadMarkdownFile` エラー時等）

### CopySelectionToClipboard

- Reducer 経路: `Ctrl+C` → `CopyClipboardAction` → Reducer が `effect::ClipboardWrite` を生成
- 直接経路: コンテキストメニューの `IDM_COPY` (`app_context_menu.cpp`) → `App::CopySelectionToClipboard()` が直接クリップボード書き込み

**対処方針:** コンテキストメニューの `IDM_COPY` は `Dispatch(CopyClipboardAction{})` に変更。`App::ShowToast` の呼び出し元は SideEffect リスト経由で `effect::ShowToast` を発行するように変更する。

## 3. ThemeConstants の手動同期リスク

`app_navigate.cpp` の `SyncPaneThemeCache()` が `Theme` から `ThemeConstants` へ7フィールドを手動コピーしている。`Theme` に新しいフィールドが追加され Reducer で使われるようになった場合、ここに追加し忘れるとバグになる。

```cpp
// app_navigate.cpp:57-69
void App::SyncPaneThemeCache()
{
    const auto& theme = renderer_.GetTheme();
    state_.window.cached_theme = ThemeConstants{
        .pane_item_height = theme.pane_item_height,
        .pane_header_height = theme.pane_header_height,
        .splitter_width = theme.splitter_width,
        .margin_left = theme.margin_left,
        .margin_right = theme.margin_right,
        .heading_spacing_above = theme.heading_spacing_above,
        .zoom = theme.zoom,
    };
}
```

**対処方針:** `Theme` クラスに `ThemeConstants ToReducerConstants() const` ファクトリメソッドを追加し、変換ロジックを `Theme` 側に集約する。フィールド追加時にコンパイルエラーで漏れを検出できるよう designated initializer を維持する。
