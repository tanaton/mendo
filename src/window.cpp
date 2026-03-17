#include "window.h"
#include "parser.h"
#include "document_utils.h"
#include "pane_layout.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <shellscalingapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")

static constexpr wchar_t WINDOW_CLASS[] = L"mendoWindow";

// ---- Helper methods ----

MainWindow::DipPoint MainWindow::PixelToDip(int px, int py) const {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return {static_cast<float>(px), static_cast<float>(py)};
    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;
    return {px / scale, py / scale};
}

MainWindow::WinPaneScrollInfo MainWindow::ComputePaneScrollInfo(
    const PaneRect& rect, float total_content) const {
    auto info = ComputeScrollInfo(rect, renderer_.GetTheme().pane_header_height, total_content);
    WinPaneScrollInfo winfo{};
    winfo.content_top = info.content_top;
    winfo.content_height = info.content_height;
    winfo.total_content = info.total_content;
    winfo.max_scroll = info.max_scroll;
    winfo.thumb_height = info.thumb_height;
    return winfo;
}

void MainWindow::HandleScrollbarClick(float dip_y, const WinPaneScrollInfo& info,
                                      ScrollState& scroll, bool& cache_dirty) {
    float scroll_ratio = (info.max_scroll > 0) ? scroll.scroll_y / info.max_scroll : 0.0f;
    float thumb_y = info.content_top + scroll_ratio * (info.content_height - info.thumb_height);

    if (dip_y >= thumb_y && dip_y <= thumb_y + info.thumb_height) {
        panes_.SetDragScrollOffset(dip_y - thumb_y);
    } else {
        panes_.SetDragScrollOffset(info.thumb_height * 0.5f);
        float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
        float track_range = info.content_height - info.thumb_height;
        float ratio = (track_range > 0) ? (new_thumb_y - info.content_top) / track_range : 0.0f;
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        scroll.scroll_y = ratio * info.max_scroll;
        scroll.max_scroll = info.max_scroll;
        cache_dirty = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::HandleScrollbarDrag(float dip_y, const WinPaneScrollInfo& info,
                                     ScrollState& scroll, bool& cache_dirty) {
    float track_range = info.content_height - info.thumb_height;
    float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
    float ratio = (track_range > 0) ? (new_thumb_y - info.content_top) / track_range : 0.0f;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    scroll.scroll_y = ratio * info.max_scroll;
    scroll.max_scroll = info.max_scroll;
    cache_dirty = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- Window creation / message loop ----

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hbrBackground = nullptr; // We handle painting

    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        WINDOW_CLASS,
        L"mendo",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1600, 900,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) return false;

    if (!renderer_.Init(hwnd_)) return false;

    // Initialize Mermaid renderer (WebView2, async)
    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), [this]() {
        // WebView2 is ready — trigger rendering of any pending mermaid blocks
        RequestMermaidRenders();
    });

    // Apply saved dark mode preference
    dark_mode_ = LoadDarkMode();
    if (dark_mode_) {
        renderer_.SetTheme(GetDarkTheme());
        ApplyDarkModeToWindow(hwnd_, true);
    }

    // Apply saved zoom level
    viewport_.SetZoomIndex(LoadZoomIndex());
    if (viewport_.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        renderer_.ApplyZoom(ZOOM_STEPS[viewport_.GetZoomIndex()]);
    }

    // Cache system cursors
    cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
    cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
    cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
    cursor_sizewe_ = LoadCursorW(nullptr, IDC_SIZEWE);

    // Set up file watch timer (check every 250ms)
    SetTimer(hwnd_, TIMER_FILE_WATCH, 250, nullptr);

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_SIZE:
            OnResize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_ENTERSIZEMOVE:
            is_sizing_ = true;
            StopSmoothScroll();
            return 0;

        case WM_EXITSIZEMOVE:
            is_sizing_ = false;
            OnResizeEnd();
            return 0;

        case WM_VSCROLL:
            OnVScroll(wParam);
            return 0;

        case WM_LBUTTONDOWN:
            OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEMOVE:
            if (wParam & MK_LBUTTON) {
                OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            } else {
                // Update cursor and hover state based on what's under the mouse
                if (renderer_.GetRenderTarget()) {
                    auto dip = PixelToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                    float dip_x = dip.x;
                    float dip_y = dip.y;

                    auto pane_layout = GetPaneLayout();
                    auto zone = DetectPaneZone(dip_x, pane_layout,
                                               renderer_.GetTheme().splitter_width,
                                               panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

                    // Reset hover states — track changes for invalidation
                    int new_file_hover = -1;
                    int new_toc_hover = -1;

                    switch (zone) {
                        case PaneZone::Splitter1:
                        case PaneZone::Splitter2:
                            SetCursor(cursor_sizewe_);
                            break;
                        case PaneZone::FilePane: {
                            SetCursor(cursor_arrow_);
                            float content_top = pane_layout.file_rect.y + renderer_.GetTheme().pane_header_height;
                            float local_y = dip_y - content_top + panes_.FileScroll().scroll_y;
                            new_file_hover = file_explorer_.HitTest(local_y, renderer_.GetTheme().pane_item_height);
                            break;
                        }
                        case PaneZone::TocPane: {
                            SetCursor(cursor_arrow_);
                            float content_top = pane_layout.toc_rect.y + renderer_.GetTheme().pane_header_height;
                            float local_y = dip_y - content_top + panes_.TocScroll().scroll_y;
                            new_toc_hover = toc_.HitTest(local_y, renderer_.GetTheme().pane_item_height);
                            break;
                        }
                        case PaneZone::MdPane: {
                            int mx = GET_X_LPARAM(lParam);
                            int my = GET_Y_LPARAM(lParam);
                            int dx = mx - last_md_hit_pos_.x;
                            int dy = my - last_md_hit_pos_.y;
                            if (dx * dx + dy * dy > 16) { // >4px movement
                                auto hit = HitTest(mx, my);
                                auto link = GetLinkAtHit(hit);
                                last_md_cursor_hand_ = link.has_value();
                                last_md_hit_pos_ = {mx, my};
                            }
                            SetCursor(last_md_cursor_hand_ ? cursor_hand_ : cursor_ibeam_);
                            break;
                        }
                        default:
                            SetCursor(cursor_arrow_);
                            break;
                    }

                    if (panes_.SetHoveredFileIndex(new_file_hover)) {
                        renderer_.InvalidateFilePaneCache();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    if (panes_.SetHoveredTocIndex(new_toc_hover)) {
                        renderer_.InvalidateTocPaneCache();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                // We handle cursor in WM_MOUSEMOVE; prevent default override
                return TRUE;
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);

        case WM_LBUTTONDBLCLK:
            OnLButtonDblClk(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEWHEEL: {
            short wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
            bool ctrl = (LOWORD(wParam) & MK_CONTROL) != 0;

            if (ctrl) {
                // Ctrl+wheel zoom — route through controller
                MouseWheelEvent event{wheel_delta, true, PaneZone::MdPane};
                ExecuteActions(controller_.HandleMouseWheel(event));
            } else {
                // Normal scroll — OnMouseWheel detects zone
                POINT pt;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                ScreenToClient(hwnd_, &pt);
                OnMouseWheel(pt.x, pt.y, wheel_delta);
            }
            return 0;
        }

        case WM_CONTEXTMENU:
            OnContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_KEYDOWN:
            OnKeyDown(wParam);
            return 0;

        case WM_DROPFILES:
            OnDropFiles(reinterpret_cast<HDROP>(wParam));
            return 0;

        case WM_DPICHANGED: {
            UINT dpi = HIWORD(wParam);
            auto* suggested = reinterpret_cast<const RECT*>(lParam);
            OnDpiChanged(dpi, suggested);
            return 0;
        }

        case WM_TIMER:
            if (wParam == TIMER_SMOOTH_SCROLL) {
                UpdateSmoothScroll();
            } else if (wParam == TIMER_FILE_WATCH) {
                file_loader_.CheckForChanges();
            } else if (wParam == TIMER_DEFERRED_LAYOUT) {
                OnDeferredLayout();
            } else if (wParam == TIMER_LOADING_ANIM) {
                loading_angle_ += 0.15f;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_APP + 1:  // WM_APP_LOAD_FILE
            DoLoadMarkdownFile();
            return 0;

        case WM_DESTROY:
            SaveLastFilePath();
            KillTimer(hwnd_, TIMER_FILE_WATCH);
            KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
            KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
            KillTimer(hwnd_, TIMER_LOADING_ANIM);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

// ---- Pane Layout ----

PaneLayout MainWindow::GetPaneLayout() const {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return {};

    auto size = rt->GetSize();
    return panes_.ComputeLayout(size.width, size.height,
                                renderer_.GetTheme().splitter_width);
}

PaneZone MainWindow::PaneAtPoint(float dip_x, [[maybe_unused]] float dip_y) const {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return PaneZone::None;
    auto size = rt->GetSize();
    return panes_.DetectZone(dip_x, size.width, size.height,
                             renderer_.GetTheme().splitter_width);
}

float MainWindow::GetMarkdownPaneWidth() const {
    auto layout = GetPaneLayout();
    return layout.md_rect.width;
}

// ---- Paint / Resize ----

void MainWindow::OnPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    auto layout = GetPaneLayout();
    if (!loading_) {
        // Ensure any dirty nodes now visible are laid out at the current width
        float viewport_top = viewport_.GetScrollY();
        float viewport_bottom = viewport_.GetScrollY() + layout.md_rect.height;

        int anchor_idx = FindFirstVisibleNode();
        float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;

        bool updated = renderer_.GetLayout().EnsureVisibleLayout(
            nodes_, layout_cache_, layout.md_rect.width, viewport_top, viewport_bottom);

        if (updated) {
            AnchorCompensateScroll(anchor_idx, anchor_y_before);
        }
    }
    if (loading_) {
        renderer_.DrawLoading(loading_angle_,
                              layout.file_rect, layout.toc_rect, layout.md_rect,
                              file_explorer_.GetEntries(), panes_.FileScroll(), panes_.GetHoveredFileIndex(),
                              toc_.GetEntries(), panes_.TocScroll(), panes_.GetHoveredTocIndex(),
                              panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());
    } else {
        renderer_.Render(nodes_, layout_cache_, viewport_.GetScrollY(), viewport_.GetSelection(),
                         layout.file_rect, layout.toc_rect, layout.md_rect,
                         file_explorer_.GetEntries(), panes_.FileScroll(), panes_.GetHoveredFileIndex(),
                         toc_.GetEntries(), panes_.TocScroll(), panes_.GetHoveredTocIndex(),
                         panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());
    }

    EndPaint(hwnd_, &ps);
}

void MainWindow::OnResize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;

    renderer_.Resize(width, height);

    if (is_sizing_) {
        // During active resize drag: skip expensive layout recomputation
        SyncMaxScroll();
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Viewport-only layout for instant feedback, then batch the rest
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float viewport_top = viewport_.GetScrollY();
    float viewport_bottom = viewport_.GetScrollY() + pane_layout.md_rect.height;

    renderer_.GetLayout().ComputeLayout(nodes_, layout_cache_, md_width, viewport_top, viewport_bottom);

    SyncMaxScroll();
    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (renderer_.GetLayout().HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    RequestMermaidRenders();
}

void MainWindow::OnDpiChanged(UINT dpi, const RECT* suggested) {
    renderer_.SetDpi(static_cast<float>(dpi));

    // Mark all node layouts dirty so they get recreated at new DPI
    for (size_t i = 0; i < nodes_.size(); i++) {
        layout_cache_[i].layout_dirty = true;
        layout_cache_[i].text_layout.Reset();
    }

    SetWindowPos(hwnd_, nullptr,
        suggested->left, suggested->top,
        suggested->right - suggested->left,
        suggested->bottom - suggested->top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    // SetWindowPos triggers WM_SIZE → OnResize → layout recompute + repaint
}

// ---- File loading ----

void MainWindow::LoadMarkdownFile(const std::wstring& path) {
    loading_path_ = path;

    // 128KB以下のファイルはローディングアニメーションをスキップして直接描画
    static constexpr DWORD LOADING_ANIM_THRESHOLD = 128 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)
        && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= LOADING_ANIM_THRESHOLD) {
        DoLoadMarkdownFile();
    } else {
        loading_ = true;
        loading_angle_ = 0.0f;
        SetTimer(hwnd_, TIMER_LOADING_ANIM, 16, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        PostMessage(hwnd_, WM_APP_LOAD_FILE, 0, 0);
    }
}

void MainWindow::DoLoadMarkdownFile() {
    KillTimer(hwnd_, TIMER_LOADING_ANIM);
    loading_ = false;

    const std::wstring& path = loading_path_;
    std::string content = FileLoader::LoadFile(path);
    if (content.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    current_file_ = path;
    viewport_.ClearSelection();

    // Cancel in-flight / queued mermaid renders before replacing nodes_
    mermaid_renderer_.CancelPending();

    nodes_ = ParseMarkdown(content);
    layout_cache_.Reset(nodes_.size());
    toc_.BuildFromNodes(nodes_);

    // Set up file explorer for the directory containing this file
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        std::wstring dir = path.substr(0, pos);
        file_explorer_.SetDirectory(dir);
        file_explorer_.SetCurrentFile(path);
    }

    // Reset pane scroll states and invalidate caches
    panes_.ResetScrollStates();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();

    UpdateLayoutAndScroll(0.0f);
    UpdateTitleBar();

    // Request Mermaid diagram rendering for any mermaid code blocks
    RequestMermaidRenders();

    // Start watching file for changes (callback runs from WM_TIMER on main thread)
    file_loader_.StartWatching(path, [this]() {
        ReloadCurrentFile();
    });
}

void MainWindow::ReloadCurrentFile() {
    if (current_file_.empty()) return;

    float old_scroll = viewport_.GetScrollY();
    mermaid_renderer_.CancelPending();
    nodes_ = ParseMarkdown(FileLoader::LoadFile(current_file_));
    layout_cache_.Reset(nodes_.size());
    toc_.BuildFromNodes(nodes_);
    renderer_.InvalidateTocPaneCache();
    UpdateLayoutAndScroll(old_scroll);
    RequestMermaidRenders();
}

void MainWindow::UpdateTitleBar() {
    int zoom_percent = static_cast<int>(ZOOM_STEPS[viewport_.GetZoomIndex()] * 100.0f + 0.5f);
    SetWindowTextW(hwnd_, BuildTitleString(current_file_, zoom_percent).c_str());
}

void MainWindow::RequestMermaidRenders() {
    if (!mermaid_renderer_.IsReady()) return;

    float viewport_width = GetMarkdownPaneWidth();
    float content_width = viewport_width
                          - renderer_.GetTheme().margin_left
                          - renderer_.GetTheme().margin_right;

    // If the available width changed, clear stale mermaid bitmaps so they re-render
    if (last_mermaid_content_width_ > 0.0f &&
        static_cast<int>(content_width) != static_cast<int>(last_mermaid_content_width_)) {
        for (size_t i = 0; i < nodes_.size(); i++) {
            if (nodes_[i].code_language == SyntaxLanguage::Mermaid) {
                auto& diagram = layout_cache_.GetDiagram(i);
                diagram.bitmap.Reset();
                diagram.width = 0;
                diagram.height = 0;
            }
        }
        mermaid_renderer_.ClearCache();
    }
    last_mermaid_content_width_ = content_width;

    for (size_t i = 0; i < nodes_.size(); i++) {
        auto& node = nodes_[i];
        if (node.type != NodeType::CodeBlock) continue;
        if (node.code_language != SyntaxLanguage::Mermaid) continue;
        auto& diagram = layout_cache_.GetDiagram(i);
        if (diagram.bitmap) continue; // already rendered

        mermaid_renderer_.RequestRender(node, layout_cache_[i], diagram,
                                        content_width, dark_mode_, [this]() {
            // Re-layout after bitmap is available, preserving scroll position
            int anchor_idx = FindFirstVisibleNode();
            float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;
            auto result = RecomputeYPositions(nodes_, layout_cache_, renderer_.GetTheme());
            renderer_.GetLayout().SetTotalHeight(result.total_height);
            SyncMaxScroll();
            AnchorCompensateScroll(anchor_idx, anchor_y_before);
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
    }
}
