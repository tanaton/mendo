#pragma once
#include "pane_layout.h"
#include <variant>
#include <vector>
#include <memory_resource>

// ──── Events (platform-agnostic user input) ────

struct KeyDownEvent {
    int key;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct MouseWheelEvent {
    int delta;                          // raw WHEEL_DELTA units
    bool ctrl = false;
    PaneZone zone = PaneZone::MdPane;
};

// ──── Actions (what the app should do) ────

enum class ScrollType { LineUp, LineDown, PageUp, PageDown, Home, End };

// Keyboard/scrollbar scroll (Shell computes concrete delta from type)
struct KeyScrollAction { ScrollType type; };

// Mouse wheel smooth scroll with pre-computed delta
struct SmoothScrollByAction { float delta; };

// Pane (file/toc) scroll
struct ScrollPaneAction { PaneZone pane; float delta; };

struct CopyClipboardAction {};
struct SelectAllAction {};
struct ClearSelectionAction {};

// Toggle a side pane
struct TogglePaneAction { bool file_pane; };   // true = file, false = toc

// Zoom: -1 = out, 0 = reset, +1 = in
struct ZoomAction { int direction; };

struct ReloadFileAction {};
struct OpenFileAction {};
struct ToggleDarkModeAction {};
struct NavigateBackAction {};
struct NavigateForwardAction {};

using AppAction = std::variant<
    KeyScrollAction,
    SmoothScrollByAction,
    ScrollPaneAction,
    CopyClipboardAction,
    SelectAllAction,
    ClearSelectionAction,
    TogglePaneAction,
    ZoomAction,
    ReloadFileAction,
    OpenFileAction,
    ToggleDarkModeAction,
    NavigateBackAction,
    NavigateForwardAction
>;

using ActionList = std::pmr::vector<AppAction>;
