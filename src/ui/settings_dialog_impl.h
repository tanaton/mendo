#pragma once
// SettingsDialog の private 実装ヘッダ。settings_dialog.cpp とテストの両方からインクルードする。
#include "settings_dialog.h"
#include "i18n.h"
#include "ui_types.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windows.h>
#include <array>
#include <iterator>
#include <string>
#include <string_view>

namespace settings_dialog_constants {
inline constexpr float MIN_WIDTH = 320.0f;
inline constexpr float PAD_X = 16.0f;
inline constexpr float TITLE_HEIGHT = 44.0f;
inline constexpr float TITLE_FONT_SCALE = 1.25f;
inline constexpr float HEADING_HEIGHT = 24.0f;
inline constexpr float ROW_HEIGHT = 28.0f;
inline constexpr float CHECK_WIDTH = 24.0f;
inline constexpr float HOVER_MARGIN_X = 8.0f;
// ホバーの塗りと削除ボタンの枠で共有する (ボタンのホバーが枠からはみ出さないように)。
inline constexpr float HOVER_CORNER = 4.0f;
inline constexpr float SECTION_GAP = 10.0f;
inline constexpr float BUTTON_PAD_X = 14.0f;
inline constexpr float BUTTON_MIN_WIDTH = 72.0f;
inline constexpr float BUTTON_GAP = 16.0f;
inline constexpr float BOTTOM_PAD = 14.0f;
inline constexpr float CORNER = 8.0f;
inline constexpr float FRAME_WIDTH = 2.0f;
inline constexpr float FRAME_ALPHA = 0.45f;
inline constexpr float ICON_FONT_SIZE = 12.0f;
inline constexpr float HEADING_ALPHA = 0.7f;

inline constexpr wchar_t GLYPH_CHECKMARK[] = L"\xE73E";

// 言語の一覧の行。先頭は OS に合わせる (nullptr)、残りは i18n::kAllStrings の並び。
inline constexpr size_t kLanguageRowCount = std::size(i18n::kAllStrings) + 1;
} // namespace settings_dialog_constants

// "0 B" / "512 KB" / "12.3 MB" の形にする。
std::wstring FormatByteSize(uint64_t bytes);

struct SettingsDialog::Impl {
    enum class HitKind : uint8_t {
        None,
        Language,
        ClearCache,
    };
    struct Hit {
        HitKind kind = HitKind::None;
        // Language のときの行。
        size_t row = 0;
        bool operator==(const Hit&) const = default;
    };

    struct Layout {
        float width = 0.0f;
        float height = 0.0f;
        DipRect language_heading{};
        std::array<DipRect, settings_dialog_constants::kLanguageRowCount> languages{};
        float separator_y = 0.0f;
        DipRect cache_heading{};
        DipRect cache_size{};
        DipRect clear{};
    };

    static constexpr const i18n::Strings* LanguageOfRow(size_t row) noexcept
    {
        return row == 0 ? nullptr : i18n::kAllStrings[row - 1];
    }

    static bool RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateDialogWindow();
    void DestroyDialogWindow() noexcept;
    // 今の layout の大きさにして、around の中央に置く。
    void PlaceWindow(const RECT& around);

    // 開いたときと言語が変わったときに、文言と書式から組み直す。
    void Rebuild();
    void CreateTextFormats();
    float TextWidth(std::wstring_view text, IDWriteTextFormat* fmt) const;
    void ComputeLayout();
    Hit HitTest(float x, float y) const noexcept;
    bool CanClearCache() const noexcept
    {
        return cache_bytes > 0 && static_cast<bool>(on_clear_cache);
    }
    void Activate(const Hit& hit);
    // カーソルの位置からホバーを引き直す。
    void RefreshHover();

    bool CreateDeviceResources();
    void Paint();
    void DrawHover(const DipRect& rect);
    void DrawLabel(std::wstring_view text, IDWriteTextFormat* fmt, const DipRect& rect, ID2D1Brush* brush);

    HWND hwnd = nullptr;
    HWND owner = nullptr;
    bool done = false;
    Hit hovered{};

    ID2D1Factory* d2d_factory = nullptr;
    IDWriteFactory* dwrite_factory = nullptr;
    const Theme* theme = nullptr;
    float dpi_scale = 1.0f;
    uint64_t cache_bytes = 0;
    std::function<void(const i18n::Strings*)> on_language;
    std::function<void()> on_clear_cache;

    // 「OS の設定に合わせる (日本語)」のように、今 OS に合わせると何語になるかを添える。
    std::wstring system_language_text;
    std::wstring cache_size_text;
    Layout layout{};

    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_frame;
    // 区切り線とボタンの枠。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_border;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_text;
    // 見出し。押せない文字 (brush_gray) より濃くする。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_sub;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_gray;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_check;
    // UI の文言は表示中の言語のロケールで組む (OS と違う言語を選んでも漢字の字形をその言語に合わせる)。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_title;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_text;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_text_center;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_icon;
    // 言語名はそれぞれの言語のロケールで組む。i18n::kAllStrings の並び。
    std::array<Microsoft::WRL::ComPtr<IDWriteTextFormat>, std::size(i18n::kAllStrings)> fmt_language_names;
};
