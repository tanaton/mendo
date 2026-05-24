#pragma once
#include "ui_types.h"
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
    bool show_file_items = false; // MdPaneの場合のみtrue
    const Theme* theme = nullptr;
};

// 戻る/進むボタンの横並び表示のため自前描画。
class ContextMenu {
public:
    enum class ItemType : uint8_t {
        NavRow,
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

    ContextMenu();
    ~ContextMenu();

    ContextMenu(const ContextMenu&) = delete;
    ContextMenu& operator=(const ContextMenu&) = delete;

    // App の初期化時に 1 回呼ぶ。
    void Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory);

    int Show(HWND owner_hwnd, const ContextMenuParams& params);

    int HitTest(float x, float y) const noexcept;
    int NavHitTest(float x, float y) const noexcept;

    const std::vector<Item>& GetItems() const noexcept;
    const NavRowLayout& GetNavLayout() const noexcept;
    float GetMenuWidth() const noexcept;
    float GetMenuHeight() const noexcept;

    // テスト補助 API（Impl の BuildItems / CreateTextFormats / ComputeLayout へ
    // 直接 forward するフックポイント）。定義は mendo_core に含まれるため
    // Release/LTCG のように /OPT:REF が効くビルドでは mendo 実行体から未参照で
    // リンカに除去される可能性があるが、Debug 等の未最適化ビルドではバイナリ
    // に残り得る。"Test" プレフィックスで用途を明示している。
    void TestBuildItems(const ContextMenuParams& params);
    void TestCreateTextFormats(const Theme& theme);
    void TestComputeLayout();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
