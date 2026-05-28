#pragma once
// ContextMenu の private 実装ヘッダ。context_menu_logic.cpp（mendo_core）と
// context_menu.cpp（Win32/D2D 本体）の両方からインクルードする。
// 本ヘッダ自体は Impl 構造体の定義（D2D フィールドを含む）を提供する。
#include "context_menu.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windows.h>
#include <memory_resource>
#include <string>

namespace context_menu_constants {
inline constexpr float ITEM_HEIGHT = 28.0f;
inline constexpr float NAV_BTN_SIZE = 28.0f;
inline constexpr float NAV_BTN_GAP = 16.0f;
inline constexpr float NAV_ROW_PAD_Y = 5.0f;
inline constexpr float NAV_BTN_CORNER = 4.0f;
inline constexpr float SEPARATOR_HEIGHT = 9.0f;
inline constexpr float PAD_X = 28.0f;
inline constexpr float PAD_Y = 4.0f;
inline constexpr float CHECK_WIDTH = 20.0f;
inline constexpr float MENU_CORNER = 8.0f;
inline constexpr float MENU_BORDER = 1.0f;
inline constexpr float MIN_MENU_WIDTH = 160.0f;
inline constexpr float ICON_FONT_SIZE = 14.0f;

inline constexpr wchar_t GLYPH_BACK[] = L"\xE72B";
inline constexpr wchar_t GLYPH_FORWARD[] = L"\xE72A";
inline constexpr wchar_t GLYPH_CHECKMARK[] = L"\xE73E";
} // namespace context_menu_constants

struct ContextMenu::Impl {
    static bool RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // CreatePopupWindow が false を返したら Show() は早期 return。
    void PrepareContent(const ContextMenuParams& params);
    bool CreatePopupWindow(int screen_x, int screen_y);
    void RunModalLoop();

    void BuildItems(const ContextMenuParams& params);
    void ComputeLayout();
    bool EnsureRenderTarget(float dpi);
    void CreateBrushes();
    bool RecreateDeviceResources();
    void CreateTextFormats(const Theme& theme);

    void Paint();
    void DrawNavRow();
    void DrawSeparator(const Item& item);
    void DrawTextItem(const Item& item);

    int HitTest(float x, float y) const noexcept;
    int NavHitTest(float x, float y) const noexcept;

    HWND hwnd = nullptr;
    HWND owner = nullptr;

    bool done = false;
    int selected_id = 0;

    int hovered_id = 0;
    int hovered_nav = 0;

    std::vector<Item> items;
    NavRowLayout nav_layout{};
    float menu_width = 0.0f;
    float menu_height = 0.0f;

    ID2D1Factory* d2d_factory = nullptr;
    IDWriteFactory* dwrite_factory = nullptr;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_border;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_text;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_gray;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_check;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_text;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_icon;

    // 同一フォントでの Show 連発時に IDWriteTextFormat の再生成を抑止するためのキー。
    std::pmr::wstring cached_fmt_font_family;
    std::pmr::wstring cached_fmt_icon_font;
    float cached_fmt_font_size = 0.0f;

    const Theme* theme = nullptr;
    float dpi_scale = 1.0f;

    ~Impl()
    {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};
