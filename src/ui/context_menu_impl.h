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
#include <optional>
#include <string>
#include <unordered_map>

namespace context_menu_constants {
inline constexpr float ITEM_HEIGHT = 28.0f;
inline constexpr float CTX_NAV_BTN_SIZE = 28.0f;
inline constexpr float CTX_NAV_BTN_GAP = 16.0f;
inline constexpr float CTX_NAV_BUTTONS_WIDTH = 2 * CTX_NAV_BTN_SIZE + CTX_NAV_BTN_GAP;
inline constexpr float CTX_NAV_ROW_PAD_Y = 5.0f;
inline constexpr float CTX_NAV_BTN_CORNER = 4.0f;
inline constexpr float SEPARATOR_HEIGHT = 9.0f;
inline constexpr float PAD_X = 28.0f;
inline constexpr float PAD_Y = 4.0f;
inline constexpr float CHECK_WIDTH = 20.0f;
inline constexpr float MENU_CORNER = 8.0f;
inline constexpr float MENU_BORDER = 1.0f;
inline constexpr float MIN_MENU_WIDTH = 160.0f;
inline constexpr float ICON_FONT_SIZE = 14.0f;
inline constexpr float SEPARATOR_MARGIN_X = 12.0f;
inline constexpr float HOVER_MARGIN_X = 4.0f;
inline constexpr float HOVER_CORNER = 4.0f;
inline constexpr float CHECK_LEFT = 8.0f;
inline constexpr float TEXT_PAD_RIGHT = 8.0f;
inline constexpr float ZOOM_BTN_SIZE = ITEM_HEIGHT;
// 「500%」が収まる幅。
inline constexpr float ZOOM_PERCENT_WIDTH = 48.0f;
// 拡大と元に戻すの間。−/+ と同じ組に見せないよう空ける。
inline constexpr float ZOOM_RESET_GAP = 8.0f;
inline constexpr float ZOOM_LABEL_GAP = 16.0f;
inline constexpr float ZOOM_PAD_RIGHT = 8.0f;
inline constexpr float ZOOM_CONTROLS_WIDTH = 3 * ZOOM_BTN_SIZE + ZOOM_PERCENT_WIDTH + ZOOM_RESET_GAP;

inline constexpr wchar_t GLYPH_BACK[] = L"\xE72B";
inline constexpr wchar_t GLYPH_FORWARD[] = L"\xE72A";
inline constexpr wchar_t GLYPH_CHECKMARK[] = L"\xE73E";
inline constexpr wchar_t GLYPH_ZOOM_OUT[] = L"\xE738";
inline constexpr wchar_t GLYPH_ZOOM_IN[] = L"\xE710";
inline constexpr wchar_t GLYPH_ZOOM_RESET[] = L"\xE7A7";
} // namespace context_menu_constants

struct ContextMenu::Impl {
    static bool RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // CreatePopupWindow が false を返したら Show() は早期 return。
    // 開くたびにウィンドウと描画先を作ると数 ms かかるので、前回のものがあれば動かして使い回す。
    bool CreatePopupWindow(int screen_x, int screen_y);
    // 閉じても壊さずに隠す。まだ前面にいればオーナーへ前面を返してから隠す。
    void HidePopupWindow() noexcept;
    // rt はウィンドウを壊してから捨てる。
    void DestroyPopupWindow() noexcept;
    void RunModalLoop();

    void BuildItems(const ContextMenuParams& params);
    void ComputeLayout();
    bool CreateDeviceResources();
    // 使い回す描画先を今回の大きさ・DPI・テーマ色に合わせる。無ければ作る。
    bool PrepareDeviceResources(UINT32 pixel_w, UINT32 pixel_h);
    void ApplyBrushColors();
    void CreateTextFormats(const Theme& theme);
    // 項目の文言は i18n の静的な文字列なので、書式が同じ間は幅を覚えて TextLayout を作り直さない。
    float TextWidth(std::wstring_view text);

    void Paint();
    void DrawNavRow();
    void DrawZoomRow(const Item& item);
    void DrawIconButton(const DipRect& rect, const wchar_t* glyph, bool enabled, bool hovered, float corner);
    void DrawSeparator(const Item& item);
    void DrawTextItem(const Item& item);

    // 無効項目・区切り上は 0。
    int HitTest(float x, float y) const noexcept;
    int NavHitTest(float x, float y) const noexcept;
    // 押せないボタンの上は無し。
    std::optional<ZoomDirection> ZoomHitTest(float x, float y) const noexcept;

    HWND hwnd = nullptr;
    HWND owner = nullptr;

    bool done = false;
    int selected_id = 0;

    int hovered_id = 0;
    // IDM_NAV_BACK / IDM_NAV_FORWARD / 0
    int hovered_nav = 0;
    std::optional<ZoomDirection> hovered_zoom;

    std::vector<Item> items;
    NavRowLayout nav_layout{};
    ZoomRowLayout zoom_layout{};
    // 表示中だけ呼び出し側から借りる。
    const ZoomLevel* zoom = nullptr;
    std::function<void(ZoomDirection)> on_zoom;
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
    // 倍率の数字。中身が変わるので TextWidth で測って寄せず、書式で中央に置く。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_text_center;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_icon;

    // 同一フォントでの Show 連発時に IDWriteTextFormat の再生成を抑止するためのキー。
    std::pmr::wstring cached_fmt_font_family;
    std::pmr::wstring cached_fmt_icon_font;
    float cached_fmt_font_size = 0.0f;
    // 文言の先頭ポインタ -> 幅。書式を作り直したら捨てる。
    std::unordered_map<const wchar_t*, float> text_widths;

    const Theme* theme = nullptr;
    float dpi_scale = 1.0f;

    ~Impl()
    {
        DestroyPopupWindow();
    }
};
