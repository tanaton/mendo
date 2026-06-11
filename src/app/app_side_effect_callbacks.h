#pragma once
#include "side_effect_executor.h"
#include "side_effect.h"
#include "app_constants.h"
#include "ui_types.h"
#include <string_view>
#include <windows.h>

class App;

// Cb 定義は app_side_effect_callbacks.cpp（mendo target 限定）。
// app.h を forward decl で受けるためメソッドは out-of-line。
struct AppSideEffectCallbacks {
    App* app = nullptr;

    void load_file(std::wstring_view path);
    void reload_file();
    void open_file_dialog();
    void invalidate_pane_cache(PaneZone pane);
    void refresh_pane_layout();
    void renderer_resize(UINT w, UINT h);
    void renderer_set_dpi(float dpi);
    void clear_file_cache();
    void perform_resize_end();
    void perform_sizing_update();
    void apply_theme_change(const effect::ApplyThemeChange& e);
    void process_deferred_layout();
    void tick_loading_animation();
    void process_mermaid_batch_timer();
    void process_bitmap_manage();
    void mermaid_init_retry();
    void destroy();
    void handle_parse_complete();
    void show_context_menu(int x, int y);
    void sync_toc_active(bool auto_scroll);

    void schedule_bitmap_manage();
    void on_app_image_loaded();
};

using SideEffectExecutor = SideEffectExecutorT<AppSideEffectCallbacks>;

extern template class SideEffectExecutorT<AppSideEffectCallbacks>;
