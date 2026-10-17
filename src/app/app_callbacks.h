#pragma once
#include "app_search_bar_callbacks.h"
#include "resource_manager.h"
#include "side_effect.h"
#include "ui_types.h"
#include <string_view>

struct AppResourceManagerCallbacks : AppCallbacksBase {
    float get_content_width();
    const Theme& get_theme();
    // 再描画の要求までを含む (anchored も同様)。
    void recompute_layout(layout::HeightChangeRange changed);
    void recompute_layout_anchored(layout::HeightChangeRange changed);
};

using ResourceManager = ResourceManagerT<AppResourceManagerCallbacks>;

extern template class ResourceManagerT<AppResourceManagerCallbacks>;
