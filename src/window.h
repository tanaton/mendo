#pragma once
#include "renderer.h"
#include "mermaid.h"
#include "file_loader.h"
#include "file_explorer.h"
#include "toc.h"
#include "pane.h"
#include "pane_layout.h"
#include "document_utils.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <optional>

class MainWindow {
public:
    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();

    void LoadMarkdownFile(const std::wstring& path);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnPaint();
    void OnResize(UINT width, UINT height);
    void OnVScroll(WPARAM wParam);
    void OnMouseWheel(int px, int py, short delta);
    void OnKeyDown(WPARAM key);
    void OnDropFiles(HDROP hDrop);
    void OnDpiChanged(UINT dpi, const RECT* suggested);

    // Mouse / selection
    void OnLButtonDown(int px, int py);
    void OnLButtonUp(int px, int py);
    void OnMouseMove(int px, int py);
    void OnLButtonDblClk(int px, int py);
    void OnContextMenu(int screen_x, int screen_y);

    // Convert physical pixel coordinates to DIP (Device Independent Pixels)
    struct DipPoint { float x, y; };
    DipPoint PixelToDip(int px, int py) const;

    struct HitResult {
        int node_index = -1;
        uint32_t text_pos = 0;
    };
    HitResult HitTest(int screen_x, int screen_y) const;
    HitResult HitTestTable(const RenderNode& node, int node_index, float dip_x, float dip_y) const;
    std::optional<std::wstring> GetLinkAtHit(const HitResult& hit) const;
    void HandleLinkClick(const std::wstring& url);
    void NavigateToAnchor(const std::wstring& anchor);
    void CopySelectionToClipboard() const;
    void SelectAll();
    void ClearSelection();

    // Scrollbar drag helpers
    struct WinPaneScrollInfo {
        float content_top;
        float content_height;
        float total_content;
        float max_scroll;
        float thumb_height;
    };
    WinPaneScrollInfo ComputePaneScrollInfo(const PaneRect& rect, float total_content) const;
    void HandleScrollbarClick(float dip_y, const WinPaneScrollInfo& info,
                              ScrollState& scroll, bool& cache_dirty);
    void HandleScrollbarDrag(float dip_y, const WinPaneScrollInfo& info,
                             ScrollState& scroll, bool& cache_dirty);

    void UpdateLayoutAndScroll(float desired_scroll);
    void UpdateScrollBar();
    void ScrollTo(float position);
    void SmoothScrollBy(float delta);
    void UpdateSmoothScroll();
    void StopSmoothScroll();
    void SyncMaxScroll();
    int FindFirstVisibleNode() const;
    void AnchorCompensateScroll(int anchor_idx, float anchor_y_before);
    void OnResizeEnd();
    void OnDeferredLayout();
    void InvalidateMdPane();
    void ReloadCurrentFile();
    void DoLoadMarkdownFile();
    void UpdateTitleBar();
    void SaveLastFilePath() const;
public:
    static std::wstring LoadLastFilePath();
private:

    // Pane layout helpers (types defined in pane_layout.h)
    ::PaneLayout GetPaneLayout() const;
    ::PaneZone PaneAtPoint(float dip_x, float dip_y) const;
    float GetMarkdownPaneWidth() const;

    void RequestMermaidRenders();

    HWND hwnd_ = nullptr;
    Renderer renderer_;
    MermaidRenderer mermaid_renderer_;
    FileLoader file_loader_;

    // Cached system cursors
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_ibeam_ = nullptr;
    HCURSOR cursor_sizewe_ = nullptr;

    std::vector<RenderNode> nodes_;
    std::wstring current_file_;

    // Scroll state (MD pane)
    float scroll_y_ = 0.0f;
    float scroll_target_ = 0.0f;
    float max_scroll_ = 0.0f;
    bool smooth_scrolling_ = false;
    static constexpr float SCROLL_SPEED = 0.25f;
    static constexpr float SCROLL_EPSILON = 0.5f;
    static constexpr UINT_PTR TIMER_SMOOTH_SCROLL = 1;
    static constexpr UINT_PTR TIMER_FILE_WATCH = 2;
    static constexpr UINT_PTR TIMER_DEFERRED_LAYOUT = 3;
    static constexpr UINT_PTR TIMER_LOADING_ANIM = 4;
    static constexpr UINT WM_APP_LOAD_FILE = WM_APP + 1;
    bool is_sizing_ = false;
    bool is_scrollbar_tracking_ = false;

    // Loading state
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::wstring loading_path_;

    // Selection state
    TextSelection selection_;
    int anchor_node_ = -1;
    uint32_t anchor_pos_ = 0;
    bool is_dragging_ = false;
    int click_start_x_ = 0;
    int click_start_y_ = 0;

    // 3-pane state
    FileExplorer file_explorer_;
    TableOfContents toc_;
    float pane_file_width_ = 220.0f;
    float pane_toc_width_ = 220.0f;
    bool show_file_pane_ = true;
    bool show_toc_pane_ = true;

    enum class DragTarget { None, Splitter1, Splitter2, FileScrollbar, TocScrollbar };
    DragTarget drag_target_ = DragTarget::None;
    float drag_scroll_offset_ = 0.0f;  // Offset from thumb top to click point
    int hovered_file_index_ = -1;
    int hovered_toc_index_ = -1;
    ScrollState file_scroll_;
    ScrollState toc_scroll_;

    void ToggleDarkMode();
    void SaveDarkMode() const;
    static bool LoadDarkMode();

    // Zoom
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    void ApplyZoom(float new_zoom);
    void SaveZoomLevel() const;
    static int LoadZoomIndex();

    int zoom_index_ = ZOOM_DEFAULT_INDEX; // index into ZOOM_STEPS[]

    bool dark_mode_ = false;
    float last_mermaid_content_width_ = 0.0f;

    static constexpr float PANE_MIN_WIDTH = 100.0f;
    static constexpr float MD_PANE_MIN_WIDTH = 200.0f;
};
