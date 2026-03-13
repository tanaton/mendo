#pragma once
#include "renderer.h"
#include "file_loader.h"
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
    void OnMouseWheel(short delta);
    void OnKeyDown(WPARAM key);
    void OnDropFiles(HDROP hDrop);
    void OnDpiChanged(UINT dpi, const RECT* suggested);

    // Mouse / selection
    void OnLButtonDown(int px, int py);
    void OnLButtonUp(int px, int py);
    void OnMouseMove(int px, int py);
    void OnLButtonDblClk(int px, int py);

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

    void UpdateLayoutAndScroll(float desired_scroll);
    void UpdateScrollBar();
    void ScrollTo(float position);
    void SmoothScrollBy(float delta);
    void UpdateSmoothScroll();
    void ReloadCurrentFile();
    void UpdateTitleBar();

    HWND hwnd_ = nullptr;
    Renderer renderer_;
    FileLoader file_loader_;

    std::vector<RenderNode> nodes_;
    std::wstring current_file_;

    // Scroll state
    float scroll_y_ = 0.0f;
    float scroll_target_ = 0.0f;
    float max_scroll_ = 0.0f;
    bool smooth_scrolling_ = false;
    static constexpr float SCROLL_SPEED = 0.25f;
    static constexpr float SCROLL_EPSILON = 0.5f;
    static constexpr UINT_PTR TIMER_SMOOTH_SCROLL = 1;
    static constexpr UINT_PTR TIMER_FILE_WATCH = 2;

    // Selection state
    TextSelection selection_;
    int anchor_node_ = -1;
    uint32_t anchor_pos_ = 0;
    bool is_dragging_ = false;
    int click_start_x_ = 0;
    int click_start_y_ = 0;
};
