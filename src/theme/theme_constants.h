#pragma once

// Reducer がテーマ定数を参照するためのキャッシュ。
struct ThemeConstants {
    float pane_item_height = 28.0f;
    float pane_header_height = 32.0f;
    float splitter_width = 4.0f;
    float margin_left = 0.0f;
    float margin_right = 0.0f;
    float heading_spacing_above = 0.0f;
    float zoom = 1.0f;
    bool is_dark = false;
};
