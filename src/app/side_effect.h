#pragma once
#include "app_constants.h"
#include "tooltip.h"
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
struct ClipboardWrite {
    // text は Document テキスト由来の UTF-8 string。
    // 書き込み側で CF_UNICODETEXT 用に wstring 変換する。
    std::pmr::string text;
};
struct ClipboardWriteHtml {
    std::pmr::string html;
    std::pmr::string plain;
};
struct ShowTooltip {
    TooltipTarget target;
};
struct ClearTooltip {};
struct ShowContextMenu {
    int screen_x;
    int screen_y;
};

struct SearchFocus {
    enum class Mode : uint8_t {
        SelectAll,    // 既存テキスト全選択
        SetCaret,     // caret に位置決め
        SetSelection, // [anchor, caret) を選択
    };
    Mode mode = Mode::SelectAll;
    int anchor = 0; // SetSelection でのみ参照
    int caret = 0;  // SetCaret/SetSelection で参照
};
struct SearchUnfocus {
    bool clear_text = false; // ファイル切替時に true。検索ボックスのテキストを消去する。
};
struct SetWindowPosition {
    int x;
    int y;
    int cx;
    int cy;
};
struct ApplyThemeChange {
    enum class Type : uint8_t {
        Zoom,
        DarkMode
    };
    Type type;
};
// OnResizeEnd が BitmapManage (または遅延レイアウト) と SyncTocActive も出すので、併せて積まない。
struct PerformResizeEnd {};
struct PerformSizingUpdate {};
struct RendererResize {
    UINT width;
    UINT height;
    // ライブリサイズ中。描画面を作り直さずに済むよう大きめのバッファを使い回す。
    bool live = false;
};
struct RendererSetDpi {
    float dpi;
};

struct LoadFile {
    std::pmr::wstring path;
};
struct ReloadFile {};
struct OpenFileDialog {};
// 表示中の文書とは別のファイルのロードを取り消す。同じファイルの再読み込みは止めない。
struct CancelOtherFileLoad {};

struct BitmapManage {};
struct SyncTocActive {
    // false: ハイライト更新のみで目次ペインの自動スクロールを行わない。
    // 目次ペイン操作由来のジャンプでクリック先が動く混乱を防ぐ (issue#259)。
    // ジャンプ由来の判断は reducer 層 (EmitScrollChangedSideEffects) が明示引数で担い、
    // SyncTocActive{} で発火する App 側の再同期 (リサイズ/ロード等) は常に追従で正しい。
    // デフォルトを外すと {} が値初期化で false になり挙動が静かに反転するため維持する。
    bool auto_scroll = true;
};

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
    effect::ClipboardWriteHtml,
    effect::ShowTooltip,
    effect::ClearTooltip,
    effect::ShowContextMenu,
    // Window
    effect::SearchFocus,
    effect::SearchUnfocus,
    effect::SetWindowPosition,
    effect::ApplyThemeChange,
    effect::PerformResizeEnd,
    effect::PerformSizingUpdate,
    effect::RendererResize,
    effect::RendererSetDpi,
    // Navigation
    effect::LoadFile,
    effect::ReloadFile,
    effect::OpenFileDialog,
    effect::CancelOtherFileLoad,
    // Layout
    effect::BitmapManage,
    effect::SyncTocActive,
    // Timer
    effect::SetTimer,
    effect::KillTimer>;

using SideEffectList = std::pmr::vector<SideEffect>;

template <typename T>
void PushEffect(SideEffectList& effects, T&& e)
{
    // スクロール等は 1 回で数件積むので、最初の 1 件で多めに確保して伸長ごとの再確保を避ける。
    if (effects.capacity() == 0) {
        effects.reserve(4);
    }
    effects.emplace_back(std::forward<T>(e));
}
