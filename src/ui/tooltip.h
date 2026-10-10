#pragma once
#include <concepts>
#include <cstdint>
#include <memory>
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
        CopyButton,
        SaveButton,
        DiagramCopyButton,
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

inline TooltipTarget MakeTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, std::wstring_view text)
{
    return MakeTooltip(current, zone, key, [text](std::pmr::wstring& out) { out = text; });
}

// <windows.h> を巻き込まずに HWND を扱うための前方宣言（Windows SDK の
// DECLARE_HANDLE(HWND) と ABI 互換）。
struct HWND__;
using HWND = HWND__*;

class Tooltip {
public:
    Tooltip();
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;
    Tooltip(Tooltip&&) noexcept;
    Tooltip& operator=(Tooltip&&) noexcept;

    void Init(HWND parent_hwnd);
    bool Update(const TooltipTarget& target);
    void Show();
    void Hide();
    void ApplyDarkMode(bool dark);
    void ResetTarget() noexcept;

    const TooltipTarget& GetCurrent() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
