#pragma once
#include "dip_rect.h"
#include <memory>
#include <string_view>
#include <vector>

struct Theme;

// Win32 / D2D / DWrite の公開ヘッダ依存を避けるため最小の前方宣言で済ませる。
// 実体は <windows.h> / <d2d1.h> / <dwrite.h> のものと ABI 互換。
struct HWND__;
using HWND = HWND__*;
struct ID2D1Factory;
struct IDWriteFactory;

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
    void Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory);

    // メニューを表示しユーザーの選択を待つ（モーダル）。
    // 戻り値: 選択されたコマンドID（IDM_NAV_BACK等）、キャンセル時は0。
    int Show(HWND owner_hwnd, const ContextMenuParams& params);

    // ヒットテスト
    int HitTest(float x, float y) const noexcept;
    int NavHitTest(float x, float y) const noexcept;

    // レイアウト結果へのアクセサ
    const std::vector<Item>& GetItems() const noexcept;
    const NavRowLayout& GetNavLayout() const noexcept;
    float GetMenuWidth() const noexcept;
    float GetMenuHeight() const noexcept;

#ifdef MENDO_TESTING
    // テスト専用API。production public API には含めない（mendo_core には常にシンボルが生成される）。
    void TestBuildItems(const ContextMenuParams& params);
    void TestCreateTextFormats(const Theme& theme);
    void TestComputeLayout();
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
