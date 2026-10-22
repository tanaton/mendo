#pragma once
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

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
        OverlayButton, // key は OverlayButton
        NavButton,
    };

    Zone zone = Zone::None;
    // zone 内で対象を識別する値 (項目 index・ボタン種別など)。表示文字列の内容には依らない。
    uint64_t key = 0;
    // 対象が変わった時だけ設定すればよい。
    std::pmr::wstring text;

    constexpr TooltipTarget() = default;
    TooltipTarget(Zone z, uint64_t k) noexcept : zone(z), key(k)
    {}

    // text は比較しない。ホバー移動のたびに text を作らずに同一判定するため。
    constexpr bool SameTarget(const TooltipTarget& o) const noexcept
    {
        return zone == o.zone && key == o.key;
    }
    constexpr bool IsEmpty() const noexcept
    {
        return zone == Zone::None;
    }
};

// current と同じ対象なら表示文字列の生成 (確保・UTF-8 変換) を省く。
template <class Fill>
    requires std::invocable<Fill&, std::pmr::wstring&>
TooltipTarget MakeTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, Fill&& fill)
{
    TooltipTarget t{ zone, key };
    if (!t.SameTarget(current)) {
        fill(t.text);
    }
    return t;
}

// text が空 (該当ボタンなし) なら空のターゲットを返す。
inline TooltipTarget MakeTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    return MakeTooltip(current, zone, key, [text](std::pmr::wstring& out) { out = text; });
}

// <windows.h> を巻き込まずに HWND を扱うための前方宣言（Windows SDK の
// DECLARE_HANDLE(HWND) と ABI 互換）。
struct HWND__;
using HWND = HWND__*;

// メインウィンドウとツールチップの両方に使う。<dwmapi.h> / <uxtheme.h> をヘッダに漏らさないため実装は tooltip.cpp。
void ApplyDarkModeToWindow(HWND hwnd, bool dark);

class Tooltip {
public:
    Tooltip() = default;
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;

    void Init(HWND parent_hwnd);
    bool Update(const TooltipTarget& target);
    void Show();
    void ApplyDarkMode(bool dark);
    // 隠して対象も忘れる。次のホバーで同じ対象に戻っても表示し直す。
    void Clear();

private:
    void Hide();
    bool EnsureWindow();

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    TooltipTarget current_;
    bool visible_ = false;
    bool dark_ = false;
};
