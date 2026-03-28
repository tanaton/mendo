#pragma once
#include "theme.h"
#include "resource.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windows.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// カスタムコンテキストメニューの表示パラメータ。
struct ContextMenuParams {
    int screen_x = 0;
    int screen_y = 0;
    float dpi_scale = 1.0f;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool has_file = false;
    bool has_selection = false;
    bool dark_mode_checked = false;
    bool show_file_items = false;       // MdPaneの場合のみtrue
    const Theme* theme = nullptr;
};

// Win32ポップアップメニューの代わりに自前描画するコンテキストメニュー。
// 戻る/進むボタンの横並び表示が可能。
class ContextMenu {
public:
    ContextMenu() = default;
    ~ContextMenu();

    ContextMenu(const ContextMenu&) = delete;
    ContextMenu& operator=(const ContextMenu&) = delete;

    // D2D/DWriteファクトリーを受け取り初期化する。Appの初期化時に1回呼ぶ。
    void Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory);

    // メニューを表示しユーザーの選択を待つ（モーダル）。
    // 戻り値: 選択されたコマンドID（IDM_NAV_BACK等）、キャンセル時は0。
    int Show(HWND owner, const ContextMenuParams& params);

    // メニュー項目の種類
    enum class ItemType { NavRow, Separator, Text };

    struct Item {
        ItemType type = ItemType::Text;
        int id = 0;
        std::wstring text;
        bool enabled = true;
        bool checked = false;
        D2D1_RECT_F rect{};
    };

    struct NavRowLayout {
        D2D1_RECT_F back_rect{};
        D2D1_RECT_F fwd_rect{};
        bool back_enabled = false;
        bool fwd_enabled = false;
    };

private:

    // ウィンドウクラス登録（1回のみ）
    static bool RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // メニュー項目構築
    void BuildItems(const ContextMenuParams& params);

    // レイアウト計算（テキスト幅計測→全体サイズ決定→各項目の矩形確定）
    void ComputeLayout();

    // D2Dリソース管理
    bool EnsureRenderTarget(float dpi);
    void CreateBrushes();
    void CreateTextFormats(const Theme& theme);

    // 描画
    void Paint();
    void DrawNavRow(const Item& item);
    void DrawSeparator(const Item& item);
    void DrawTextItem(const Item& item);

public:
    // ヒットテスト
    int HitTest(float x, float y) const;
    int NavHitTest(float x, float y) const;

    // テスト用アクセサ
    const std::vector<Item>& GetItems() const noexcept { return items_; }
    const NavRowLayout& GetNavLayout() const noexcept { return nav_layout_; }
    float GetMenuWidth() const noexcept { return menu_width_; }
    float GetMenuHeight() const noexcept { return menu_height_; }

    // テスト用: BuildItems/ComputeLayoutを外部から呼べるようにする
    void TestBuildItems(const ContextMenuParams& params) { BuildItems(params); }
    void TestCreateTextFormats(const Theme& theme) { CreateTextFormats(theme); }
    void TestComputeLayout() { ComputeLayout(); }

private:

    // 定数
    static constexpr float ITEM_HEIGHT = 28.0f;
    static constexpr float NAV_BTN_SIZE = 28.0f;
    static constexpr float NAV_BTN_GAP = 16.0f;
    static constexpr float NAV_ROW_PAD_Y = 5.0f;
    static constexpr float NAV_BTN_CORNER = 4.0f;
    static constexpr float SEPARATOR_HEIGHT = 9.0f;
    static constexpr float PAD_X = 28.0f;
    static constexpr float PAD_Y = 4.0f;
    static constexpr float CHECK_WIDTH = 20.0f;
    static constexpr float MENU_CORNER = 8.0f;
    static constexpr float MENU_BORDER = 1.0f;

    // Segoe Fluent Icons グリフ
    static constexpr wchar_t GLYPH_BACK[] = L"\xE72B";
    static constexpr wchar_t GLYPH_FORWARD[] = L"\xE72A";
    static constexpr wchar_t GLYPH_CHECKMARK[] = L"\xE73E";

    // ウィンドウ
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    static bool class_registered_;

    // モーダルループ制御
    bool done_ = false;
    int selected_id_ = 0;

    // ホバー状態
    int hovered_id_ = 0;       // 通常項目のホバーID
    int hovered_nav_ = 0;      // ナビ行: -1=戻る, 1=進む, 0=なし

    // メニュー項目
    std::vector<Item> items_;
    NavRowLayout nav_layout_{};
    float menu_width_ = 0.0f;
    float menu_height_ = 0.0f;

    // D2Dリソース
    ID2D1Factory* d2d_factory_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_border_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_gray_;
    ComPtr<ID2D1SolidColorBrush> brush_hover_;
    ComPtr<ID2D1SolidColorBrush> brush_check_;
    ComPtr<IDWriteTextFormat> fmt_text_;
    ComPtr<IDWriteTextFormat> fmt_icon_;

    // 現在のテーマ（Show中のみ有効）
    const Theme* theme_ = nullptr;
    float dpi_scale_ = 1.0f;
};
