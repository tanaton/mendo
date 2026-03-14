#pragma once

struct PaneRect {
    float x, y, width, height;
};

struct ScrollState {
    float scroll_y = 0.0f;
    float max_scroll = 0.0f;
};

// Shared scrollbar constants
static constexpr float PANE_SCROLLBAR_WIDTH = 8.0f;
static constexpr float PANE_SCROLLBAR_THUMB_MIN = 24.0f;
