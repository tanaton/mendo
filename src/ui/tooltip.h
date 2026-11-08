#pragma once
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <windows.h>

// ツールチップの対象。同一判定は zone と key だけで行い、表示文字列の内容には依らない。
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
    // zone 内で対象を識別する値 (項目 index・ボタン種別など)。
    uint64_t key = 0;

    constexpr bool operator==(const TooltipTarget&) const noexcept = default;
    constexpr bool IsEmpty() const noexcept
    {
        return zone == Zone::None;
    }
};

// ポインタの下で求めたツールチップ。text は対象が変わった時だけ作る (ホバー移動のたびに確保・UTF-8 変換しない)。
struct TooltipHover {
    TooltipTarget target;
    std::wstring text;
};

// current (表示中か表示待ちの対象) と同じ対象なら表示文字列の生成を省く。
template <class Fill>
    requires std::invocable<Fill&, std::wstring&>
TooltipHover MakeTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, Fill&& fill)
{
    TooltipHover t{
        { zone, key }
    };
    if (t.target != current) {
        fill(t.text);
    }
    return t;
}

// text が空 (該当ボタンなし) なら空のターゲットを返す。
inline TooltipHover MakeTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    return MakeTooltip(current, zone, key, [text](std::wstring& out) { out = text; });
}

// ホバーしてから遅れて出すツールチップ。待ちには parent の timer_id のタイマーを使う。
class Tooltip {
public:
    Tooltip() = default;
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;

    void Init(HWND parent_hwnd, UINT_PTR timer_id);
    // 出ていれば隠し、text を TOOLTIP_DELAY_MS 後に出す。空なら隠すだけ。
    void Arm(std::wstring_view text);
    // 次に Arm されるまで出さない。
    void Dismiss()
    {
        Arm({});
    }
    // parent が timer_id の WM_TIMER を受けたら呼ぶ。
    void OnTimer();
    void ApplyDarkMode(bool dark);

private:
    enum class Phase : uint8_t {
        Hidden,
        // タイマーを掛けて、出すのを待っている。
        Armed,
        Shown,
    };

    void Show();
    void Hide();
    bool EnsureWindow();

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    UINT_PTR timer_id_ = 0;
    // Armed と Shown の間の文言。
    std::wstring text_;
    Phase phase_ = Phase::Hidden;
    bool dark_ = false;
};
