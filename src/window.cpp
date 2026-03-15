#include "window.h"
#include "resource.h"
#include "parser.h"
#include "document_utils.h"
#include "pane_layout.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

static constexpr wchar_t WINDOW_CLASS[] = L"MaDViewWindow";

// DWMWA_USE_IMMERSIVE_DARK_MODE (supported on Windows 10 1809+ / Windows 11)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static void ApplyDarkModeToWindow(HWND hwnd, bool dark) {
    // Dark title bar
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // Dark scrollbar via explorer theme
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

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
        drag_scroll_offset_ = dip_y - thumb_y;
    } else {
        drag_scroll_offset_ = info.thumb_height * 0.5f;
        float new_thumb_y = dip_y - drag_scroll_offset_;
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
    float new_thumb_y = dip_y - drag_scroll_offset_;
    float ratio = (track_range > 0) ? (new_thumb_y - info.content_top) / track_range : 0.0f;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    scroll.scroll_y = ratio * info.max_scroll;
    scroll.max_scroll = info.max_scroll;
    cache_dirty = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

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
        L"MaDView",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT,
        2100, 1400,
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
    zoom_index_ = LoadZoomIndex();
    if (zoom_index_ != ZOOM_DEFAULT_INDEX) {
        renderer_.ApplyZoom(ZOOM_STEPS[zoom_index_]);
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
                                               show_file_pane_, show_toc_pane_);

                    // Reset hover states
                    int old_file_hover = hovered_file_index_;
                    int old_toc_hover = hovered_toc_index_;
                    hovered_file_index_ = -1;
                    hovered_toc_index_ = -1;

                    switch (zone) {
                        case PaneZone::Splitter1:
                        case PaneZone::Splitter2:
                            SetCursor(cursor_sizewe_);
                            break;
                        case PaneZone::FilePane: {
                            SetCursor(cursor_arrow_);
                            float content_top = pane_layout.file_rect.y + renderer_.GetTheme().pane_header_height;
                            float local_y = dip_y - content_top + file_scroll_.scroll_y;
                            hovered_file_index_ = file_explorer_.HitTest(local_y, renderer_.GetTheme().pane_item_height);
                            break;
                        }
                        case PaneZone::TocPane: {
                            SetCursor(cursor_arrow_);
                            float content_top = pane_layout.toc_rect.y + renderer_.GetTheme().pane_header_height;
                            float local_y = dip_y - content_top + toc_scroll_.scroll_y;
                            hovered_toc_index_ = toc_.HitTest(local_y, renderer_.GetTheme().pane_item_height);
                            break;
                        }
                        case PaneZone::MdPane: {
                            auto hit = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                            auto link = GetLinkAtHit(hit);
                            SetCursor(link.has_value() ? cursor_hand_ : cursor_ibeam_);
                            break;
                        }
                        default:
                            SetCursor(cursor_arrow_);
                            break;
                    }

                    if (hovered_file_index_ != old_file_hover) {
                        renderer_.InvalidateFilePaneCache();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    if (hovered_toc_index_ != old_toc_hover) {
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
            // Ctrl + Mouse Wheel = Zoom
            if (LOWORD(wParam) & MK_CONTROL) {
                if (wheel_delta > 0) ZoomIn();
                else if (wheel_delta < 0) ZoomOut();
                return 0;
            }
            // Normal scroll
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd_, &pt);
            OnMouseWheel(pt.x, pt.y, wheel_delta);
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
    return ComputePaneLayout(size.width, size.height,
                              pane_file_width_, pane_toc_width_,
                              renderer_.GetTheme().splitter_width,
                              show_file_pane_, show_toc_pane_,
                              MD_PANE_MIN_WIDTH);
}

PaneZone MainWindow::PaneAtPoint(float dip_x, [[maybe_unused]] float dip_y) const {
    auto layout = GetPaneLayout();
    return DetectPaneZone(dip_x, layout, renderer_.GetTheme().splitter_width,
                           show_file_pane_, show_toc_pane_);
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
        float viewport_top = scroll_y_;
        float viewport_bottom = scroll_y_ + layout.md_rect.height;

        int anchor_idx = FindFirstVisibleNode();
        float anchor_y_before = (anchor_idx >= 0) ? nodes_[anchor_idx].y_position : 0.0f;

        bool updated = renderer_.GetLayout().EnsureVisibleLayout(
            nodes_, layout.md_rect.width, viewport_top, viewport_bottom);

        if (updated) {
            AnchorCompensateScroll(anchor_idx, anchor_y_before);
        }
    }
    if (loading_) {
        renderer_.DrawLoading(loading_angle_,
                              layout.file_rect, layout.toc_rect, layout.md_rect,
                              file_explorer_.GetEntries(), file_scroll_, hovered_file_index_,
                              toc_.GetEntries(), toc_scroll_, hovered_toc_index_,
                              show_file_pane_, show_toc_pane_);
    } else {
        renderer_.Render(nodes_, scroll_y_, selection_,
                         layout.file_rect, layout.toc_rect, layout.md_rect,
                         file_explorer_.GetEntries(), file_scroll_, hovered_file_index_,
                         toc_.GetEntries(), toc_scroll_, hovered_toc_index_,
                         show_file_pane_, show_toc_pane_);
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
    float viewport_top = scroll_y_;
    float viewport_bottom = scroll_y_ + pane_layout.md_rect.height;

    renderer_.GetLayout().ComputeLayout(nodes_, md_width, viewport_top, viewport_bottom);

    SyncMaxScroll();
    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (renderer_.GetLayout().HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    RequestMermaidRenders();
}

void MainWindow::OnVScroll(WPARAM wParam) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(hwnd_, SB_VERT, &si);

    float old_pos = scroll_y_;
    auto pane_layout = GetPaneLayout();
    float page_size = pane_layout.md_rect.height;

    switch (LOWORD(wParam)) {
        case SB_LINEUP:    ScrollTo(scroll_y_ - 40.0f); break;
        case SB_LINEDOWN:  ScrollTo(scroll_y_ + 40.0f); break;
        case SB_PAGEUP:    ScrollTo(scroll_y_ - page_size); break;
        case SB_PAGEDOWN:  ScrollTo(scroll_y_ + page_size); break;
        case SB_THUMBTRACK:
            is_scrollbar_tracking_ = true;
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_THUMBPOSITION:
            is_scrollbar_tracking_ = false;
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_ENDSCROLL:
            is_scrollbar_tracking_ = false;
            break;
        case SB_TOP:       ScrollTo(0.0f); break;
        case SB_BOTTOM:    ScrollTo(max_scroll_); break;
    }

    if (scroll_y_ != old_pos) {
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnMouseWheel(int px, int py, short delta) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip.x, pane_layout,
                                renderer_.GetTheme().splitter_width,
                                show_file_pane_, show_toc_pane_);
    float scroll_amount = -delta * 0.5f;
    const auto& theme = renderer_.GetTheme();

    switch (zone) {
        case PaneZone::FilePane: {
            float max_file_scroll = std::max(0.0f,
                static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height
                - (pane_layout.file_rect.height - theme.pane_header_height));
            file_scroll_.scroll_y = std::clamp(file_scroll_.scroll_y + scroll_amount, 0.0f, max_file_scroll);
            file_scroll_.max_scroll = max_file_scroll;
            renderer_.InvalidateFilePaneCache();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case PaneZone::TocPane: {
            float max_toc_scroll = std::max(0.0f,
                static_cast<float>(toc_.GetEntries().size()) * theme.pane_item_height
                - (pane_layout.toc_rect.height - theme.pane_header_height));
            toc_scroll_.scroll_y = std::clamp(toc_scroll_.scroll_y + scroll_amount, 0.0f, max_toc_scroll);
            toc_scroll_.max_scroll = max_toc_scroll;
            renderer_.InvalidateTocPaneCache();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        default:
            // MD pane or anywhere else
            SmoothScrollBy(scroll_amount);
            break;
    }
}

void MainWindow::OnKeyDown(WPARAM key) {
    auto pane_layout = GetPaneLayout();
    float page_size = pane_layout.md_rect.height;

    switch (key) {
        case VK_UP:    SmoothScrollBy(-40.0f); break;
        case VK_DOWN:  SmoothScrollBy(40.0f); break;
        case VK_PRIOR: SmoothScrollBy(-page_size * 0.9f); break;  // Page Up
        case VK_NEXT:  SmoothScrollBy(page_size * 0.9f); break;   // Page Down
        case VK_HOME:  SmoothScrollBy(-scroll_y_); break;
        case VK_END:   SmoothScrollBy(max_scroll_ - scroll_y_); break;
        case VK_F5:    ReloadCurrentFile(); break;
        case 'C':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                CopySelectionToClipboard();
            }
            break;
        case 'A':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                SelectAll();
            }
            break;
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                auto path = FileLoader::OpenFileDialog(hwnd_);
                if (!path.empty()) LoadMarkdownFile(path);
            }
            break;
        case '1':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                show_file_pane_ = !show_file_pane_;
                // Trigger resize to recalculate layout
                RECT rc;
                GetClientRect(hwnd_, &rc);
                OnResize(static_cast<UINT>(rc.right - rc.left),
                         static_cast<UINT>(rc.bottom - rc.top));
            }
            break;
        case '2':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                show_toc_pane_ = !show_toc_pane_;
                RECT rc;
                GetClientRect(hwnd_, &rc);
                OnResize(static_cast<UINT>(rc.right - rc.left),
                         static_cast<UINT>(rc.bottom - rc.top));
            }
            break;
        case VK_ESCAPE:
            ClearSelection();
            break;

        // Zoom: Ctrl+Plus / Ctrl+Minus / Ctrl+0
        case VK_OEM_PLUS:   // =/+ key
        case VK_ADD:        // Numpad +
            if (GetKeyState(VK_CONTROL) & 0x8000) ZoomIn();
            break;
        case VK_OEM_MINUS:  // -/_ key
        case VK_SUBTRACT:   // Numpad -
            if (GetKeyState(VK_CONTROL) & 0x8000) ZoomOut();
            break;
        case '0':
        case VK_NUMPAD0:
            if (GetKeyState(VK_CONTROL) & 0x8000) ZoomReset();
            break;
    }
}

