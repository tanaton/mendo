#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>

// ApplyDarkModeToWindow は app.h で宣言済み（循環回避のため前方宣言）
void ApplyDarkModeToWindow(HWND hwnd, bool dark);

// ツールチップの表示対象を識別する構造体。
// zone + text の組み合わせでホバー対象の変化を検出する。
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
        NavButton,
    };

    Zone zone = Zone::None;
    std::wstring text;

    bool operator==(const TooltipTarget&) const = default;
    bool IsEmpty() const noexcept { return zone == Zone::None; }
};

// Win32 TOOLTIPS_CLASS を TTF_TRACK モードで管理するラッパー。
// App 側のホバー検出結果をもとに、ツールチップの表示/非表示を制御する。
class Tooltip {
public:
    Tooltip() = default;
    ~Tooltip() { Destroy(); }
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;

    // 親ウィンドウに紐づくツールチップを作成する。
    void Init(HWND parent)
    {
        parent_ = parent;
        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT,
            parent_,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (!hwnd_) {
            return;
        }

        // TTF_TRACK ツールを1つだけ登録（手動で位置・表示を制御するため）
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.hwnd = parent_;
        ti.uId = TOOL_ID;
        ti.lpszText = const_cast<LPWSTR>(L"");
        SendMessageW(hwnd_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));

        // 最大幅を設定（長いURLの折り返し用）
        SendMessageW(hwnd_, TTM_SETMAXTIPWIDTH, 0, 600);
    }

    // ホバー対象が変わったらタイマーのリセットが必要かを返す。
    // screen_pos: マウスのスクリーン座標（ツールチップ表示位置用）。
    // 戻り値: タイマーを再設定すべきなら true。
    bool Update(const TooltipTarget& target, POINT screen_pos)
    {
        if (target == current_) {
            return false;
        }

        Hide();
        current_ = target;
        show_pos_ = screen_pos;

        if (current_.IsEmpty()) {
            return false;
        }
        return true;
    }

    // 遅延タイマー発火時に呼び出す。ツールチップを表示する。
    void Show()
    {
        if (!hwnd_ || current_.IsEmpty()) {
            return;
        }

        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = parent_;
        ti.uId = TOOL_ID;
        ti.lpszText = const_cast<LPWSTR>(current_.text.c_str());
        SendMessageW(hwnd_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));

        SendMessageW(hwnd_, TTM_TRACKPOSITION, 0,
            MAKELPARAM(show_pos_.x, show_pos_.y + 20));

        SendMessageW(hwnd_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
        visible_ = true;
    }

    // ツールチップを非表示にする。
    void Hide()
    {
        if (!hwnd_ || !visible_) {
            return;
        }
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = parent_;
        ti.uId = TOOL_ID;
        SendMessageW(hwnd_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
        visible_ = false;
    }

    // ダークモード切替時に呼び出す。
    void ApplyDarkMode(bool dark)
    {
        if (hwnd_) {
            ApplyDarkModeToWindow(hwnd_, dark);
        }
    }

    // ターゲットをクリアする（非表示にはしない — Hide() と組み合わせて使う）。
    void ResetTarget() noexcept { current_ = {}; }

private:
    void Destroy()
    {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    static constexpr UINT_PTR TOOL_ID = 1;

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    TooltipTarget current_;
    POINT show_pos_{};
    bool visible_ = false;
};
