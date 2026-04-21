#pragma once
#include "dip_rect.h"
#include <memory>
#include <string_view>
#include <vector>

struct Theme;

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
    bool file_pane_checked = false;
    bool toc_pane_checked = false;
    bool show_file_items = false;       // MdPaneの場合のみtrue
    const Theme* theme = nullptr;
};

// Win32ポップアップメニューの代わりに自前描画するコンテキストメニュー。
// 戻る/進むボタンの横並び表示が可能。
class ContextMenu {
public:
    // メニュー項目の種類
    enum class ItemType { NavRow, Separator, Text };

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

    ContextMenu();
    ~ContextMenu();

    ContextMenu(const ContextMenu&) = delete;
    ContextMenu& operator=(const ContextMenu&) = delete;

    // D2D/DWriteファクトリーを受け取り初期化する。Appの初期化時に1回呼ぶ。
    // 引数は ID2D1Factory* / IDWriteFactory* を void* として受ける（ヘッダ依存を避けるため）。
    void Init(void* d2d_factory, void* dwrite_factory);

    // メニューを表示しユーザーの選択を待つ（モーダル）。
    // owner_hwnd は親ウィンドウの HWND を void* として受ける。
    // 戻り値: 選択されたコマンドID（IDM_NAV_BACK等）、キャンセル時は0。
    int Show(void* owner_hwnd, const ContextMenuParams& params);

    // ヒットテスト
    int HitTest(float x, float y) const noexcept;
    int NavHitTest(float x, float y) const noexcept;

    // テスト用アクセサ
    const std::vector<Item>& GetItems() const noexcept;
    const NavRowLayout& GetNavLayout() const noexcept;
    float GetMenuWidth() const noexcept;
    float GetMenuHeight() const noexcept;

    // テスト用アクセサ（ビルド構成に関わらず公開。本番では未使用）。
    void TestBuildItems(const ContextMenuParams& params);
    void TestCreateTextFormats(const Theme& theme);
    void TestComputeLayout();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
