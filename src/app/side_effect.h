#pragma once
#include "app_constants.h"
#include "doc_text.h"
#include "ui_types.h"
#include "pane_layout.h"
#include "tooltip.h"
#include <algorithm>
#include <type_traits>
#include <variant>
#include <string>
#include <memory_resource>
#include <windows.h>

// Reducer が返す副作用の型定義。ドメインごとに variant 化し、SideEffect はそれらを束ねた二段 variant。
// 新機能追加時の Action/Effect/Executor 同期ポイントが該当ドメインに閉じることを狙う。

namespace effect {

struct InvalidateWindow {};
struct InvalidateTitleBar {};
// MD 本文ペインのみ無効化 (詳細は reducer.cpp::EmitScrollChangedSideEffects)。
struct InvalidateMdPane {};
struct SetCapture {};
struct ReleaseCapture {};
enum class CursorType : uint8_t {
    Arrow,
    Hand,
    IBeam,
    SizeWE
};
struct SetCursor {
    CursorType type;
};
struct ClipboardWrite {
    // text は Document テキスト由来の UTF-8 string。
    // executor 側で CF_UNICODETEXT 用に wstring 変換する。
    std::pmr::string text;
};
struct ClipboardWriteHtml {
    std::pmr::string html;
    std::pmr::string plain;
};
struct ShowTooltip {
    TooltipTarget target;
    int px;
    int py;
};
struct ClearTooltip {};
struct ShowToast {
    std::pmr::wstring message;
};
struct ShowContextMenu {
    int screen_x;
    int screen_y;
};

struct ShowWindowCmd {
    int cmd;
};
struct PostWindowMessage {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
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
struct SetWindowTitle {
    std::pmr::wstring title;
};
struct SetWindowPosition {
    int x;
    int y;
    int cx;
    int cy;
};
struct ApplyDarkMode {
    bool dark;
};
struct ApplyThemeChange {
    enum class Type : uint8_t {
        Zoom,
        DarkMode
    };
    Type type;
    uint8_t zoom_index; // Zoom 値は ZOOM_STEPS[zoom_index] で復元する。
};
struct PerformResizeEnd {};
struct PerformSizingUpdate {};
struct RendererResize {
    UINT width;
    UINT height;
};
struct RendererSetDpi {
    float dpi;
};

struct ShellOpen {
    std::pmr::wstring url;
};
struct LoadFile {
    std::pmr::wstring path;
};
struct ReloadFile {};
struct OpenFileDialog {};

struct DeferredLayout {};
struct BitmapManage {};
struct MermaidBatch {};
struct InvalidatePaneCache {
    PaneZone pane;
};
struct RefreshPaneLayout {};
struct SyncTocActive {};
// 可視範囲のレイアウトを即時計測する。md_width/md_height はペインレイアウトのキャッシュ値。
struct ViewportLayout {
    float md_width;
    float md_height;
};
// LayoutService の合計高さを viewport.max_scroll に反映する。
struct SyncMaxScroll {
    float md_pane_height;
};

struct LoadImages {};
struct RequestMermaidRenders {};
struct CancelMermaidBatch {};
struct NotifyImageLoaded {};
struct ClearFileCache {};
struct StartFileWatch {
    std::pmr::wstring path;
};
struct StopFileWatch {};
struct ResumeFileWatch {};
struct CheckFileChanges {};

struct SetTimer {
    app_timer::Id id;
    UINT ms;
};
struct KillTimer {
    app_timer::Id id;
};
struct ProcessDeferredLayout {};
struct TickLoadingAnimation {};
struct ProcessMermaidBatchTimer {};
struct ProcessBitmapManage {};
struct MermaidInitRetry {};

struct Destroy {};
struct HandleParseComplete {};

} // namespace effect

// 全 effect を 1 段 variant に束ねる。論理グループ (Ui/Window/Navigation/Layout/Resource/Timer/Lifecycle)
// は side_effect_executor.cpp の単一 visitor 内のコメント区切りで表現する。
using SideEffect = std::variant<
    // Ui
    effect::InvalidateWindow,
    effect::InvalidateTitleBar,
    effect::InvalidateMdPane,
    effect::SetCapture,
    effect::ReleaseCapture,
    effect::SetCursor,
    effect::ClipboardWrite,
    effect::ClipboardWriteHtml,
    effect::ShowTooltip,
    effect::ClearTooltip,
    effect::ShowToast,
    effect::ShowContextMenu,
    // Window
    effect::ShowWindowCmd,
    effect::PostWindowMessage,
    effect::SearchFocus,
    effect::SearchUnfocus,
    effect::SetWindowTitle,
    effect::SetWindowPosition,
    effect::ApplyDarkMode,
    effect::ApplyThemeChange,
    effect::PerformResizeEnd,
    effect::PerformSizingUpdate,
    effect::RendererResize,
    effect::RendererSetDpi,
    // Navigation
    effect::ShellOpen,
    effect::LoadFile,
    effect::ReloadFile,
    effect::OpenFileDialog,
    // Layout
    effect::DeferredLayout,
    effect::BitmapManage,
    effect::MermaidBatch,
    effect::InvalidatePaneCache,
    effect::RefreshPaneLayout,
    effect::SyncTocActive,
    effect::ViewportLayout,
    effect::SyncMaxScroll,
    // Resource
    effect::LoadImages,
    effect::RequestMermaidRenders,
    effect::CancelMermaidBatch,
    effect::NotifyImageLoaded,
    effect::ClearFileCache,
    effect::StartFileWatch,
    effect::StopFileWatch,
    effect::ResumeFileWatch,
    effect::CheckFileChanges,
    // Timer
    effect::SetTimer,
    effect::KillTimer,
    effect::ProcessDeferredLayout,
    effect::TickLoadingAnimation,
    effect::ProcessMermaidBatchTimer,
    effect::ProcessBitmapManage,
    effect::MermaidInitRetry,
    // Lifecycle
    effect::Destroy,
    effect::HandleParseComplete>;

using SideEffectList = std::pmr::vector<SideEffect>;

// effect::XXX{} を effects に追加する。
template <typename T>
void PushEffect(SideEffectList& effects, T&& e)
{
    effects.emplace_back(std::forward<T>(e));
}

template <typename T>
bool HasEffect(const SideEffectList& effects) noexcept
{
    return std::ranges::any_of(effects, [](const SideEffect& se) {
        return std::holds_alternative<T>(se);
    });
}

// SideEffect から特定の effect 型を取り出す。該当しなければ nullptr。
template <typename T>
const T* GetEffect(const SideEffect& se) noexcept
{
    return std::get_if<T>(&se);
}
