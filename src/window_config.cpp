#include "window.h"
#include "config_store.h"
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// DWMWA_USE_IMMERSIVE_DARK_MODE (supported on Windows 10 1809+ / Windows 11)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

void ApplyDarkModeToWindow(HWND hwnd, bool dark) {
    // Dark title bar
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // Dark scrollbar via explorer theme
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// ---- Dark mode ----

void MainWindow::ToggleDarkMode() {
    dark_mode_ = !dark_mode_;
    Theme new_theme = dark_mode_ ? GetDarkTheme() : GetLightTheme();
    // Preserve current zoom level across theme switch
    if (zoom_index_ != ZOOM_DEFAULT_INDEX) {
        new_theme.ApplyZoom(ZOOM_STEPS[zoom_index_]);
    }
    renderer_.SetTheme(new_theme);

    // Apply dark mode to title bar and scrollbar
    ApplyDarkModeToWindow(hwnd_, dark_mode_);

    // Re-layout with new theme (reuses existing parsed nodes)
    for (auto& node : nodes_) {
        node.text_layout.Reset();
        node.effects_applied = false;
        node.inline_code_bgs.clear();
        // Clear mermaid bitmaps so they re-render with correct theme.
        // Keep diagram_width/diagram_height so layout uses the previous size
        // as a placeholder, preventing scroll position jumps.
        if (node.code_language == SyntaxLanguage::Mermaid) {
            node.diagram_bitmap.Reset();
        }
    }
    mermaid_renderer_.ClearCache();

    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().UpdateTheme(renderer_.GetTheme());
    renderer_.GetLayout().RecreateFormats();
    renderer_.GetLayout().LayoutNodes(nodes_, md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);
    float total_height = nodes_.empty() ? 0 : nodes_.back().y_position + nodes_.back().height + renderer_.GetTheme().margin_top;
    max_scroll_ = std::max(0.0f, total_height - (renderer_.GetRenderTarget()->GetSize().height));
    scroll_y_ = std::min(scroll_y_, max_scroll_);
    scroll_target_ = scroll_y_;

    // Re-render mermaid diagrams with new theme
    RequestMermaidRenders();

    SaveDarkMode();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SaveDarkMode() const {
    config::SaveBool(L"dark_mode.txt", dark_mode_);
}

bool MainWindow::LoadDarkMode() {
    return config::LoadBool(L"dark_mode.txt", false);
}

// ---- Zoom ----

void MainWindow::ZoomIn() {
    if (zoom_index_ < ZOOM_STEP_COUNT - 1) {
        ApplyZoom(ZOOM_STEPS[++zoom_index_]);
    }
}

void MainWindow::ZoomOut() {
    if (zoom_index_ > 0) {
        ApplyZoom(ZOOM_STEPS[--zoom_index_]);
    }
}

void MainWindow::ZoomReset() {
    if (zoom_index_ != ZOOM_DEFAULT_INDEX) {
        zoom_index_ = ZOOM_DEFAULT_INDEX;
        ApplyZoom(ZOOM_STEPS[zoom_index_]);
    }
}

void MainWindow::ApplyZoom(float new_zoom) {
    // Remember the first visible node to anchor scroll position
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? nodes_[anchor_idx].y_position : 0.0f;
    // Offset from anchor node top to current scroll position (in pre-zoom coords)
    float anchor_offset = scroll_y_ - anchor_y_before;

    float old_zoom = renderer_.GetTheme().zoom;
    float zoom_ratio = new_zoom / old_zoom;

    // Scale pane widths proportionally
    pane_file_width_ *= zoom_ratio;
    pane_toc_width_  *= zoom_ratio;

    // Update theme sizes and recreate DirectWrite formats (including pane formats)
    renderer_.ApplyZoom(new_zoom);

    // Reset all node layouts
    for (auto& node : nodes_) {
        node.text_layout.Reset();
        node.effects_applied = false;
        node.inline_code_bgs.clear();
    }

    // Re-layout
    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().LayoutNodes(nodes_,
        md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);

    // Compensate scroll: scale the offset proportionally to the zoom ratio
    if (anchor_idx >= 0 && anchor_idx < static_cast<int>(nodes_.size())) {
        float anchor_y_after = nodes_[anchor_idx].y_position;
        scroll_y_ = anchor_y_after + anchor_offset * zoom_ratio;
    }

    // Scale pane scroll positions
    file_scroll_.scroll_y *= zoom_ratio;
    file_scroll_.max_scroll *= zoom_ratio;
    toc_scroll_.scroll_y *= zoom_ratio;
    toc_scroll_.max_scroll *= zoom_ratio;

    SyncMaxScroll();
    scroll_target_ = scroll_y_;

    UpdateScrollBar();
    UpdateTitleBar();
    SaveZoomLevel();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SaveZoomLevel() const {
    config::SaveInt(L"zoom_level.txt", zoom_index_);
}

int MainWindow::LoadZoomIndex() {
    return config::LoadInt(L"zoom_level.txt", ZOOM_DEFAULT_INDEX, 0, ZOOM_STEP_COUNT - 1);
}

// ---- Last file persistence ----

void MainWindow::SaveLastFilePath() const {
    if (current_file_.empty()) return;
    config::SaveWString(L"last_file.txt", current_file_);
}

std::wstring MainWindow::LoadLastFilePath() {
    std::wstring path = config::LoadWString(L"last_file.txt");
    // Verify the file still exists
    if (!path.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}
