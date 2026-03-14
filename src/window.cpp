#include "window.h"
#include "parser.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <shellscalingapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")

static constexpr wchar_t WINDOW_CLASS[] = L"MaDViewWindow";

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
        1200, 700,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) return false;

    if (!renderer_.Init(hwnd_)) return false;

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
                auto* rt = renderer_.GetRenderTarget();
                if (rt) {
                    float dpi_x, dpi_y;
                    rt->GetDpi(&dpi_x, &dpi_y);
                    float scale = dpi_x / 96.0f;
                    float dip_x = GET_X_LPARAM(lParam) / scale;
                    float dip_y = GET_Y_LPARAM(lParam) / scale;

                    auto zone = PaneAtPoint(dip_x, dip_y);

                    // Reset hover states
                    int old_file_hover = hovered_file_index_;
                    int old_toc_hover = hovered_toc_index_;
                    hovered_file_index_ = -1;
                    hovered_toc_index_ = -1;

                    switch (zone) {
                        case PaneZone::Splitter1:
                        case PaneZone::Splitter2:
                            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                            break;
                        case PaneZone::FilePane: {
                            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                            auto layout = GetPaneLayout();
                            float content_top = layout.file_rect.y + renderer_.GetTheme().pane_header_height;
                            float local_y = dip_y - content_top + file_scroll_.scroll_y;
                            hovered_file_index_ = file_explorer_.HitTest(local_y, renderer_.GetTheme().pane_item_height);
                            break;
                        }
                        case PaneZone::TocPane: {
                            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                            auto layout = GetPaneLayout();
                            float content_top = layout.toc_rect.y + renderer_.GetTheme().pane_header_height;
                            float local_y = dip_y - content_top + toc_scroll_.scroll_y;
                            hovered_toc_index_ = toc_.HitTest(local_y, renderer_.GetTheme().pane_item_height);
                            break;
                        }
                        case PaneZone::MdPane: {
                            auto hit = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                            auto link = GetLinkAtHit(hit);
                            SetCursor(LoadCursorW(nullptr, link.has_value() ? IDC_HAND : IDC_IBEAM));
                            break;
                        }
                        default:
                            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
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
            // Get cursor position in client coordinates for pane detection
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd_, &pt);
            OnMouseWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }

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
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd_, TIMER_FILE_WATCH);
            KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
            KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

// ---- Pane Layout ----

MainWindow::PaneLayout MainWindow::GetPaneLayout() const {
    PaneLayout layout{};
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return layout;

    auto size = rt->GetSize();
    float total_width = size.width;
    float total_height = size.height;
    const auto& theme = renderer_.GetTheme();
    float splitter_w = theme.splitter_width;

    float x = 0.0f;

    // File pane
    if (show_file_pane_) {
        layout.file_rect = {x, 0.0f, pane_file_width_, total_height};
        x += pane_file_width_ + splitter_w;
    }

    // TOC pane
    if (show_toc_pane_) {
        layout.toc_rect = {x, 0.0f, pane_toc_width_, total_height};
        x += pane_toc_width_ + splitter_w;
    }

    // MD pane takes the rest
    float md_width = std::max(MD_PANE_MIN_WIDTH, total_width - x);
    layout.md_rect = {x, 0.0f, md_width, total_height};

    return layout;
}

MainWindow::PaneZone MainWindow::PaneAtPoint(float dip_x, [[maybe_unused]] float dip_y) const {
    const auto& theme = renderer_.GetTheme();
    float splitter_w = theme.splitter_width;
    auto layout = GetPaneLayout();

    if (show_file_pane_) {
        float s1_x = layout.file_rect.x + layout.file_rect.width;
        if (dip_x >= layout.file_rect.x && dip_x < s1_x) {
            return PaneZone::FilePane;
        }
        if (dip_x >= s1_x && dip_x < s1_x + splitter_w) {
            return PaneZone::Splitter1;
        }
    }

    if (show_toc_pane_) {
        float s2_x = layout.toc_rect.x + layout.toc_rect.width;
        if (dip_x >= layout.toc_rect.x && dip_x < s2_x) {
            return PaneZone::TocPane;
        }
        if (dip_x >= s2_x && dip_x < s2_x + splitter_w) {
            return PaneZone::Splitter2;
        }
    }

    if (dip_x >= layout.md_rect.x) {
        return PaneZone::MdPane;
    }

    return PaneZone::None;
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
    renderer_.Render(nodes_, scroll_y_, selection_,
                     layout.file_rect, layout.toc_rect, layout.md_rect,
                     file_explorer_.GetEntries(), file_scroll_, hovered_file_index_,
                     toc_.GetEntries(), toc_scroll_, hovered_toc_index_,
                     show_file_pane_, show_toc_pane_);

    EndPaint(hwnd_, &ps);
}

