#pragma once
#include "popup_window.h"
#include "ui_types.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>
#include <windows.h>

struct ContextMenuParams {
    PopupEnv env;
    int screen_x = 0;
    int screen_y = 0;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool can_edit_file = false;
    bool has_selection = false;
    bool dark_mode_checked = false;
    bool file_pane_checked = false;
    bool toc_pane_checked = false;
    bool show_file_items = false; // MdPaneの場合のみtrue
    // ファイル一覧の項目の上。「新しいウィンドウ」をその項目を開く文言に替える。
    bool on_file_entry = false;
    // 右クリックした領域 (本文・ファイル一覧・目次) の倍率。無ければズームの行を出さない。
    // メニューを開いたまま on_zoom で変わるので、表示中は生きている倍率を指す。
    const ZoomLevel* zoom = nullptr;
    // ズームの行のボタンで呼ぶ。ブラウザと同じくメニューは閉じない。本体の再描画はメニューのループが配る。
    std::function<void(ZoomDirection)> on_zoom;
};

namespace context_menu_constants {
inline constexpr float ITEM_HEIGHT = 28.0f;
inline constexpr float CTX_NAV_BTN_SIZE = 28.0f;
inline constexpr float CTX_NAV_BTN_GAP = 16.0f;
inline constexpr float CTX_NAV_BUTTONS_WIDTH = 2 * CTX_NAV_BTN_SIZE + CTX_NAV_BTN_GAP;
inline constexpr float CTX_NAV_ROW_PAD_Y = 5.0f;
inline constexpr float SEPARATOR_HEIGHT = 9.0f;
inline constexpr float PAD_X = 28.0f;
inline constexpr float PAD_Y = 4.0f;
inline constexpr float CHECK_WIDTH = 20.0f;
inline constexpr float MIN_MENU_WIDTH = 160.0f;
inline constexpr float ICON_FONT_SIZE = 14.0f;
inline constexpr float SEPARATOR_MARGIN_X = 12.0f;
inline constexpr float HOVER_MARGIN_X = 4.0f;
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
inline constexpr wchar_t GLYPH_ZOOM_OUT[] = L"\xE738";
inline constexpr wchar_t GLYPH_ZOOM_IN[] = L"\xE710";
inline constexpr wchar_t GLYPH_ZOOM_RESET[] = L"\xE7A7";
} // namespace context_menu_constants

// 戻る/進むボタンの横並び表示のため自前描画。開くたびに作る。テストはウィンドウを作らずに各段階を直接呼ぶ。
struct ContextMenu : PopupWindow<ContextMenu> {
    static constexpr wchar_t kClassName[] = L"mendoContextMenu";

    // 選んだ項目の ID。何も選ばずに閉じたら 0。
    static int Show(HWND owner_hwnd, const ContextMenuParams& params);

    enum class ItemType : uint8_t {
        NavRow,
        ZoomRow,
        Separator,
        Text
    };

    struct Item {
        ItemType type = ItemType::Text;
        int id = 0;
        std::wstring_view text;
        bool enabled = true;
        bool checked = false;
        DipRect rect{};
    };

    struct NavRowLayout {
        DipRect back_rect{};
        DipRect fwd_rect{};
        bool back_enabled = false;
        bool fwd_enabled = false;
    };

    // 押せるかは開いている間に変わるので、ZoomLevel から都度求める。
    struct ZoomRowLayout {
        DipRect out_rect{};
        DipRect percent_rect{};
        DipRect in_rect{};
        DipRect reset_rect{};
    };

    void BuildItems(const ContextMenuParams& params);
    void ComputeLayout();

    // PopupWindow から呼ばれる。
    void PaintContent();
    bool UpdateHover(float x, float y);
    void OnPress(UINT msg, float x, float y);

    void DrawNavRow();
    void DrawZoomRow(const Item& item);
    void DrawIconButton(const DipRect& rect, const wchar_t* glyph, bool enabled, bool hovered);
    void DrawSeparator(const Item& item);
    void DrawTextItem(const Item& item);

    // 無効項目・区切り上は 0。
    int HitTest(float x, float y) const noexcept;
    int NavHitTest(float x, float y) const noexcept;
    // 戻る/進むのボタン (IDM_NAV_*) か項目の ID。どちらでもなければ 0。
    int CommandAt(float x, float y) const noexcept
    {
        const int nav = NavHitTest(x, y);
        return nav != 0 ? nav : HitTest(x, y);
    }
    // 押せないボタンの上は無し。
    std::optional<ZoomDirection> ZoomHitTest(float x, float y) const noexcept;

    int selected_id = 0;

    // CommandAt の結果。戻る/進むのボタンも項目と同じ ID の並びにある。
    int hovered_id = 0;
    std::optional<ZoomDirection> hovered_zoom;

    std::vector<Item> items;
    NavRowLayout nav_layout{};
    ZoomRowLayout zoom_layout{};
    // 呼び出し側から借りる。倍率の数字は中身が変わるので、測って寄せずに fmt_text_center で中央に置く。
    const ZoomLevel* zoom = nullptr;
    std::function<void(ZoomDirection)> on_zoom;
};
