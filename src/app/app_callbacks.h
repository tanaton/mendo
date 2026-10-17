#pragma once
#include "app_search_bar_callbacks.h"
#include "resource_manager.h"
#include "side_effect.h"
#include "side_effect_executor.h"
#include "ui_types.h"
#include <string_view>

// 定義は app_callbacks.cpp（mendo target 限定）。App は前方宣言のみなのでメソッドは out-of-line。
struct AppSideEffectCallbacks : AppCallbacksBase {
    void load_file(std::wstring_view path);
    void reload_file();
    void open_file_dialog();
    void cancel_other_file_load();
    void renderer_resize(UINT w, UINT h);
    void renderer_set_dpi(float dpi);
    void perform_resize_end();
    void perform_sizing_update();
    void apply_theme_change(const effect::ApplyThemeChange& e);
    void show_context_menu(int x, int y);
    void sync_toc_active(bool auto_scroll);

    void schedule_bitmap_manage();
};

using SideEffectExecutor = SideEffectExecutorT<AppSideEffectCallbacks>;

extern template class SideEffectExecutorT<AppSideEffectCallbacks>;

struct AppResourceManagerCallbacks : AppCallbacksBase {
    float get_content_width();
    const Theme& get_theme();
    // 再描画の要求までを含む (anchored も同様)。
    void recompute_layout(layout::HeightChangeRange changed);
    void recompute_layout_anchored(layout::HeightChangeRange changed);
};

using ResourceManager = ResourceManagerT<AppResourceManagerCallbacks>;

extern template class ResourceManagerT<AppResourceManagerCallbacks>;
