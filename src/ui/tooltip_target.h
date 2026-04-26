#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// ツールチップの表示対象を識別する値型。
// zone + text の組み合わせでホバー対象の変化を検出する。
// プラットフォーム非依存（Reducer / AppAction 経由で使うため Win32 ヘッダを引き込まない）。
struct TooltipTarget {
    enum class Zone : uint8_t {
        None,
        TitleBarButton,
        SearchBarButton,
        FilePaneItem,
        FilePaneButton,
        TocPaneItem,
        TocPaneButton,
        MdLink,
        MdImage,
        CopyButton,
        SaveButton,
        SvgCopyButton,
        NavButton,
    };

    Zone zone = Zone::None;
    std::wstring text;

    TooltipTarget() = default;
    TooltipTarget(Zone z, std::wstring_view t) : zone(z), text(t) {}

    bool operator==(const TooltipTarget&) const = default;
    bool IsEmpty() const noexcept { return zone == Zone::None; }
};
