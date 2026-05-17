#pragma once
#include "resource_manager.h"
#include "app_constants.h"
#include <windows.h>

class App;

// Cb 定義は app_resource_manager_callbacks.cpp（mendo target 限定）。
// app.h を forward decl で受けるためメソッドは out-of-line。
struct AppResourceManagerCallbacks {
    App* app = nullptr;

    void invalidate();
    void set_timer(app_timer::Id id, UINT ms);
    void kill_timer(app_timer::Id id);
    float get_content_width();
    float get_viewport_height();
    float get_indent_width();
    void recompute_layout();
    void recompute_layout_anchored();
};

using ResourceManager = ResourceManagerT<AppResourceManagerCallbacks>;

extern template class ResourceManagerT<AppResourceManagerCallbacks>;
