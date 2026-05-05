#pragma once
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
    // text は Document テキストから抽出されるので doc_string。
    // executor (CF_UNICODETEXT 書込) 側で UTF-16 変換が必要 (UTF-8 ビルド時のみ)。
    mendo::doc_string text;
};
struct ClipboardWriteHtml {
    mendo::doc_string html;
    mendo::doc_string plain;
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
    UINT_PTR id;
    UINT ms;
};
struct KillTimer {
    UINT_PTR id;
};
struct ProcessDeferredLayout {};
struct TickLoadingAnimation {};
struct ProcessMermaidBatchTimer {};
struct ProcessBitmapManage {};
struct MermaidInitRetry {};

struct Destroy {};
struct HandleParseComplete {};

} // namespace effect

using UiEffect = std::variant<
    effect::InvalidateWindow,
    effect::InvalidateTitleBar,
    effect::SetCapture,
    effect::ReleaseCapture,
    effect::SetCursor,
    effect::ClipboardWrite,
    effect::ClipboardWriteHtml,
    effect::ShowTooltip,
    effect::ClearTooltip,
    effect::ShowToast,
    effect::ShowContextMenu>;

using WindowEffect = std::variant<
    effect::ShowWindowCmd,
    effect::PostWindowMessage,
    effect::SetWindowTitle,
    effect::SetWindowPosition,
    effect::ApplyDarkMode,
    effect::ApplyThemeChange,
    effect::PerformResizeEnd,
    effect::PerformSizingUpdate,
    effect::RendererResize,
    effect::RendererSetDpi>;

using NavigationEffect = std::variant<
    effect::ShellOpen,
    effect::LoadFile,
    effect::ReloadFile,
    effect::OpenFileDialog>;

using LayoutEffect = std::variant<
    effect::DeferredLayout,
    effect::BitmapManage,
    effect::MermaidBatch,
    effect::InvalidatePaneCache,
    effect::RefreshPaneLayout,
    effect::SyncTocActive,
    effect::ViewportLayout,
    effect::SyncMaxScroll>;

using ResourceEffect = std::variant<
    effect::LoadImages,
    effect::RequestMermaidRenders,
    effect::CancelMermaidBatch,
    effect::NotifyImageLoaded,
    effect::ClearFileCache,
    effect::StartFileWatch,
    effect::StopFileWatch,
    effect::ResumeFileWatch,
    effect::CheckFileChanges>;

using TimerEffect = std::variant<
    effect::SetTimer,
    effect::KillTimer,
    effect::ProcessDeferredLayout,
    effect::TickLoadingAnimation,
    effect::ProcessMermaidBatchTimer,
    effect::ProcessBitmapManage,
    effect::MermaidInitRetry>;

using LifecycleEffect = std::variant<
    effect::Destroy,
    effect::HandleParseComplete>;

// ドメイン variant を束ねる二段 variant。
using SideEffect = std::variant<
    UiEffect,
    WindowEffect,
    NavigationEffect,
    LayoutEffect,
    ResourceEffect,
    TimerEffect,
    LifecycleEffect>;

using SideEffectList = std::pmr::vector<SideEffect>;

namespace side_effect_detail {

template <typename T, typename Variant>
struct variant_contains;

template <typename T, typename... Us>
struct variant_contains<T, std::variant<Us...>>
    : std::disjunction<std::is_same<T, Us>...> {};

template <typename T, typename Variant>
inline constexpr bool variant_contains_v = variant_contains<T, Variant>::value;

template <typename T>
constexpr SideEffect WrapIntoDomain(T&& e)
{
    using D = std::remove_cvref_t<T>;
    if constexpr (variant_contains_v<D, UiEffect>) {
        return UiEffect{ std::forward<T>(e) };
    }
    else if constexpr (variant_contains_v<D, WindowEffect>) {
        return WindowEffect{ std::forward<T>(e) };
    }
    else if constexpr (variant_contains_v<D, NavigationEffect>) {
        return NavigationEffect{ std::forward<T>(e) };
    }
    else if constexpr (variant_contains_v<D, LayoutEffect>) {
        return LayoutEffect{ std::forward<T>(e) };
    }
    else if constexpr (variant_contains_v<D, ResourceEffect>) {
        return ResourceEffect{ std::forward<T>(e) };
    }
    else if constexpr (variant_contains_v<D, TimerEffect>) {
        return TimerEffect{ std::forward<T>(e) };
    }
    else if constexpr (variant_contains_v<D, LifecycleEffect>) {
        return LifecycleEffect{ std::forward<T>(e) };
    }
    else {
        static_assert(!std::is_same_v<D, D>, "Effect type is not registered in any domain variant");
    }
}

} // namespace side_effect_detail

// effect::XXX{} を適切なドメイン variant に自動 wrap して effects に追加する。
template <typename T>
void PushEffect(SideEffectList& effects, T&& e)
{
    effects.emplace_back(side_effect_detail::WrapIntoDomain(std::forward<T>(e)));
}

template <typename T>
bool HasEffect(const SideEffectList& effects) noexcept
{
    return std::ranges::any_of(effects, [](const SideEffect& se) {
        return std::visit([](const auto& domain_effect) -> bool {
            using D = std::remove_cvref_t<decltype(domain_effect)>;
            if constexpr (side_effect_detail::variant_contains_v<T, D>) {
                return std::holds_alternative<T>(domain_effect);
            }
            return false;
        }, se);
    });
}

// 二段 variant の中から特定の effect 型を取り出す。該当しなければ nullptr。
template <typename T>
const T* GetEffect(const SideEffect& se) noexcept
{
    return std::visit([](const auto& domain_effect) -> const T* {
        using D = std::remove_cvref_t<decltype(domain_effect)>;
        if constexpr (side_effect_detail::variant_contains_v<T, D>) {
            return std::get_if<T>(&domain_effect);
        }
        return nullptr;
    }, se);
}
