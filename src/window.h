#pragma once
#include "renderer.h"
#include "mermaid.h"
#include "file_loader.h"
#include "file_explorer.h"
#include "toc.h"
#include "pane.h"
#include "pane_layout.h"
#include "pane_controller.h"
#include "document_utils.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "app_controller.h"
#include "nav_history.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <optional>

// Apply dark mode styling to window title bar and scrollbar.
void ApplyDarkModeToWindow(HWND hwnd, bool dark);

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

    // Execute actions returned by AppController
    void ExecuteActions(const ActionList& actions);

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
    HitResult HitTestTable(const Node& node, const NodeLayoutEntry& entry,
                           int node_index, float dip_x, float dip_y) const;
    std::optional<std::wstring> GetLinkAtHit(const HitResult& hit) const;
    void HandleLinkClick(const std::wstring& url);
    void NavigateToAnchor(const std::wstring& anchor);
    void NavigateBack();
    void NavigateForward();
    void PushNavHistory();
    void NavigateToEntry(const NavEntry& entry);
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
    AppController controller_;

    // Cached system cursors
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_ibeam_ = nullptr;
    HCURSOR cursor_sizewe_ = nullptr;

    // WM_MOUSEMOVE HitTest throttle: skip expensive hit-test when mouse barely moved
    POINT last_md_hit_pos_ = {LONG_MIN, LONG_MIN};
    bool last_md_cursor_hand_ = false;

    std::vector<Node> nodes_;
    LayoutCache layout_cache_;
    ViewportManager viewport_;
    std::wstring current_file_;

    static constexpr UINT_PTR TIMER_SMOOTH_SCROLL = 1;
    static constexpr UINT_PTR TIMER_FILE_WATCH = 2;
    static constexpr UINT_PTR TIMER_DEFERRED_LAYOUT = 3;
    static constexpr UINT_PTR TIMER_LOADING_ANIM = 4;
    static constexpr UINT WM_APP_LOAD_FILE = WM_APP + 1;
    bool is_sizing_ = false;

    // Loading state
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::wstring loading_path_;

    // 3-pane state
    FileExplorer file_explorer_;
    TableOfContents toc_;
    PaneController panes_;
    NavHistory nav_history_;

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

    bool dark_mode_ = false;
    float last_mermaid_content_width_ = 0.0f;

    // Navigation overlay buttons
    enum class NavButtonHover { None, Back, Forward };
    NavButtonHover nav_hover_ = NavButtonHover::None;
    NavButtonHover NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const;
};
