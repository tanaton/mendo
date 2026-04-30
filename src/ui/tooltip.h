#pragma once
#include <cstdint>
#include <memory>
#include <memory_resource>
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
    std::pmr::wstring text;

    constexpr TooltipTarget() = default;
    constexpr TooltipTarget(Zone z, std::wstring_view t) : zone(z), text(t)
    {}

    constexpr bool operator==(const TooltipTarget&) const = default;
    constexpr bool IsEmpty() const noexcept
    {
        return zone == Zone::None;
    }
};

// <windows.h> を巻き込まずに HWND を扱うための前方宣言（Windows SDK の
// DECLARE_HANDLE(HWND) と ABI 互換）。
struct HWND__;
using HWND = HWND__*;

// App 側のホバー検出結果をもとに、ツールチップの表示/非表示を制御する。
class Tooltip {
public:
    Tooltip();
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;
    Tooltip(Tooltip&&) noexcept;
    Tooltip& operator=(Tooltip&&) noexcept;

    void Init(HWND parent_hwnd);
    bool Update(const TooltipTarget& target, int screen_x, int screen_y);
    void Show();
    void Hide();
    void ApplyDarkMode(bool dark);
    void ResetTarget() noexcept;

    const TooltipTarget& GetCurrent() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
