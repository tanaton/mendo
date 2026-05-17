#pragma once
#include "search_bar_controller.h"
#include "app_constants.h"
#include <windows.h>

class App;

// AppState（mendo_core）の値メンバとして保持されるため struct サイズは
// mendo_core から見えている必要があるが、メソッド本体は App を触るため
// mendo target 側でしか定義できない。
// mendo_tests は reducer.cpp が SearchBarControllerT<AppSearchBarCallbacks> を
// 介して本 struct のメソッドを参照するので、リンク解決用に
// tests/app_search_bar_callbacks_stub.cpp の no-op スタブを併用する。
struct AppSearchBarCallbacks {
    App* app = nullptr;

    void invalidate();
    void invalidate_search_bar();
    void set_timer(app_timer::Id id, UINT ms);
    void kill_timer(app_timer::Id id);
    void focus_select_all();
    void unfocus();
    float get_md_pane_height();
    void on_scroll_changed(float md_pane_height);
    void on_wrap_around();
};

using SearchBarController = SearchBarControllerT<AppSearchBarCallbacks>;

extern template class SearchBarControllerT<AppSearchBarCallbacks>;