void MainWindow::OnResize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;

    renderer_.Resize(width, height);

    float md_width = GetMarkdownPaneWidth();

    if (is_sizing_) {
        // During active resize drag: skip expensive layout recomputation
        float total = renderer_.GetLayout().GetTotalHeight();
        auto layout = GetPaneLayout();
        max_scroll_ = std::max(0.0f, total - layout.md_rect.height);
        scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
        scroll_target_ = scroll_y_;
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Viewport-only layout for instant feedback, then batch the rest
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float viewport_top = scroll_y_;
    float viewport_bottom = scroll_y_ + pane_layout.md_rect.height;

    renderer_.GetLayout().ComputeLayout(nodes_, md_width, viewport_top, viewport_bottom);

    float total = renderer_.GetLayout().GetTotalHeight();
    max_scroll_ = std::max(0.0f, total - pane_layout.md_rect.height);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;

    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (renderer_.GetLayout().HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }
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
            ScrollTo(static_cast<float>(si.nTrackPos));
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
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return;

    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;
    float dip_x = px / scale;
    float dip_y = py / scale;

    auto zone = PaneAtPoint(dip_x, dip_y);
    float scroll_amount = -delta * 0.5f;
    const auto& theme = renderer_.GetTheme();

    switch (zone) {
        case PaneZone::FilePane: {
            float max_file_scroll = std::max(0.0f,
                static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height
                - (GetPaneLayout().file_rect.height - theme.pane_header_height));
            file_scroll_.scroll_y = std::clamp(file_scroll_.scroll_y + scroll_amount, 0.0f, max_file_scroll);
            file_scroll_.max_scroll = max_file_scroll;
            renderer_.InvalidateFilePaneCache();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case PaneZone::TocPane: {
            float max_toc_scroll = std::max(0.0f,
                static_cast<float>(toc_.GetEntries().size()) * theme.pane_item_height
                - (GetPaneLayout().toc_rect.height - theme.pane_header_height));
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
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnResizeEnd() {
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    float md_width = GetMarkdownPaneWidth();
    auto pane_layout = GetPaneLayout();
    float viewport_top = scroll_y_;
    float viewport_bottom = scroll_y_ + pane_layout.md_rect.height;

    renderer_.GetLayout().ComputeLayout(nodes_, md_width, viewport_top, viewport_bottom);

    float total = renderer_.GetLayout().GetTotalHeight();
    max_scroll_ = std::max(0.0f, total - pane_layout.md_rect.height);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;

    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);

    // Start incremental processing of remaining dirty nodes
    if (renderer_.GetLayout().HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }
}

void MainWindow::OnDeferredLayout() {
    float md_width = GetMarkdownPaneWidth();
    bool more = renderer_.GetLayout().ProcessDirtyBatch(nodes_, md_width, 200);

    auto pane_layout = GetPaneLayout();
    float total = renderer_.GetLayout().GetTotalHeight();
    max_scroll_ = std::max(0.0f, total - pane_layout.md_rect.height);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;

    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (!more) {
        KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
    }
}

void MainWindow::UpdateLayoutAndScroll(float desired_scroll) {
    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().ComputeLayout(nodes_, md_width);

    auto pane_layout = GetPaneLayout();
    float total = renderer_.GetLayout().GetTotalHeight();
    max_scroll_ = std::max(0.0f, total - pane_layout.md_rect.height);
    scroll_y_ = std::clamp(desired_scroll, 0.0f, max_scroll_);
    scroll_target_ = scroll_y_;

    UpdateScrollBar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::LoadMarkdownFile(const std::wstring& path) {
    std::string content = FileLoader::LoadFile(path);
    if (content.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return; // File doesn't exist
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
}

void MainWindow::UpdateTitleBar() {
    std::wstring title = L"MaDView";
    if (!current_file_.empty()) {
        // Extract filename
        auto pos = current_file_.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            title = current_file_.substr(pos + 1) + L" - MaDView";
        } else {
            title = current_file_ + L" - MaDView";
        }
    }
    SetWindowTextW(hwnd_, title.c_str());
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

    // Find the node at this y position
    for (int i = 0; i < static_cast<int>(nodes_.size()); i++) {
        const auto& node = nodes_[i];
        if (dip_y < node.y_position) continue;
        if (dip_y > node.y_position + node.height) continue;

        if (node.type == NodeType::Table) {
            return HitTestTable(node, i, dip_x, dip_y);
        }

        if (!node.text_layout) continue;

        // Found the node, now hit-test within its text layout
        float indent = node.indent_level * theme.indent_width;
        float local_x = dip_x - theme.margin_left - indent;
        float local_y = dip_y - node.y_position;

        BOOL is_trailing = FALSE;
        BOOL is_inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        node.text_layout->HitTestPoint(local_x, local_y,
                                       &is_trailing, &is_inside, &metrics);

        result.node_index = i;
        result.text_pos = metrics.textPosition + (is_trailing ? 1 : 0);
        return result;
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
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return;

    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;
    float dip_x = px / scale;
    float dip_y = py / scale;

    auto zone = PaneAtPoint(dip_x, dip_y);

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
            auto layout = GetPaneLayout();
            const auto& theme = renderer_.GetTheme();
            float content_top = layout.file_rect.y + theme.pane_header_height;
            float content_height = layout.file_rect.height - theme.pane_header_height;
            float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height;
            float local_x = dip_x - layout.file_rect.x;

            // Check if click is on scrollbar area (rightmost PANE_SCROLLBAR_WIDTH + margin)
            if (local_x >= layout.file_rect.width - PANE_SCROLLBAR_WIDTH - 4.0f
                && total_content > content_height) {
                SetCapture(hwnd_);
                drag_target_ = DragTarget::FileScrollbar;

                // Calculate current thumb position for offset
                float thumb_ratio = content_height / total_content;
                float thumb_height = std::max(PANE_SCROLLBAR_THUMB_MIN, content_height * thumb_ratio);
                float max_s = total_content - content_height;
                float scroll_ratio = (max_s > 0) ? file_scroll_.scroll_y / max_s : 0.0f;
                float thumb_y = content_top + scroll_ratio * (content_height - thumb_height);

                if (dip_y >= thumb_y && dip_y <= thumb_y + thumb_height) {
                    // Clicked on thumb — store offset
                    drag_scroll_offset_ = dip_y - thumb_y;
                } else {
                    // Clicked on track — jump thumb center to click
                    drag_scroll_offset_ = thumb_height * 0.5f;
                    float new_thumb_y = dip_y - drag_scroll_offset_;
                    float track_range = content_height - thumb_height;
                    float ratio = (track_range > 0) ? (new_thumb_y - content_top) / track_range : 0.0f;
                    ratio = std::clamp(ratio, 0.0f, 1.0f);
                    file_scroll_.scroll_y = ratio * max_s;
                    file_scroll_.max_scroll = max_s;
                    renderer_.InvalidateFilePaneCache();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }

            float local_y = dip_y - content_top + file_scroll_.scroll_y;
            int idx = file_explorer_.HitTest(local_y, theme.pane_item_height);
            if (idx >= 0 && idx < static_cast<int>(file_explorer_.GetEntries().size())) {
                const auto& entry = file_explorer_.GetEntries()[idx];
                if (entry.is_directory) {
                    // Navigate into the directory
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
            auto layout = GetPaneLayout();
            const auto& theme = renderer_.GetTheme();
            float content_top = layout.toc_rect.y + theme.pane_header_height;
            float content_height = layout.toc_rect.height - theme.pane_header_height;
            float total_content = static_cast<float>(toc_.GetEntries().size()) * theme.pane_item_height;
            float local_x = dip_x - layout.toc_rect.x;

            // Check if click is on scrollbar area
            if (local_x >= layout.toc_rect.width - PANE_SCROLLBAR_WIDTH - 4.0f
                && total_content > content_height) {
                SetCapture(hwnd_);
                drag_target_ = DragTarget::TocScrollbar;

                float thumb_ratio = content_height / total_content;
                float thumb_height = std::max(PANE_SCROLLBAR_THUMB_MIN, content_height * thumb_ratio);
                float max_s = total_content - content_height;
                float scroll_ratio = (max_s > 0) ? toc_scroll_.scroll_y / max_s : 0.0f;
                float thumb_y = content_top + scroll_ratio * (content_height - thumb_height);

                if (dip_y >= thumb_y && dip_y <= thumb_y + thumb_height) {
                    drag_scroll_offset_ = dip_y - thumb_y;
                } else {
                    drag_scroll_offset_ = thumb_height * 0.5f;
                    float new_thumb_y = dip_y - drag_scroll_offset_;
                    float track_range = content_height - thumb_height;
                    float ratio = (track_range > 0) ? (new_thumb_y - content_top) / track_range : 0.0f;
                    ratio = std::clamp(ratio, 0.0f, 1.0f);
                    toc_scroll_.scroll_y = ratio * max_s;
                    toc_scroll_.max_scroll = max_s;
                    renderer_.InvalidateTocPaneCache();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }

            float local_y = dip_y - content_top + toc_scroll_.scroll_y;
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

    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;
    float dip_x = px / scale;

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
        float dpi_x2, dpi_y2;
        rt->GetDpi(&dpi_x2, &dpi_y2);
        float scale2 = dpi_x2 / 96.0f;
        float dip_y2 = py / scale2;

        auto layout = GetPaneLayout();
        const auto& theme = renderer_.GetTheme();
        float content_top = layout.file_rect.y + theme.pane_header_height;
        float content_height = layout.file_rect.height - theme.pane_header_height;
        float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height;
        float max_s = total_content - content_height;

        float thumb_ratio = content_height / total_content;
        float thumb_height = std::max(PANE_SCROLLBAR_THUMB_MIN, content_height * thumb_ratio);
        float track_range = content_height - thumb_height;

        float new_thumb_y = dip_y2 - drag_scroll_offset_;
        float ratio = (track_range > 0) ? (new_thumb_y - content_top) / track_range : 0.0f;
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        file_scroll_.scroll_y = ratio * max_s;
        file_scroll_.max_scroll = max_s;
        renderer_.InvalidateFilePaneCache();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (drag_target_ == DragTarget::TocScrollbar) {
        float dpi_x2, dpi_y2;
        rt->GetDpi(&dpi_x2, &dpi_y2);
        float scale2 = dpi_x2 / 96.0f;
        float dip_y2 = py / scale2;

        auto layout = GetPaneLayout();
        const auto& theme = renderer_.GetTheme();
        float content_top = layout.toc_rect.y + theme.pane_header_height;
        float content_height = layout.toc_rect.height - theme.pane_header_height;
        float total_content = static_cast<float>(toc_.GetEntries().size()) * theme.pane_item_height;
        float max_s = total_content - content_height;

        float thumb_ratio = content_height / total_content;
        float thumb_height = std::max(PANE_SCROLLBAR_THUMB_MIN, content_height * thumb_ratio);
        float track_range = content_height - thumb_height;

        float new_thumb_y = dip_y2 - drag_scroll_offset_;
        float ratio = (track_range > 0) ? (new_thumb_y - content_top) / track_range : 0.0f;
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        toc_scroll_.scroll_y = ratio * max_s;
        toc_scroll_.max_scroll = max_s;
        renderer_.InvalidateTocPaneCache();
        InvalidateRect(hwnd_, nullptr, FALSE);
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
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return;

    float dpi_x, dpi_y;
    rt->GetDpi(&dpi_x, &dpi_y);
    float scale = dpi_x / 96.0f;
    float dip_x = px / scale;
    float dip_y = py / scale;

    auto zone = PaneAtPoint(dip_x, dip_y);
    if (zone != PaneZone::MdPane) return;

    auto hit = HitTest(px, py);
    if (hit.node_index < 0) return;

    const auto& text = nodes_[hit.node_index].text;
    if (text.empty()) return;

    // Find word boundaries
    uint32_t pos = hit.text_pos;
    if (pos >= text.size()) pos = static_cast<uint32_t>(text.size()) - 1;

    auto is_word_char = [](wchar_t c) {
        return IsCharAlphaNumericW(c) || c == L'_';
    };

    if (!is_word_char(text[pos])) return;

    uint32_t word_start = pos;
    while (word_start > 0 && is_word_char(text[word_start - 1])) word_start--;

    uint32_t word_end = pos + 1;
    while (word_end < text.size() && is_word_char(text[word_end])) word_end++;

    anchor_node_ = hit.node_index;
    anchor_pos_ = word_start;
    selection_ = TextSelection::MakeOrdered(
        hit.node_index, word_start, hit.node_index, word_end);
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

    std::wstring result;
    for (int i = selection_.start_node; i <= selection_.end_node; i++) {
        if (i < 0 || i >= static_cast<int>(nodes_.size())) continue;
        const auto& text = nodes_[i].text;

        uint32_t start = 0;
        uint32_t end = static_cast<uint32_t>(text.size());
        if (i == selection_.start_node) start = selection_.start_pos;
        if (i == selection_.end_node)   end = selection_.end_pos;

        if (start < end && start < text.size()) {
            if (end > text.size()) end = static_cast<uint32_t>(text.size());
            result += text.substr(start, end - start);
        }
        // Add newline between nodes
        if (i < selection_.end_node) {
            result += L"\r\n";
        }
    }

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

    const auto& node = nodes_[hit.node_index];
    for (const auto& run : node.runs) {
        if (run.link_url.has_value() &&
            hit.text_pos >= run.start &&
            hit.text_pos < run.start + run.length) {
            return run.link_url;
        }
    }
    return std::nullopt;
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
    if (anchor.empty()) return;

    // Convert anchor to lowercase for comparison
    std::wstring target = anchor;
    for (auto& c : target) {
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    }

    for (const auto& node : nodes_) {
        if (node.type == NodeType::Heading && node.anchor_id == target) {
            float target_y = node.y_position - renderer_.GetTheme().heading_spacing_above;
            target_y = std::max(0.0f, target_y);
            SmoothScrollBy(target_y - scroll_y_);
            return;
        }
    }
}
