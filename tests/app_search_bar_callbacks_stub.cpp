// mendo_tests は mendo_core のみリンクするが、reducer.cpp が
// SearchBarControllerT<AppSearchBarCallbacks> のメソッドを参照しており、
// 各メソッドが cb_.X() 経由で AppSearchBarCallbacks::X を要求する。
// 実本体は src/app/app_search_bar_callbacks.cpp（mendo 実行ファイル target）に
// あるためテストでは見えない。本 TU で no-op スタブを提供し、
// テストでの未解決外部シンボルを防ぐ。
//
// なお、SearchBarController 自身の挙動を観測したいテスト
// (test_search_bar_controller.cpp) は独自の Cb 型 + 別インスタンス化で
// 観測するため、ここのスタブは「reducer 経由でたまたま入る経路」の
// リンク充足用に過ぎない。
#include "search_bar_controller_impl.h"
#include "app_search_bar_callbacks.h"

void AppSearchBarCallbacks::invalidate()
{}
void AppSearchBarCallbacks::invalidate_search_bar()
{}
void AppSearchBarCallbacks::set_timer(app_timer::Id, UINT)
{}
void AppSearchBarCallbacks::kill_timer(app_timer::Id)
{}
void AppSearchBarCallbacks::focus_select_all()
{}
void AppSearchBarCallbacks::unfocus()
{}
float AppSearchBarCallbacks::get_md_pane_height()
{
    return 0.0f;
}
void AppSearchBarCallbacks::on_scroll_changed(float)
{}
void AppSearchBarCallbacks::on_wrap_around()
{}

template class SearchBarControllerT<AppSearchBarCallbacks>;