void MainWindow::OnContextMenu(int screen_x, int screen_y) {
    POINT pt = {screen_x, screen_y};
    POINT client_pt = pt;
    ScreenToClient(hwnd_, &client_pt);
    auto dip = PixelToDip(client_pt.x, client_pt.y);
    auto zone = PaneAtPoint(dip.x, dip.y);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (zone == PaneZone::MdPane) {
        // "Edit file" item - enabled only when a file is loaded
        bool has_file = !current_file_.empty();
        AppendMenuW(menu, MF_STRING | (has_file ? 0 : MF_GRAYED), IDM_EDIT_FILE, L"エディタで開く(&E)");

        // "Copy" item - enabled only when text is selected
        bool has_selection = selection_.active && selection_.start_node >= 0;
        AppendMenuW(menu, MF_STRING | (has_selection ? 0 : MF_GRAYED), IDM_COPY, L"コピー(&C)");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING | (dark_mode_ ? MF_CHECKED : 0),
                IDM_TOGGLE_DARK_MODE, L"ダークモード(&D)");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == IDM_EDIT_FILE) {
        ShellExecuteW(hwnd_, L"open", current_file_.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == IDM_COPY) {
        CopySelectionToClipboard();
    } else if (cmd == IDM_TOGGLE_DARK_MODE) {
        ToggleDarkMode();
    }
}

