#pragma once
#include "ui_types.h"
#include <cstdint>
#include <functional>
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
    // 右クリックした領域 (本文・ファイル一覧・目次) の倍率。無ければズームの行を出さない。
    // メニューを開いたまま on_zoom で変わるので、表示中は生きている倍率を指す。
    const ZoomLevel* zoom = nullptr;
    // ズームの行のボタンで呼ぶ。ブラウザと同じくメニューは閉じない。本体の再描画はメニューのループが配る。
    std::function<void(ZoomDirection)> on_zoom;
    const Theme* theme = nullptr;
};

// 戻る/進むボタンの横並び表示のため自前描画。
class ContextMenu {
public:
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

    ContextMenu();
    ~ContextMenu();

    ContextMenu(const ContextMenu&) = delete;
    ContextMenu& operator=(const ContextMenu&) = delete;

    // App の初期化時に 1 回呼ぶ。
    void Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory);

    int Show(HWND owner_hwnd, const ContextMenuParams& params);

    // 定義は context_menu_impl.h。テストはウィンドウを作らずに Impl の各段階を直接呼ぶ。
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
