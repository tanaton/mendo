#pragma once
#include "app_constants.h"
#include <memory_resource>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <windows.h>

// Reducer が返す副作用の型定義。実行は App::Execute。

namespace effect {

struct InvalidateWindow {};
struct SetCapture {};
struct ReleaseCapture {};
// text は Document テキスト由来の UTF-8。書き込み側で CF_UNICODETEXT 用に wstring 変換する。
// html があれば CF_HTML にも載せる。
struct ClipboardWrite {
    std::string text;
    std::string html;
};
// 出ているツールチップを隠し、text を表示待ちにする (遅れて出す)。空なら隠すだけ。
struct SetTooltip {
    std::wstring text;
};
struct ShowContextMenu {
    int screen_x;
    int screen_y;
};
struct Beep {};

// 検索の入力欄にフォーカスを移す。EM_SETSEL の [anchor, caret)。既定値は全選択。
struct SearchFocus {
    int anchor = 0;
    int caret = -1;
};
struct SearchUnfocus {
    bool clear_text = false; // ファイル切替時に true。検索ボックスのテキストを消去する。
};
// 描画の DPI を変え、ウィンドウを WM_DPICHANGED が勧める矩形 (スクリーン座標) へ動かす。
struct ApplyDpi {
    float dpi;
    RECT suggested;
};
struct ApplyZoom {};
struct ToggleDarkMode {};
// リサイズ・スプリッタやスクロールバーのドラッグの終わり・ペインの開閉の後に、表示位置を落ち着かせる。
// App::SettleViewport (RelayoutViewport) が BitmapManage (または遅延レイアウト) も出すので、併せて積まない。
struct SettleViewport {};
struct PerformSizingUpdate {};
struct RendererResize {
    UINT width;
    UINT height;
    // ライブリサイズ中。描画面を作り直さずに済むよう大きめのバッファを使い回す。
    bool live = false;
};

struct LoadFile {
    std::wstring path;
};
struct ReloadFile {};
struct OpenFileDialog {};
// 表示中の文書とは別のファイルのロードを取り消す。同じファイルの再読み込みは止めない。
struct CancelOtherFileLoad {};

struct BitmapManage {};

struct SetTimer {
    app_timer::Id id;
    UINT ms;
};
struct KillTimer {
    app_timer::Id id;
};

} // namespace effect

// 論理グループ (Ui/Window/Navigation/Layout/Timer) は App::Execute の visitor と同じ並び。
using SideEffect = std::variant<
    // Ui
    effect::InvalidateWindow,
    effect::SetCapture,
    effect::ReleaseCapture,
    effect::ClipboardWrite,
    effect::SetTooltip,
    effect::ShowContextMenu,
    effect::Beep,
    // Window
    effect::SearchFocus,
    effect::SearchUnfocus,
    effect::ApplyZoom,
    effect::ToggleDarkMode,
    effect::SettleViewport,
    effect::PerformSizingUpdate,
    effect::RendererResize,
    effect::ApplyDpi,
    // Navigation
    effect::LoadFile,
    effect::ReloadFile,
    effect::OpenFileDialog,
    effect::CancelOtherFileLoad,
    // Layout
    effect::BitmapManage,
    // Timer
    effect::SetTimer,
    effect::KillTimer>;

using SideEffectList = std::pmr::vector<SideEffect>;

template <typename T>
void PushEffect(SideEffectList& effects, T&& e)
{
    // スクロール等は 1 回で数件積むので、最初の 1 件で多めに確保して伸長ごとの再確保を避ける
    // (App::Dispatch の領域は解放しないので、伸長のたびに使い捨てる)。
    if (effects.capacity() == 0) {
        effects.reserve(4);
    }
    effects.emplace_back(std::forward<T>(e));
}
