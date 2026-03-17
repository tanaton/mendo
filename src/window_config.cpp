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
    if (viewport_.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        new_theme.ApplyZoom(ZOOM_STEPS[viewport_.GetZoomIndex()]);
    }
    renderer_.SetTheme(new_theme);

    // Apply dark mode to title bar and scrollbar
    ApplyDarkModeToWindow(hwnd_, dark_mode_);

    // Re-layout with new theme (reuses existing parsed nodes)
    for (size_t i = 0; i < nodes_.size(); ++i) {
        layout_cache_[i].text_layout.Reset();
        layout_cache_[i].effects_applied = false;
        layout_cache_[i].inline_code_bgs.clear();
        // Clear mermaid bitmaps so they re-render with correct theme.
        // Keep width/height so layout uses the previous size
        // as a placeholder, preventing scroll position jumps.
        if (nodes_[i].code_language == SyntaxLanguage::Mermaid) {
            layout_cache_.GetDiagram(i).bitmap.Reset();
        }
    }
    mermaid_renderer_.ClearCache();

    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().UpdateTheme(renderer_.GetTheme());
    renderer_.GetLayout().RecreateFormats();
    renderer_.GetLayout().LayoutNodes(nodes_, layout_cache_, md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);
    float total_height = 0;
    if (!nodes_.empty()) {
        size_t last = nodes_.size() - 1;
        total_height = layout_cache_[last].y_position + layout_cache_[last].height + renderer_.GetTheme().margin_top;
    }
    float viewport_height = renderer_.GetRenderTarget()->GetSize().height;
    viewport_.SyncMaxScroll(total_height, viewport_height);

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
    float z = viewport_.ZoomIn();
    if (z > 0.0f) ApplyZoom(z);
}

void MainWindow::ZoomOut() {
    float z = viewport_.ZoomOut();
    if (z > 0.0f) ApplyZoom(z);
}

void MainWindow::ZoomReset() {
    float z = viewport_.ZoomReset();
    if (z > 0.0f) ApplyZoom(z);
}

void MainWindow::ApplyZoom(float new_zoom) {
    // Remember the first visible node to anchor scroll position
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;
    // Offset from anchor node top to current scroll position (in pre-zoom coords)
    float anchor_offset = viewport_.GetScrollY() - anchor_y_before;

    float old_zoom = renderer_.GetTheme().zoom;
    float zoom_ratio = new_zoom / old_zoom;

    // Scale pane widths and scroll positions proportionally
    panes_.ApplyZoom(zoom_ratio);

    // Update theme sizes and recreate DirectWrite formats (including pane formats)
    renderer_.ApplyZoom(new_zoom);

    // Reset all node layouts
    for (size_t i = 0; i < nodes_.size(); ++i) {
        layout_cache_[i].text_layout.Reset();
        layout_cache_[i].effects_applied = false;
        layout_cache_[i].inline_code_bgs.clear();
    }

    // Re-layout
    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().LayoutNodes(nodes_, layout_cache_,
        md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);

    // Compensate scroll: scale the offset proportionally to the zoom ratio
    if (anchor_idx >= 0 && anchor_idx < static_cast<int>(nodes_.size())) {
        float anchor_y_after = layout_cache_[anchor_idx].y_position;
        viewport_.SetScrollY(anchor_y_after + anchor_offset * zoom_ratio);
    }

    SyncMaxScroll();
    viewport_.SetScrollTarget(viewport_.GetScrollY());

    UpdateScrollBar();
    UpdateTitleBar();
    SaveZoomLevel();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SaveZoomLevel() const {
    config::SaveInt(L"zoom_level.txt", viewport_.GetZoomIndex());
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
