#pragma once
#include "ui_types.h"
#include <functional>
#include <windows.h>

struct ContextMenuParams {
    PopupEnv env;
    int screen_x = 0;
    int screen_y = 0;
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
};

// 戻る/進むボタンの横並び表示のため自前描画。
class ContextMenu {
public:
    // 選んだ項目の ID。何も選ばずに閉じたら 0。
    static int Show(HWND owner_hwnd, const ContextMenuParams& params);

    // 定義は context_menu_impl.h。開くたびに作る。テストはウィンドウを作らずに Impl の各段階を直接呼ぶ。
    struct Impl;
};
