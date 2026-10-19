#pragma once
#include "search_bar_controller.h"
#include "app_constants.h"
#include <windows.h>

class App;

// App の Cb 群が共通に持つ、App への転送。定義は app_callbacks.cpp。
struct AppCallbacksBase {
    App* app = nullptr;

    float get_md_pane_height();
};

// AppState（mendo_core）の値メンバとして保持されるため struct サイズは
// mendo_core から見えている必要があるが、メソッド本体は App を触るため
// mendo target 側でしか定義できない。
// mendo_tests は reducer.cpp が SearchBarControllerT<AppSearchBarCallbacks> を
// 介して本 struct のメソッドを参照するので、リンク解決用に
// tests/app_search_bar_callbacks_stub.cpp の no-op スタブを併用する。
struct AppSearchBarCallbacks : AppCallbacksBase {
    // node_index のノードを計測済みにする (検索マッチの行位置を引けるようにする)。
    void ensure_node_layout(int node_index);
};

using SearchBarController = SearchBarControllerT<AppSearchBarCallbacks>;

extern template class SearchBarControllerT<AppSearchBarCallbacks>;
