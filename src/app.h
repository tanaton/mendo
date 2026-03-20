#pragma once
#include "renderer.h"
#include "mermaid.h"
#include "file_loader.h"
#include "file_explorer.h"
#include "document.h"
#include "document_service.h"
#include "hit_test_service.h"
#include "pane.h"
#include "pane_layout.h"
#include "pane_controller.h"
#include "document_utils.h"
#include "layout_cache.h"
#include "layout_service.h"
#include "viewport_manager.h"
#include "app_controller.h"
#include "nav_history.h"
#include "navigation_service.h"
#include "mouse_gesture.h"
#include "config_service.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <optional>
#include <memory>

// Apply dark mode styling to window title bar and scrollbar.
void ApplyDarkModeToWindow(HWND hwnd, bool dark);

class App {
public:
    bool Init(HWND hwnd);

    void LoadMarkdownFile(const std::wstring& path);
    std::wstring LoadLastFilePath() const;

    // Event handlers called from Win32Window
    void OnPaint();
    void OnResize(UINT width, UINT height);
    void OnVScroll(WPARAM wParam);
    void OnMouseWheel(int px, int py, short delta, bool ctrl = false);
    void OnKeyDown(WPARAM key);
    void OnDropFiles(HDROP hDrop);
    void OnDpiChanged(UINT dpi, const RECT* suggested);

    void OnLButtonDown(int px, int py);
    void OnLButtonUp(int px, int py);
    void OnMouseMove(int px, int py);
    void OnLButtonDblClk(int px, int py);
    void OnContextMenu(int screen_x, int screen_y);
    bool OnRButtonDown(int px, int py);
    bool OnRButtonUp(int px, int py);
    void OnRButtonMove(int px, int py);

    // Non-button mouse hover handling
    void OnMouseHover(int px, int py);

    // Mouse X-button navigation
    void OnXButtonBack();
    void OnXButtonForward();

    // Timer callbacks
    void OnSmoothScrollTimer();
    void OnFileWatchTimer();
    void OnDeferredLayoutTimer();
    void OnLoadingAnimTimer();
    void OnAppLoadFile();
    void OnCaptureChanged();
    void OnDestroy();

    // Sizing state
    void OnEnterSizeMove();
    void OnExitSizeMove();

    // Cursor state for WM_SETCURSOR
    bool IsRenderReady() const noexcept { return renderer_.GetRenderTarget() != nullptr; }

    // Expose DPI scale for Win32Window cursor/invalidation
    float GetDpiScale() const noexcept { return cached_dpi_scale_; }

private:
    // Execute actions returned by AppController
    void ExecuteActions(const ActionList& actions);

    // DIP conversion
    struct DipPoint { float x, y; };
    DipPoint PixelToDip(int px, int py) const;

    // Hit testing
    using HitResult = HitTestService::HitResult;
    HitResult HitTest(int screen_x, int screen_y) const;
    std::optional<std::wstring> GetLinkAtHit(const HitResult& hit) const;
    void HandleLinkClick(const std::wstring& url);
    void NavigateToAnchor(const std::wstring& anchor);
    void ApplyNavigateResult(const NavigationService::NavigateResult& result);
    void NavigateBack();
    void NavigateForward();
    void PushNavHistory();
    void CopySelectionToClipboard() const;
    void SelectAll();
    void ClearSelection();

    // Scrollbar helpers
    PaneScrollInfo ComputePaneScrollInfo(const PaneRect& rect, float total_content) const;
    void HandleScrollbarClick(float dip_y, const PaneScrollInfo& info,
                              ScrollState& scroll, bool& cache_dirty);
    void HandleScrollbarDrag(float dip_y, const PaneScrollInfo& info,
                             ScrollState& scroll, bool& cache_dirty);

    // Layout / scroll
    void UpdateLayoutAndScroll(float desired_scroll);
    void UpdateScrollBar();
    void UpdateScrollBar(float md_pane_height);
    void ScrollTo(float position);
    void SmoothScrollBy(float delta);
    void UpdateSmoothScroll();
    void StopSmoothScroll();
    void SyncMaxScroll();
    void SyncMaxScroll(float md_pane_height);
    int FindFirstVisibleNode() const;
    void AnchorCompensateScroll(int anchor_idx, float anchor_y_before);
    void AnchorCompensateScroll(int anchor_idx, float anchor_y_before, float md_pane_height);
    void OnResizeEnd();
    void OnDeferredLayout();
    void InvalidateMdPane();
    void ReloadCurrentFile();
    void DoLoadMarkdownFile();
    void UpdateTitleBar();
    void SaveLastFilePath();

    // Pane layout
    ::PaneLayout GetPaneLayout() const;
    ::PaneZone PaneAtPoint(float dip_x, float dip_y) const;
    float GetMarkdownPaneWidth() const;

    void RequestMermaidRenders();

    // Dark mode
    void ToggleDarkMode();
    void SaveDarkMode();
    bool LoadDarkMode() const;

    // Zoom
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    void ApplyZoom(float new_zoom);
    void SaveZoomLevel();
    int LoadZoomIndex() const;

public:
    // Timer IDs (shared with Win32Window for message routing)
    static constexpr UINT_PTR TIMER_SMOOTH_SCROLL = 1;
    static constexpr UINT_PTR TIMER_FILE_WATCH = 2;
    static constexpr UINT_PTR TIMER_DEFERRED_LAYOUT = 3;
    static constexpr UINT_PTR TIMER_LOADING_ANIM = 4;
    static constexpr UINT WM_APP_LOAD_FILE = WM_APP + 1;

private:
    // Win32 handle
    HWND hwnd_ = nullptr;
    float cached_dpi_scale_ = 1.0f;

    // Cached system cursors
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_ibeam_ = nullptr;
    HCURSOR cursor_sizewe_ = nullptr;

    // HitTest throttle
    POINT last_md_hit_pos_ = {LONG_MIN, LONG_MIN};
    bool last_md_cursor_hand_ = false;

    // Core services
    Renderer renderer_;
    MermaidRenderer mermaid_renderer_;
    FileLoader file_loader_;
    DocumentService doc_service_{file_loader_};
    AppController controller_;
    ConfigService config_;

    // Domain state
    Document doc_;
    LayoutCache layout_cache_;
    ViewportManager viewport_;
    std::optional<LayoutService> layout_service_;

    bool is_sizing_ = false;

    // Loading state
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::wstring loading_path_;

    // 3-pane state
    FileExplorer file_explorer_;
    PaneController panes_;
    NavHistory nav_history_;
    NavigationService nav_service_{nav_history_};
    MouseGesture gesture_;
    HitTestService hit_test_;

    bool dark_mode_ = false;
    float last_mermaid_content_width_ = 0.0f;

    // Navigation overlay
    using NavButtonHover = HitTestService::NavButtonHover;
    NavButtonHover nav_hover_ = NavButtonHover::None;
};