void MainWindow::OnDropFiles(HDROP hDrop) {
    wchar_t path[MAX_PATH];
    if (DragQueryFileW(hDrop, 0, path, MAX_PATH)) {
        LoadMarkdownFile(path);
    }
    DragFinish(hDrop);
}

void MainWindow::OnDpiChanged(UINT dpi, const RECT* suggested) {
    renderer_.SetDpi(static_cast<float>(dpi));

    // Mark all node layouts dirty so they get recreated at new DPI
    for (auto& node : nodes_) {
        node.layout_dirty = true;
        node.text_layout.Reset();
    }

    SetWindowPos(hwnd_, nullptr,
        suggested->left, suggested->top,
        suggested->right - suggested->left,
        suggested->bottom - suggested->top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    // SetWindowPos triggers WM_SIZE → OnResize → layout recompute + repaint
}

void MainWindow::UpdateScrollBar() {
    auto pane_layout = GetPaneLayout();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    si.nMin = 0;
    si.nMax = static_cast<int>(renderer_.GetLayout().GetTotalHeight());
    si.nPage = static_cast<UINT>(pane_layout.md_rect.height);
    si.nPos = static_cast<int>(scroll_y_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void MainWindow::ScrollTo(float position) {
    scroll_y_ = std::clamp(position, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;
}

void MainWindow::SmoothScrollBy(float delta) {
    scroll_target_ = std::clamp(scroll_target_ + delta, 0.0f, max_scroll_);

    if (!smooth_scrolling_) {
        smooth_scrolling_ = true;
        SetTimer(hwnd_, TIMER_SMOOTH_SCROLL, 16, nullptr);  // ~60fps
    }
}

void MainWindow::UpdateSmoothScroll() {
    float diff = scroll_target_ - scroll_y_;

    if (std::abs(diff) < SCROLL_EPSILON) {
        scroll_y_ = scroll_target_;
        smooth_scrolling_ = false;
        KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
    } else {
        scroll_y_ += diff * SCROLL_SPEED;
    }

    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    UpdateScrollBar();
    InvalidateMdPane();
}

void MainWindow::InvalidateMdPane() {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;
    auto layout = GetPaneLayout();
    RECT rc;
    rc.left = static_cast<LONG>(layout.md_rect.x * scale);
    rc.top = 0;
    rc.right = static_cast<LONG>((layout.md_rect.x + layout.md_rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>(layout.md_rect.height * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void MainWindow::StopSmoothScroll() {
    if (!smooth_scrolling_) return;
    scroll_y_ = scroll_target_;
    smooth_scrolling_ = false;
    KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
}

void MainWindow::SyncMaxScroll() {
    auto pane_layout = GetPaneLayout();
    float total = renderer_.GetLayout().GetTotalHeight();
    max_scroll_ = std::max(0.0f, total - pane_layout.md_rect.height);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;
}

int MainWindow::FindFirstVisibleNode() const {
    int lo = 0, hi = static_cast<int>(nodes_.size());
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (nodes_[mid].y_position + nodes_[mid].height <= scroll_y_)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < static_cast<int>(nodes_.size()) ? lo : -1;
}

void MainWindow::AnchorCompensateScroll(int anchor_idx, float anchor_y_before) {
    if (anchor_idx < 0) return;
    float shift = nodes_[anchor_idx].y_position - anchor_y_before;
    scroll_y_ += shift;
    scroll_target_ += shift;
    SyncMaxScroll();
}

void MainWindow::OnResizeEnd() {
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float viewport_top = scroll_y_;
    float viewport_bottom = scroll_y_ + pane_layout.md_rect.height;

    renderer_.GetLayout().ComputeLayout(nodes_, md_width, viewport_top, viewport_bottom);

    SyncMaxScroll();
    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (renderer_.GetLayout().HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    RequestMermaidRenders();
}

void MainWindow::OnDeferredLayout() {
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? nodes_[anchor_idx].y_position : 0.0f;

    float md_width = GetMarkdownPaneWidth();
    bool more = renderer_.GetLayout().ProcessDirtyBatch(nodes_, md_width, 200);

    // Compensate scroll to keep visible content at the same screen position,
    // but skip during active scrollbar drag to avoid fighting with user input
    if (!is_scrollbar_tracking_) {
        AnchorCompensateScroll(anchor_idx, anchor_y_before);
    } else {
        SyncMaxScroll();
    }

    if (!more) {
        // Only repaint on the final batch; intermediate batches only affect
        // off-screen nodes, so repainting would just cause sub-pixel jitter.
        KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::UpdateLayoutAndScroll(float desired_scroll) {
    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().ComputeLayout(nodes_, md_width);

    scroll_y_ = desired_scroll;
    scroll_target_ = desired_scroll;
    SyncMaxScroll();

    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

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
    ClearSelection();

    nodes_ = ParseMarkdown(content);
    toc_.BuildFromNodes(nodes_);

    // Set up file explorer for the directory containing this file
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        std::wstring dir = path.substr(0, pos);
        file_explorer_.SetDirectory(dir);
        file_explorer_.SetCurrentFile(path);
    }

    // Reset pane scroll states and invalidate caches
    file_scroll_ = {};
    toc_scroll_ = {};
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

    float old_scroll = scroll_y_;
    nodes_ = ParseMarkdown(FileLoader::LoadFile(current_file_));
    toc_.BuildFromNodes(nodes_);
    renderer_.InvalidateTocPaneCache();
    UpdateLayoutAndScroll(old_scroll);
    RequestMermaidRenders();
}

void MainWindow::UpdateTitleBar() {
    int zoom_percent = static_cast<int>(ZOOM_STEPS[zoom_index_] * 100.0f + 0.5f);
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
        for (auto& node : nodes_) {
            if (node.code_language == SyntaxLanguage::Mermaid) {
                node.diagram_bitmap.Reset();
                node.diagram_width = 0;
                node.diagram_height = 0;
            }
        }
        mermaid_renderer_.ClearCache();
    }
    last_mermaid_content_width_ = content_width;

    for (auto& node : nodes_) {
        if (node.type != NodeType::CodeBlock) continue;
        if (node.code_language != SyntaxLanguage::Mermaid) continue;
        if (node.diagram_bitmap) continue; // already rendered

        mermaid_renderer_.RequestRender(node, content_width, dark_mode_, [this]() {
            // Re-layout after bitmap is available, preserving scroll position
            int anchor_idx = FindFirstVisibleNode();
            float anchor_y_before = (anchor_idx >= 0) ? nodes_[anchor_idx].y_position : 0.0f;
            auto result = RecomputeYPositions(nodes_, renderer_.GetTheme());
            renderer_.GetLayout().SetTotalHeight(result.total_height);
            SyncMaxScroll();
            AnchorCompensateScroll(anchor_idx, anchor_y_before);
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
    }
}

// ---- Dark mode persistence ----

static std::filesystem::path GetDarkModeConfigPath() {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return {};
    }
    std::filesystem::path dir = std::filesystem::path(appdata) / L"MaDView";
    CoTaskMemFree(appdata);
    return dir / L"dark_mode.txt";
}

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
    auto config_path = GetDarkModeConfigPath();
    if (config_path.empty()) return;

    std::filesystem::create_directories(config_path.parent_path());
    std::ofstream ofs(config_path);
    if (ofs) {
        ofs << (dark_mode_ ? "1" : "0");
    }
}

bool MainWindow::LoadDarkMode() {
    auto config_path = GetDarkModeConfigPath();
    if (config_path.empty()) return false;

    std::ifstream ifs(config_path);
    if (!ifs) return false;

    char c = '0';
    ifs >> c;
    return c == '1';
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

    // Update theme sizes and recreate DirectWrite formats
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
        float zoom_ratio = new_zoom / old_zoom;
        scroll_y_ = anchor_y_after + anchor_offset * zoom_ratio;
    }
    SyncMaxScroll();
    scroll_target_ = scroll_y_;

    UpdateScrollBar();
    UpdateTitleBar();
    SaveZoomLevel();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

static std::filesystem::path GetZoomConfigPath() {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return {};
    }
    std::filesystem::path dir = std::filesystem::path(appdata) / L"MaDView";
    CoTaskMemFree(appdata);
    return dir / L"zoom_level.txt";
}

void MainWindow::SaveZoomLevel() const {
    auto config_path = GetZoomConfigPath();
    if (config_path.empty()) return;

    std::filesystem::create_directories(config_path.parent_path());
    std::ofstream ofs(config_path);
    if (ofs) {
        ofs << zoom_index_;
    }
}

int MainWindow::LoadZoomIndex() {
    auto config_path = GetZoomConfigPath();
    if (config_path.empty()) return ZOOM_DEFAULT_INDEX;

    std::ifstream ifs(config_path);
    if (!ifs) return ZOOM_DEFAULT_INDEX;

    int index = ZOOM_DEFAULT_INDEX;
    ifs >> index;
    if (index < 0 || index >= ZOOM_STEP_COUNT) return ZOOM_DEFAULT_INDEX;
    return index;
}

// ---- Last file persistence ----

static std::filesystem::path GetLastFileConfigPath() {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) {
        return {};
    }
    std::filesystem::path dir = std::filesystem::path(appdata) / L"MaDView";
    CoTaskMemFree(appdata);
    return dir / L"last_file.txt";
}

void MainWindow::SaveLastFilePath() const {
    if (current_file_.empty()) return;

    auto config_path = GetLastFileConfigPath();
    if (config_path.empty()) return;

    std::filesystem::create_directories(config_path.parent_path());
    std::ofstream ofs(config_path, std::ios::binary);
    if (ofs) {
        // Write as UTF-16LE (wstring direct)
        ofs.write(reinterpret_cast<const char*>(current_file_.data()),
                  static_cast<std::streamsize>(current_file_.size() * sizeof(wchar_t)));
    }
}

std::wstring MainWindow::LoadLastFilePath() {
    auto config_path = GetLastFileConfigPath();
    if (config_path.empty()) return {};

    std::ifstream ifs(config_path, std::ios::binary | std::ios::ate);
    if (!ifs) return {};

    auto size = ifs.tellg();
    if (size <= 0 || size % sizeof(wchar_t) != 0) return {};
    ifs.seekg(0);

    std::wstring path(static_cast<size_t>(size) / sizeof(wchar_t), L'\0');
    ifs.read(reinterpret_cast<char*>(path.data()), size);

    // Verify the file still exists
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return path;
}

// ---- Selection / Hit Testing ----

MainWindow::HitResult MainWindow::HitTest(int screen_x, int screen_y) const {
    HitResult result;
    if (nodes_.empty()) return result;

    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return result;

    const auto& theme = renderer_.GetTheme();
    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;

    // Convert physical pixels to DIPs
    float dip_x = screen_x / scale;
    float dip_y = screen_y / scale + scroll_y_;

    // Offset by MD pane position
    auto pane_layout = GetPaneLayout();
    float md_left = pane_layout.md_rect.x;
    dip_x -= md_left;

    // Binary search for the node containing dip_y
    int lo = 0, hi = static_cast<int>(nodes_.size()) - 1;
    int candidate = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (nodes_[mid].y_position <= dip_y) {
            candidate = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (candidate >= 0 && dip_y <= nodes_[candidate].y_position + nodes_[candidate].height) {
        const auto& node = nodes_[candidate];

        if (node.type == NodeType::Table) {
            return HitTestTable(node, candidate, dip_x, dip_y);
        }

        if (node.text_layout) {
            float indent = node.indent_level * theme.indent_width;
            float local_x = dip_x - theme.margin_left - indent;
            float local_y = dip_y - node.y_position;

            BOOL is_trailing = FALSE;
            BOOL is_inside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            node.text_layout->HitTestPoint(local_x, local_y,
                                           &is_trailing, &is_inside, &metrics);

            result.node_index = candidate;
            result.text_pos = metrics.textPosition + (is_trailing ? 1 : 0);
            return result;
        }
    }

    // Click below all nodes → select end of last node
    for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; i--) {
        if (!nodes_[i].text.empty()) {
            result.node_index = i;
            result.text_pos = static_cast<uint32_t>(nodes_[i].text.size());
            return result;
        }
    }
    return result;
}

MainWindow::HitResult MainWindow::HitTestTable(const RenderNode& node, int node_index,
                                                float dip_x, float dip_y) const {
    HitResult result;
    result.node_index = node_index;

    const auto& theme = renderer_.GetTheme();
    float indent = node.indent_level * theme.indent_width;
    float base_x = theme.margin_left + indent;
    float cell_padding = 8.0f;
    float border = 1.0f;

    // Find which row was clicked
    float ry = node.y_position;
    int hit_row = -1;
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        float row_bottom = ry + node.table_rows[r].row_height + border;
        if (dip_y < row_bottom) {
            hit_row = static_cast<int>(r);
            break;
        }
        ry += node.table_rows[r].row_height + border;
    }
    if (hit_row < 0) {
        result.text_pos = static_cast<uint32_t>(node.text.size());
        return result;
    }

    // Find which column was clicked
    float cx = base_x + border;
    int hit_col = static_cast<int>(node.col_widths.size()) - 1; // default to last
    for (size_t c = 0; c < node.col_widths.size(); c++) {
        float col_right = cx + node.col_widths[c] + cell_padding * 2.0f;
        if (dip_x < col_right) {
            hit_col = static_cast<int>(c);
            break;
        }
        cx += node.col_widths[c] + cell_padding * 2.0f + border;
    }
    if (hit_col < 0) hit_col = 0;

    // Compute flat text offset for cell (hit_row, hit_col)
    uint32_t flat_offset = 0;
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        const auto& row_cells = node.table_rows[r].cells;
        for (size_t c = 0; c < row_cells.size(); c++) {
            if (static_cast<int>(r) == hit_row && static_cast<int>(c) == hit_col) {
                // Hit test within the cell's text layout
                const auto& cell = row_cells[c];
                if (cell.text_layout) {
                    // Compute cell text position
                    float cell_x = base_x + border;
                    for (size_t cc = 0; cc < c; cc++) {
                        cell_x += node.col_widths[cc] + cell_padding * 2.0f + border;
                    }
                    float cell_text_x = cell_x + cell_padding;

                    float cell_y = node.y_position;
                    for (size_t rr = 0; rr < r; rr++) {
                        cell_y += node.table_rows[rr].row_height + border;
                    }
                    float cell_text_y = cell_y + cell_padding;

                    BOOL is_trailing = FALSE, is_inside = FALSE;
                    DWRITE_HIT_TEST_METRICS metrics{};
                    cell.text_layout->HitTestPoint(
                        dip_x - cell_text_x, dip_y - cell_text_y,
                        &is_trailing, &is_inside, &metrics);

                    result.text_pos = flat_offset + metrics.textPosition + (is_trailing ? 1 : 0);
                } else {
                    result.text_pos = flat_offset;
                }
                return result;
            }
            flat_offset += static_cast<uint32_t>(row_cells[c].text.size());
            if (c + 1 < row_cells.size()) flat_offset++; // tab
        }
        if (r + 1 < node.table_rows.size()) flat_offset++; // newline
    }

    result.text_pos = static_cast<uint32_t>(node.text.size());
    return result;
}

void MainWindow::OnLButtonDown(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    float dip_x = dip.x;
    float dip_y = dip.y;

    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip_x, pane_layout,
                                renderer_.GetTheme().splitter_width,
                                show_file_pane_, show_toc_pane_);

    switch (zone) {
        case PaneZone::Splitter1:
            SetCapture(hwnd_);
            drag_target_ = DragTarget::Splitter1;
            return;
        case PaneZone::Splitter2:
            SetCapture(hwnd_);
            drag_target_ = DragTarget::Splitter2;
            return;
        case PaneZone::FilePane: {
            const auto& theme = renderer_.GetTheme();
            float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height;
            auto scroll_info = ComputePaneScrollInfo(pane_layout.file_rect, total_content);
            float local_x = dip_x - pane_layout.file_rect.x;

            // Check if click is on scrollbar area
            if (local_x >= pane_layout.file_rect.width - PANE_SCROLLBAR_WIDTH - 4.0f
                && total_content > scroll_info.content_height) {
                SetCapture(hwnd_);
                drag_target_ = DragTarget::FileScrollbar;
                bool dirty = false;
                HandleScrollbarClick(dip_y, scroll_info, file_scroll_, dirty);
                if (dirty) renderer_.InvalidateFilePaneCache();
                return;
            }

            float local_y = dip_y - scroll_info.content_top + file_scroll_.scroll_y;
            int idx = file_explorer_.HitTest(local_y, theme.pane_item_height);
            if (idx >= 0 && idx < static_cast<int>(file_explorer_.GetEntries().size())) {
                const auto& entry = file_explorer_.GetEntries()[idx];
                if (entry.is_directory) {
                    file_explorer_.SetDirectory(entry.full_path);
                    if (!current_file_.empty()) {
                        file_explorer_.SetCurrentFile(current_file_);
                    }
                    file_scroll_ = {};
                    renderer_.InvalidateFilePaneCache();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else if (!entry.is_current) {
                    LoadMarkdownFile(entry.full_path);
                }
            }
            return;
        }
        case PaneZone::TocPane: {
            const auto& theme = renderer_.GetTheme();
            float total_content = static_cast<float>(toc_.GetEntries().size()) * theme.pane_item_height;
            auto scroll_info = ComputePaneScrollInfo(pane_layout.toc_rect, total_content);
            float local_x = dip_x - pane_layout.toc_rect.x;

            // Check if click is on scrollbar area
            if (local_x >= pane_layout.toc_rect.width - PANE_SCROLLBAR_WIDTH - 4.0f
                && total_content > scroll_info.content_height) {
                SetCapture(hwnd_);
                drag_target_ = DragTarget::TocScrollbar;
                bool dirty = false;
                HandleScrollbarClick(dip_y, scroll_info, toc_scroll_, dirty);
                if (dirty) renderer_.InvalidateTocPaneCache();
                return;
            }

            float local_y = dip_y - scroll_info.content_top + toc_scroll_.scroll_y;
            int idx = toc_.HitTest(local_y, theme.pane_item_height);
            if (idx >= 0 && idx < static_cast<int>(toc_.GetEntries().size())) {
                NavigateToAnchor(toc_.GetEntries()[idx].anchor_id);
            }
            return;
        }
        case PaneZone::MdPane:
            break;
        default:
            return;
    }

    // MD pane: existing selection logic
    SetCapture(hwnd_);
    click_start_x_ = px;
    click_start_y_ = py;
    auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        anchor_node_ = hit.node_index;
        anchor_pos_ = hit.text_pos;
        is_dragging_ = true;
        selection_.Clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnLButtonUp(int px, int py) {
    ReleaseCapture();

    if (drag_target_ != DragTarget::None) {
        drag_target_ = DragTarget::None;
        // Recalculate layout after splitter drag
        RECT rc;
        GetClientRect(hwnd_, &rc);
        OnResize(static_cast<UINT>(rc.right - rc.left),
                 static_cast<UINT>(rc.bottom - rc.top));
        return;
    }

    if (is_dragging_) {
        auto hit = HitTest(px, py);
        if (hit.node_index >= 0) {
            selection_ = TextSelection::MakeOrdered(
                anchor_node_, anchor_pos_, hit.node_index, hit.text_pos);
        }
        is_dragging_ = false;

        // If it was a click (not a drag), check for link
        int dx = px - click_start_x_;
        int dy = py - click_start_y_;
        if (!selection_.active && (dx * dx + dy * dy) < 25) {
            auto link = GetLinkAtHit(hit);
            if (link.has_value()) {
                HandleLinkClick(link.value());
            }
        }

        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnMouseMove(int px, int py) {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return;

    auto dip = PixelToDip(px, py);
    float dip_x = dip.x;

    // Handle splitter dragging
    if (drag_target_ == DragTarget::Splitter1) {
        float new_width = dip_x;
        pane_file_width_ = std::clamp(new_width, PANE_MIN_WIDTH, dip_x);

        // Ensure MD pane doesn't get too small
        auto size = rt->GetSize();
        float used = pane_file_width_ + renderer_.GetTheme().splitter_width;
        if (show_toc_pane_) used += pane_toc_width_ + renderer_.GetTheme().splitter_width;
        if (size.width - used < MD_PANE_MIN_WIDTH) {
            pane_file_width_ = size.width - MD_PANE_MIN_WIDTH - renderer_.GetTheme().splitter_width;
            if (show_toc_pane_) pane_file_width_ -= pane_toc_width_ + renderer_.GetTheme().splitter_width;
            pane_file_width_ = std::max(PANE_MIN_WIDTH, pane_file_width_);
        }

        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (drag_target_ == DragTarget::FileScrollbar) {
        auto layout = GetPaneLayout();
        float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        auto info = ComputePaneScrollInfo(layout.file_rect, total_content);
        bool dirty = false;
        HandleScrollbarDrag(dip.y, info, file_scroll_, dirty);
        if (dirty) renderer_.InvalidateFilePaneCache();
        return;
    }

    if (drag_target_ == DragTarget::TocScrollbar) {
        auto layout = GetPaneLayout();
        float total_content = static_cast<float>(toc_.GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        auto info = ComputePaneScrollInfo(layout.toc_rect, total_content);
        bool dirty = false;
        HandleScrollbarDrag(dip.y, info, toc_scroll_, dirty);
        if (dirty) renderer_.InvalidateTocPaneCache();
        return;
    }

    if (drag_target_ == DragTarget::Splitter2) {
        auto pane_layout = GetPaneLayout();
        float toc_left = pane_layout.toc_rect.x;
        float new_width = dip_x - toc_left;
        pane_toc_width_ = std::clamp(new_width, PANE_MIN_WIDTH, new_width);

        // Ensure MD pane doesn't get too small
        auto size = rt->GetSize();
        float used = renderer_.GetTheme().splitter_width;
        if (show_file_pane_) used += pane_file_width_ + renderer_.GetTheme().splitter_width;
        used += pane_toc_width_;
        if (size.width - used < MD_PANE_MIN_WIDTH) {
            pane_toc_width_ = size.width - MD_PANE_MIN_WIDTH - renderer_.GetTheme().splitter_width;
            if (show_file_pane_) pane_toc_width_ -= pane_file_width_ + renderer_.GetTheme().splitter_width;
            pane_toc_width_ = std::max(PANE_MIN_WIDTH, pane_toc_width_);
        }

        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // MD pane: existing drag selection logic
    if (!is_dragging_) return;
    auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        selection_ = TextSelection::MakeOrdered(
            anchor_node_, anchor_pos_, hit.node_index, hit.text_pos);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnLButtonDblClk(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) return;

    auto hit = HitTest(px, py);
    if (hit.node_index < 0) return;

    const auto& text = nodes_[hit.node_index].text;
    if (text.empty()) return;

    auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) return;

    anchor_node_ = hit.node_index;
    anchor_pos_ = wb.start;
    selection_ = TextSelection::MakeOrdered(
        hit.node_index, wb.start, hit.node_index, wb.end);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ClearSelection() {
    selection_.Clear();
    anchor_node_ = -1;
    is_dragging_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SelectAll() {
    if (nodes_.empty()) return;

    int last = static_cast<int>(nodes_.size()) - 1;
    selection_ = TextSelection::MakeOrdered(
        0, 0, last, static_cast<uint32_t>(nodes_[last].text.size()));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CopySelectionToClipboard() const {
    if (!selection_.active) return;

    std::wstring result = ExtractSelectedText(nodes_, selection_);
    if (result.empty()) return;

    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();

    size_t bytes = (result.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* ptr = GlobalLock(hMem);
        if (ptr) {
            memcpy(ptr, result.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
}

std::optional<std::wstring> MainWindow::GetLinkAtHit(const HitResult& hit) const {
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(nodes_.size()))
        return std::nullopt;

    return FindLinkAtPosition(nodes_[hit.node_index], hit.text_pos);
}

void MainWindow::HandleLinkClick(const std::wstring& url) {
    if (url.empty()) return;

    // Internal anchor link: #something
    if (url[0] == L'#') {
        std::wstring anchor = url.substr(1);
        NavigateToAnchor(anchor);
        return;
    }

    // External link: open in default browser
    ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::NavigateToAnchor(const std::wstring& anchor) {
    int idx = FindAnchorNodeIndex(nodes_, anchor);
    if (idx < 0) return;

    float target_y = nodes_[idx].y_position - renderer_.GetTheme().heading_spacing_above;
    target_y = std::max(0.0f, target_y);
    ScrollTo(target_y);
    UpdateScrollBar();
    InvalidateMdPane();
}
