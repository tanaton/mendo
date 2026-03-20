#include "app.h"
#include "parser.h"
#include "resource.h"
#include "pane_layout.h"
#include "document_utils.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <variant>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// DWMWA_USE_IMMERSIVE_DARK_MODE (supported on Windows 10 1809+ / Windows 11)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Visitor helper for std::visit
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

void ApplyDarkModeToWindow(HWND hwnd, bool dark) {
    // Dark title bar
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // Dark scrollbar via explorer theme
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// ============================================================
// Initialization
// ============================================================

bool App::Init(HWND hwnd) {
    hwnd_ = hwnd;

    if (!renderer_.Init(hwnd_)) return false;

    layout_service_.emplace(renderer_.GetLayout(), viewport_);

    // Cache DPI scale for PixelToDip (updated in OnDpiChanged)
    float init_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    cached_dpi_scale_ = (init_dpi > 0.0f) ? (init_dpi / 96.0f) : 1.0f;

    // Initialize Mermaid renderer (WebView2, async)
    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), [this]() {
        RequestMermaidRenders();
    });

    // When D2D device is lost and render target is recreated, update MermaidRenderer
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
    });

    // Apply saved dark mode and zoom preferences
    theme_service_.LoadDarkMode();
    viewport_.SetZoomIndex(theme_service_.LoadZoomIndex());
    if (theme_service_.IsDarkMode() || viewport_.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        renderer_.SetTheme(theme_service_.CreateTheme(viewport_.GetZoomIndex()));
    }
    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }

    // Cache system cursors
    cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
    cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
    cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
    cursor_sizewe_ = LoadCursorW(nullptr, IDC_SIZEWE);

    // Set up file watch timer (check every 250ms)
    SetTimer(hwnd_, TIMER_FILE_WATCH, 250, nullptr);

    return true;
}

// ============================================================
// Helpers
// ============================================================

App::DipPoint App::PixelToDip(int px, int py) const {
    return {px / cached_dpi_scale_, py / cached_dpi_scale_};
}

PaneScrollInfo App::ComputePaneScrollInfo(
    const PaneRect& rect, float total_content) const {
    return ComputeScrollInfo(rect, renderer_.GetTheme().pane_header_height, total_content);
}

void App::HandleScrollbarClick(float dip_y, const PaneScrollInfo& info,
                               ScrollState& scroll, bool& cache_dirty) {
    float thumb_y = ComputeThumbY(info, scroll.scroll_y);

    if (dip_y >= thumb_y && dip_y <= thumb_y + info.thumb_height) {
        panes_.SetDragScrollOffset(dip_y - thumb_y);
    } else {
        panes_.SetDragScrollOffset(info.thumb_height * 0.5f);
        float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
        scroll.scroll_y = ScrollFromThumbY(info, new_thumb_y);
        scroll.max_scroll = info.max_scroll;
        cache_dirty = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::HandleScrollbarDrag(float dip_y, const PaneScrollInfo& info,
                              ScrollState& scroll, bool& cache_dirty) {
    float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
    scroll.scroll_y = ScrollFromThumbY(info, new_thumb_y);
    scroll.max_scroll = info.max_scroll;
    cache_dirty = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================
// Pane Layout
// ============================================================

PaneLayout App::GetPaneLayout() const {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return {};

    auto size = rt->GetSize();
    return panes_.ComputeLayout(size.width, size.height,
                                renderer_.GetTheme().splitter_width);
}

PaneZone App::PaneAtPoint(float dip_x, [[maybe_unused]] float dip_y) const {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return PaneZone::None;
    auto size = rt->GetSize();
    return panes_.DetectZone(dip_x, size.width, size.height,
                             renderer_.GetTheme().splitter_width);
}

float App::GetMarkdownPaneWidth() const {
    auto layout = GetPaneLayout();
    return layout.md_rect.width;
}

// ============================================================
// Paint / Resize
// ============================================================

void App::OnPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    auto layout = GetPaneLayout();
    if (!file_load_service_.IsLoading()) {
        // Ensure any dirty nodes now visible are laid out at the current width
        int anchor_idx = FindFirstVisibleNode();
        float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;

        bool updated = layout_service_->EnsureVisibleLayout(
            doc_, layout_cache_, layout.md_rect.width, layout.md_rect.height);

        if (updated) {
            AnchorCompensateScroll(anchor_idx, anchor_y_before, layout.md_rect.height);
        }
    }
    GestureRenderState gs;
    gs.trail_active = gesture_.IsGestureActive();
    gs.trail_points = &gesture_.GetTrailPoints();
    gs.overlay_visible = gesture_.IsOverlayVisible();
    gs.direction = (gesture_.GetDirection() == GestureDirection::Left) ? -1
                 : (gesture_.GetDirection() == GestureDirection::Right) ? 1 : 0;
    gs.overlay_alpha = gesture_.GetOverlayAlpha();

    SidePaneState sp{layout.file_rect, layout.toc_rect,
                     file_explorer_.GetEntries(), panes_.FileScroll(), panes_.GetHoveredFileIndex(),
                     doc_.GetToc().GetEntries(), panes_.TocScroll(), panes_.GetHoveredTocIndex(),
                     panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible()};

    if (file_load_service_.IsLoading()) {
        renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, gs);
    } else {
        renderer_.Render(doc_.GetNodesMut(), layout_cache_, viewport_.GetScrollY(), viewport_.GetSelection(),
                         layout.md_rect, sp,
                         nav_service_.CanGoBack(), nav_service_.CanGoForward(),
                         static_cast<int>(nav_hover_), gs);
    }

    EndPaint(hwnd_, &ps);
}

void App::OnResize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;

    renderer_.Resize(width, height);

    if (is_sizing_) {
        auto sizing_layout = GetPaneLayout();
        float sizing_h = sizing_layout.md_rect.height;
        SyncMaxScroll(sizing_h);
        UpdateScrollBar(sizing_h);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;

    layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);

    SyncMaxScroll(md_height);
    UpdateScrollBar(md_height);
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (layout_service_->HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    RequestMermaidRenders();
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested) {
    cached_dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    if (cached_dpi_scale_ <= 0.0f) cached_dpi_scale_ = 1.0f;
    renderer_.SetDpi(static_cast<float>(dpi));

    layout_cache_.MarkAllDirty();

    SetWindowPos(hwnd_, nullptr,
        suggested->left, suggested->top,
        suggested->right - suggested->left,
        suggested->bottom - suggested->top,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

// ============================================================
// Sizing state
// ============================================================

void App::OnEnterSizeMove() {
    is_sizing_ = true;
    StopSmoothScroll();
}

void App::OnExitSizeMove() {
    is_sizing_ = false;
    OnResizeEnd();
}

// ============================================================
// File loading
// ============================================================

void App::LoadMarkdownFile(const std::wstring& path) {
    if (!DocumentService::NeedsLoadingAnimation(path)) {
        file_load_service_.SetLoadingPath(path);
        DoLoadMarkdownFile();
    } else {
        file_load_service_.StartLoading(path);
        SetTimer(hwnd_, TIMER_LOADING_ANIM, 16, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        PostMessage(hwnd_, WM_APP_LOAD_FILE, 0, 0);
    }
}

void App::DoLoadMarkdownFile() {
    KillTimer(hwnd_, TIMER_LOADING_ANIM);

    viewport_.ClearSelection();
    mermaid_renderer_.CancelPending();

    if (!file_load_service_.ExecuteLoad(doc_, layout_cache_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    std::wstring dir = doc_.GetDirectory();
    if (!dir.empty()) {
        file_explorer_.SetDirectory(dir);
        file_explorer_.SetCurrentFile(doc_.GetFilePath());
    }

    panes_.ResetScrollStates();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();

    UpdateLayoutAndScroll(0.0f);
    UpdateTitleBar();
    RequestMermaidRenders();

    doc_service_.StartWatching(doc_.GetFilePath(), [this]() {
        ReloadCurrentFile();
    });
}

void App::ReloadCurrentFile() {
    if (doc_.GetFilePath().empty()) return;

    float old_scroll = viewport_.GetScrollY();
    mermaid_renderer_.CancelPending();

    if (file_load_service_.ExecuteReload(doc_, layout_cache_)) {
        renderer_.InvalidateTocPaneCache();
        UpdateLayoutAndScroll(old_scroll);
        RequestMermaidRenders();
    }
}

void App::UpdateTitleBar() {
    int zoom_percent = static_cast<int>(ZOOM_STEPS[viewport_.GetZoomIndex()] * 100.0f + 0.5f);
    SetWindowTextW(hwnd_, BuildTitleString(doc_.GetFilePath(), zoom_percent).c_str());
}

void App::RequestMermaidRenders() {
    if (!mermaid_renderer_.IsReady()) return;

    float viewport_width = GetMarkdownPaneWidth();
    float content_width = viewport_width
                          - renderer_.GetTheme().margin_left
                          - renderer_.GetTheme().margin_right;

    if (last_mermaid_content_width_ > 0.0f &&
        static_cast<int>(content_width) != static_cast<int>(last_mermaid_content_width_)) {
        for (size_t i = 0; i < doc_.GetNodes().size(); i++) {
            if (doc_.GetNodes()[i].code_language == SyntaxLanguage::Mermaid) {
                auto& diagram = layout_cache_.GetDiagram(i);
                diagram.bitmap.Reset();
                diagram.width = 0;
                diagram.height = 0;
            }
        }
        mermaid_renderer_.ClearCache();
    }
    last_mermaid_content_width_ = content_width;

    for (size_t i = 0; i < doc_.GetNodes().size(); i++) {
        auto& node = doc_.GetNodesMut()[i];
        if (node.type != NodeType::CodeBlock) continue;
        if (node.code_language != SyntaxLanguage::Mermaid) continue;
        auto& diagram = layout_cache_.GetDiagram(i);
        if (diagram.bitmap) continue;

        mermaid_renderer_.RequestRender(node, layout_cache_[i], diagram,
                                        content_width, theme_service_.IsDarkMode(), [this]() {
            int anchor_idx = FindFirstVisibleNode();
            float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;
            layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
            SyncMaxScroll();
            AnchorCompensateScroll(anchor_idx, anchor_y_before);
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
    }
}

// ============================================================
// Mouse wheel / Keyboard
// ============================================================

void App::OnMouseWheel(int px, int py, short delta, bool ctrl) {
    if (!renderer_.GetRenderTarget()) return;

    if (ctrl) {
        MouseWheelEvent event{delta, true, PaneZone::MdPane};
        ExecuteActions(controller_.HandleMouseWheel(event));
        return;
    }

    auto dip = PixelToDip(px, py);
    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip.x, pane_layout,
                                renderer_.GetTheme().splitter_width,
                                panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

    MouseWheelEvent event{delta, false, zone};
    ExecuteActions(controller_.HandleMouseWheel(event));
}

void App::OnKeyDown(WPARAM key) {
    KeyDownEvent event{
        static_cast<int>(key),
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0
    };
    ExecuteActions(controller_.HandleKeyDown(event));
}

void App::ExecuteActions(const ActionList& actions) {
    for (const auto& action : actions) {
        std::visit(overloaded{
            [this](const KeyScrollAction& a) {
                auto pane_layout = GetPaneLayout();
                float page_size = pane_layout.md_rect.height;
                switch (a.type) {
                    case ScrollType::LineUp:   SmoothScrollBy(-40.0f); break;
                    case ScrollType::LineDown: SmoothScrollBy(40.0f); break;
                    case ScrollType::PageUp:   SmoothScrollBy(-page_size * 0.9f); break;
                    case ScrollType::PageDown: SmoothScrollBy(page_size * 0.9f); break;
                    case ScrollType::Home:     SmoothScrollBy(-viewport_.GetScrollY()); break;
                    case ScrollType::End:      SmoothScrollBy(viewport_.GetMaxScroll() - viewport_.GetScrollY()); break;
                }
            },
            [this](const SmoothScrollByAction& a) {
                SmoothScrollBy(a.delta);
            },
            [this](const ScrollPaneAction& a) {
                auto pane_layout = GetPaneLayout();
                const auto& theme = renderer_.GetTheme();
                if (a.pane == PaneZone::FilePane) {
                    float max_file_scroll = std::max(0.0f,
                        static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height
                        - (pane_layout.file_rect.height - theme.pane_header_height));
                    if (panes_.ScrollFilePaneBy(a.delta, max_file_scroll)) {
                        renderer_.InvalidateFilePaneCache();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                } else if (a.pane == PaneZone::TocPane) {
                    float max_toc_scroll = std::max(0.0f,
                        static_cast<float>(doc_.GetToc().GetEntries().size()) * theme.pane_item_height
                        - (pane_layout.toc_rect.height - theme.pane_header_height));
                    if (panes_.ScrollTocPaneBy(a.delta, max_toc_scroll)) {
                        renderer_.InvalidateTocPaneCache();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            },
            [this](const CopyClipboardAction&) {
                CopySelectionToClipboard();
            },
            [this](const SelectAllAction&) {
                SelectAll();
            },
            [this](const ClearSelectionAction&) {
                ClearSelection();
            },
            [this](const TogglePaneAction& a) {
                if (a.file_pane) panes_.ToggleFilePane();
                else panes_.ToggleTocPane();
                RECT rc;
                GetClientRect(hwnd_, &rc);
                OnResize(static_cast<UINT>(rc.right - rc.left),
                         static_cast<UINT>(rc.bottom - rc.top));
            },
            [this](const ZoomAction& a) {
                if (a.direction > 0) ZoomIn();
                else if (a.direction < 0) ZoomOut();
                else ZoomReset();
            },
            [this](const ReloadFileAction&) {
                ReloadCurrentFile();
            },
            [this](const OpenFileAction&) {
                auto path = FileLoader::OpenFileDialog(hwnd_);
                if (!path.empty()) {
                    if (!doc_.GetFilePath().empty()) PushNavHistory();
                    LoadMarkdownFile(path);
                }
            },
            [this](const ToggleDarkModeAction&) {
                ToggleDarkMode();
            },
            [this](const NavigateBackAction&) {
                NavigateBack();
            },
            [this](const NavigateForwardAction&) {
                NavigateForward();
            },
        }, action);
    }
}

void App::OnDropFiles(HDROP hDrop) {
    UINT required = DragQueryFileW(hDrop, 0, nullptr, 0);
    if (required > 0) {
        std::wstring path(required, L'\0');
        if (DragQueryFileW(hDrop, 0, path.data(), required + 1)) {
            if (!doc_.GetFilePath().empty()) PushNavHistory();
            LoadMarkdownFile(path);
        }
    }
    DragFinish(hDrop);
}

void App::OnContextMenu(int screen_x, int screen_y) {
    POINT pt = {screen_x, screen_y};
    POINT client_pt = pt;
    ScreenToClient(hwnd_, &client_pt);
    auto dip = PixelToDip(client_pt.x, client_pt.y);
    auto zone = PaneAtPoint(dip.x, dip.y);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (zone == PaneZone::MdPane) {
        bool has_file = !doc_.GetFilePath().empty();
        AppendMenuW(menu, MF_STRING | (has_file ? 0 : MF_GRAYED), IDM_EDIT_FILE, L"エディタで開く(&E)");

        bool has_selection = viewport_.GetSelection().active && viewport_.GetSelection().start_node >= 0;
        AppendMenuW(menu, MF_STRING | (has_selection ? 0 : MF_GRAYED), IDM_COPY, L"コピー(&C)");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING | (theme_service_.IsDarkMode() ? MF_CHECKED : 0),
                IDM_TOGGLE_DARK_MODE, L"ダークモード(&D)");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == IDM_EDIT_FILE) {
        ShellExecuteW(hwnd_, L"open", doc_.GetFilePath().c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == IDM_COPY) {
        CopySelectionToClipboard();
    } else if (cmd == IDM_TOGGLE_DARK_MODE) {
        ToggleDarkMode();
    }
}

// ============================================================
// Right-click gesture
// ============================================================

bool App::OnRButtonDown(int px, int py) {
    if (!renderer_.GetRenderTarget()) return false;
    if (viewport_.IsDragging()) return false;

    auto dip = PixelToDip(px, py);
    auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) return false;

    gesture_.OnRButtonDown(dip.x, dip.y);
    SetCapture(hwnd_);
    return true;
}

bool App::OnRButtonUp(int px, int py) {
    if (gesture_.GetPhase() == GesturePhase::Idle) return false;

    auto result = gesture_.OnRButtonUp();
    ReleaseCapture();

    switch (result) {
        case GestureResult::ShowContextMenu: {
            gesture_.Reset();
            POINT pt = {px, py};
            ClientToScreen(hwnd_, &pt);
            OnContextMenu(pt.x, pt.y);
            break;
        }
        case GestureResult::Back:
            NavigateBack();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case GestureResult::Forward:
            NavigateForward();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case GestureResult::None:
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
    }
    return true;
}

void App::OnRButtonMove(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    gesture_.OnMouseMove(dip.x, dip.y);

    if (gesture_.IsGestureActive()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::OnXButtonBack() {
    NavigateBack();
}

void App::OnXButtonForward() {
    NavigateForward();
}

// ============================================================
// Hit Testing
// ============================================================

App::HitResult App::HitTest(int screen_x, int screen_y) const {
    auto pane_layout = GetPaneLayout();
    return hit_test_.HitTest(doc_.GetNodes(), layout_cache_,
                             renderer_.GetTheme(), viewport_.GetScrollY(),
                             pane_layout.md_rect.x, cached_dpi_scale_,
                             screen_x, screen_y);
}

// ============================================================
// Mouse events
// ============================================================

void App::HandleFilePaneClick(float dip_x, float dip_y, const PaneLayout& layout) {
    const auto& theme = renderer_.GetTheme();
    float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height;
    auto scroll_info = ComputePaneScrollInfo(layout.file_rect, total_content);
    float local_x = dip_x - layout.file_rect.x;

    if (local_x >= layout.file_rect.width - PANE_SCROLLBAR_WIDTH - 4.0f
        && total_content > scroll_info.content_height) {
        SetCapture(hwnd_);
        panes_.StartDrag(PaneController::DragTarget::FileScrollbar);
        bool dirty = false;
        HandleScrollbarClick(dip_y, scroll_info, panes_.FileScroll(), dirty);
        if (dirty) renderer_.InvalidateFilePaneCache();
        return;
    }

    float local_y = dip_y - scroll_info.content_top + panes_.FileScroll().scroll_y;
    int idx = file_explorer_.HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(file_explorer_.GetEntries().size())) {
        const auto& file_entry = file_explorer_.GetEntries()[idx];
        if (file_entry.is_directory) {
            file_explorer_.SetDirectory(file_entry.full_path);
            if (!doc_.GetFilePath().empty()) {
                file_explorer_.SetCurrentFile(doc_.GetFilePath());
            }
            panes_.FileScroll() = {};
            renderer_.InvalidateFilePaneCache();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (!file_entry.is_current) {
            PushNavHistory();
            LoadMarkdownFile(file_entry.full_path);
        }
    }
}

void App::HandleTocPaneClick(float dip_x, float dip_y, const PaneLayout& layout) {
    const auto& theme = renderer_.GetTheme();
    float total_content = static_cast<float>(doc_.GetToc().GetEntries().size()) * theme.pane_item_height;
    auto scroll_info = ComputePaneScrollInfo(layout.toc_rect, total_content);
    float local_x = dip_x - layout.toc_rect.x;

    if (local_x >= layout.toc_rect.width - PANE_SCROLLBAR_WIDTH - 4.0f
        && total_content > scroll_info.content_height) {
        SetCapture(hwnd_);
        panes_.StartDrag(PaneController::DragTarget::TocScrollbar);
        bool dirty = false;
        HandleScrollbarClick(dip_y, scroll_info, panes_.TocScroll(), dirty);
        if (dirty) renderer_.InvalidateTocPaneCache();
        return;
    }

    float local_y = dip_y - scroll_info.content_top + panes_.TocScroll().scroll_y;
    int idx = doc_.GetToc().HitTest(local_y, theme.pane_item_height);
    if (idx >= 0 && idx < static_cast<int>(doc_.GetToc().GetEntries().size())) {
        PushNavHistory();
        NavigateToAnchor(doc_.GetToc().GetEntries()[idx].anchor_id);
    }
}

void App::OnLButtonDown(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip.x, pane_layout,
                                renderer_.GetTheme().splitter_width,
                                panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

    switch (zone) {
        case PaneZone::Splitter1:
            SetCapture(hwnd_);
            panes_.StartDrag(PaneController::DragTarget::Splitter1);
            return;
        case PaneZone::Splitter2:
            SetCapture(hwnd_);
            panes_.StartDrag(PaneController::DragTarget::Splitter2);
            return;
        case PaneZone::FilePane:
            HandleFilePaneClick(dip.x, dip.y, pane_layout);
            return;
        case PaneZone::TocPane:
            HandleTocPaneClick(dip.x, dip.y, pane_layout);
            return;
        case PaneZone::MdPane: {
            auto nav_hit = hit_test_.NavButtonHitTest(dip.x, dip.y, pane_layout.md_rect);
            if (nav_hit == NavButtonHover::Back) {
                NavigateBack();
                return;
            }
            if (nav_hit == NavButtonHover::Forward) {
                NavigateForward();
                return;
            }
            break;
        }
        default:
            return;
    }

    // MD pane: selection logic
    SetCapture(hwnd_);
    viewport_.SetClickStart(px, py);
    auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        viewport_.SetAnchor(hit.node_index, hit.text_pos);
        viewport_.SetDragging(true);
        viewport_.GetSelectionMut().Clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::OnLButtonUp(int px, int py) {
    ReleaseCapture();

    if (panes_.GetDragTarget() != PaneController::DragTarget::None) {
        panes_.EndDrag();
        RECT rc;
        GetClientRect(hwnd_, &rc);
        OnResize(static_cast<UINT>(rc.right - rc.left),
                 static_cast<UINT>(rc.bottom - rc.top));
        return;
    }

    if (viewport_.IsDragging()) {
        auto hit = HitTest(px, py);
        if (hit.node_index >= 0) {
            viewport_.SetSelection(TextSelection::MakeOrdered(
                viewport_.GetAnchorNode(), viewport_.GetAnchorPos(), hit.node_index, hit.text_pos));
        }
        viewport_.SetDragging(false);

        int dx = px - viewport_.GetClickStartX();
        int dy = py - viewport_.GetClickStartY();
        if (!viewport_.GetSelection().active && (dx * dx + dy * dy) < 25) {
            auto link = GetLinkAtHit(hit);
            if (link.has_value()) {
                HandleLinkClick(link.value());
            }
        }

        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::OnMouseMove(int px, int py) {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return;

    auto dip = PixelToDip(px, py);
    float dip_x = dip.x;
    auto size = rt->GetSize();
    float splitter_w = renderer_.GetTheme().splitter_width;

    if (panes_.GetDragTarget() == PaneController::DragTarget::Splitter1) {
        panes_.DragSplitter1To(dip_x, size.width, splitter_w);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::FileScrollbar) {
        auto layout = GetPaneLayout();
        float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        auto info = ComputePaneScrollInfo(layout.file_rect, total_content);
        bool dirty = false;
        HandleScrollbarDrag(dip.y, info, panes_.FileScroll(), dirty);
        if (dirty) renderer_.InvalidateFilePaneCache();
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::TocScrollbar) {
        auto layout = GetPaneLayout();
        float total_content = static_cast<float>(doc_.GetToc().GetEntries().size()) * renderer_.GetTheme().pane_item_height;
        auto info = ComputePaneScrollInfo(layout.toc_rect, total_content);
        bool dirty = false;
        HandleScrollbarDrag(dip.y, info, panes_.TocScroll(), dirty);
        if (dirty) renderer_.InvalidateTocPaneCache();
        return;
    }

    if (panes_.GetDragTarget() == PaneController::DragTarget::Splitter2) {
        panes_.DragSplitter2To(dip_x, size.width, splitter_w);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // MD pane: drag selection
    if (!viewport_.IsDragging()) return;
    auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        viewport_.SetSelection(TextSelection::MakeOrdered(
            viewport_.GetAnchorNode(), viewport_.GetAnchorPos(), hit.node_index, hit.text_pos));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::OnMouseHover(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    float dip_x = dip.x;
    float dip_y = dip.y;

    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip_x, pane_layout,
                                renderer_.GetTheme().splitter_width,
                                panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

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
            new_toc_hover = doc_.GetToc().HitTest(local_y, renderer_.GetTheme().pane_item_height);
            break;
        }
        case PaneZone::MdPane: {
            auto nav_hit = hit_test_.NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
            auto old_nav_hover = nav_hover_;
            nav_hover_ = nav_hit;
            if (nav_hit != NavButtonHover::None) {
                SetCursor(cursor_hand_);
                if (nav_hit != old_nav_hover)
                    InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            }
            if (old_nav_hover != NavButtonHover::None)
                InvalidateRect(hwnd_, nullptr, FALSE);

            int dx = px - last_md_hit_pos_.x;
            int dy = py - last_md_hit_pos_.y;
            if (dx * dx + dy * dy > 16) {
                auto hit = HitTest(px, py);
                auto link = GetLinkAtHit(hit);
                last_md_cursor_hand_ = link.has_value();
                last_md_hit_pos_ = {px, py};
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

void App::OnLButtonDblClk(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) return;

    auto hit = HitTest(px, py);
    if (hit.node_index < 0) return;

    const auto& text = doc_.GetNodes()[hit.node_index].text;
    if (text.empty()) return;

    auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) return;

    viewport_.SetAnchor(hit.node_index, wb.start);
    viewport_.SetSelection(TextSelection::MakeOrdered(
        hit.node_index, wb.start, hit.node_index, wb.end));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================
// Selection / Clipboard
// ============================================================

void App::ClearSelection() {
    viewport_.ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SelectAll() {
    viewport_.SelectAll(doc_.GetNodes());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CopySelectionToClipboard() const {
    if (!viewport_.GetSelection().active) return;

    std::wstring result = ExtractSelectedText(doc_.GetNodes(), viewport_.GetSelection());
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
            if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                GlobalFree(hMem);
            }
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

// ============================================================
// Link navigation
// ============================================================

std::optional<std::wstring> App::GetLinkAtHit(const HitResult& hit) const {
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(doc_.GetNodes().size()))
        return std::nullopt;

    return FindLinkAtPosition(doc_.GetNodes()[hit.node_index], hit.text_pos);
}

void App::HandleLinkClick(const std::wstring& url) {
    if (url.empty()) return;

    auto result = nav_service_.HandleLinkClick(url, doc_.GetFilePath());
    switch (result.type) {
        case NavigationService::NavigateResult::Type::Anchor:
            PushNavHistory();
            NavigateToAnchor(result.target);
            break;
        case NavigationService::NavigateResult::Type::ExternalUrl:
            ShellExecuteW(hwnd_, L"open", result.target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        default:
            break;
    }
}

void App::NavigateToAnchor(const std::wstring& anchor) {
    int idx = FindAnchorNodeIndex(doc_.GetNodes(), anchor);
    if (idx < 0) return;

    float target_y = layout_cache_[idx].y_position - renderer_.GetTheme().heading_spacing_above;
    target_y = std::max(0.0f, target_y);
    viewport_.ScrollTo(target_y);
    UpdateScrollBar();
    InvalidateMdPane();
}

void App::PushNavHistory() {
    nav_service_.PushHistory(doc_.GetFilePath(), viewport_.GetScrollY());
}

void App::ApplyNavigateResult(const NavigationService::NavigateResult& result) {
    if (result.type == NavigationService::NavigateResult::Type::None) return;

    if (result.type == NavigationService::NavigateResult::Type::LoadFile) {
        file_load_service_.SetLoadingPath(result.target);
        DoLoadMarkdownFile();
    }
    viewport_.ScrollTo(result.scroll_y);
    UpdateScrollBar();
    InvalidateMdPane();
}

void App::NavigateBack() {
    ApplyNavigateResult(nav_service_.GoBack(doc_.GetFilePath(), viewport_.GetScrollY()));
}

void App::NavigateForward() {
    ApplyNavigateResult(nav_service_.GoForward(doc_.GetFilePath(), viewport_.GetScrollY()));
}

// ============================================================
// Timer callbacks
// ============================================================

void App::OnSmoothScrollTimer() {
    UpdateSmoothScroll();
}

void App::OnFileWatchTimer() {
    doc_service_.CheckForChanges();
}

void App::OnDeferredLayoutTimer() {
    OnDeferredLayout();
}

void App::OnLoadingAnimTimer() {
    file_load_service_.TickLoadingAnimation();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnAppLoadFile() {
    DoLoadMarkdownFile();
}

void App::OnCaptureChanged() {
    if (gesture_.GetPhase() != GesturePhase::Idle) {
        gesture_.Reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::OnDestroy() {
    SaveLastFilePath();
    KillTimer(hwnd_, TIMER_FILE_WATCH);
    KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
    KillTimer(hwnd_, TIMER_LOADING_ANIM);
}

// ============================================================
// Scrollbar & Scroll
// ============================================================

void App::OnVScroll(WPARAM wParam) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(hwnd_, SB_VERT, &si);

    float old_pos = viewport_.GetScrollY();
    auto pane_layout = GetPaneLayout();
    float page_size = pane_layout.md_rect.height;

    switch (LOWORD(wParam)) {
        case SB_LINEUP:    ScrollTo(viewport_.GetScrollY() - 40.0f); break;
        case SB_LINEDOWN:  ScrollTo(viewport_.GetScrollY() + 40.0f); break;
        case SB_PAGEUP:    ScrollTo(viewport_.GetScrollY() - page_size); break;
        case SB_PAGEDOWN:  ScrollTo(viewport_.GetScrollY() + page_size); break;
        case SB_THUMBTRACK:
            viewport_.SetScrollbarTracking(true);
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_THUMBPOSITION:
            viewport_.SetScrollbarTracking(false);
            ScrollTo(static_cast<float>(si.nTrackPos));
            break;
        case SB_ENDSCROLL:
            viewport_.SetScrollbarTracking(false);
            break;
        case SB_TOP:       ScrollTo(0.0f); break;
        case SB_BOTTOM:    ScrollTo(viewport_.GetMaxScroll()); break;
    }

    if (viewport_.GetScrollY() != old_pos) {
        UpdateScrollBar(page_size);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::UpdateScrollBar() {
    auto pane_layout = GetPaneLayout();
    UpdateScrollBar(pane_layout.md_rect.height);
}

void App::UpdateScrollBar(float md_pane_height) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    si.nMin = 0;
    si.nMax = static_cast<int>(layout_service_->GetTotalHeight());
    si.nPage = static_cast<UINT>(md_pane_height);
    si.nPos = static_cast<int>(viewport_.GetScrollY());
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void App::ScrollTo(float position) {
    viewport_.ScrollTo(position);
    last_md_hit_pos_ = {LONG_MIN, LONG_MIN};
}

void App::SmoothScrollBy(float delta) {
    bool was_scrolling = viewport_.IsSmoothScrolling();
    viewport_.SmoothScrollBy(delta);

    if (!was_scrolling && viewport_.IsSmoothScrolling()) {
        SetTimer(hwnd_, TIMER_SMOOTH_SCROLL, 16, nullptr);
    }
}

void App::UpdateSmoothScroll() {
    bool still_active = viewport_.UpdateSmoothScroll();

    if (!still_active) {
        KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
    }

    UpdateScrollBar();
    InvalidateMdPane();
}

void App::InvalidateMdPane() {
    if (!renderer_.GetRenderTarget()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    float scale = cached_dpi_scale_;
    auto layout = GetPaneLayout();
    RECT rc;
    rc.left = static_cast<LONG>(layout.md_rect.x * scale);
    rc.top = 0;
    rc.right = static_cast<LONG>((layout.md_rect.x + layout.md_rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>(layout.md_rect.height * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::StopSmoothScroll() {
    if (!viewport_.IsSmoothScrolling()) return;
    viewport_.StopSmoothScroll();
    KillTimer(hwnd_, TIMER_SMOOTH_SCROLL);
}

void App::SyncMaxScroll() {
    auto pane_layout = GetPaneLayout();
    float total = layout_service_->GetTotalHeight();
    viewport_.SyncMaxScroll(total, pane_layout.md_rect.height);
}

void App::SyncMaxScroll(float md_pane_height) {
    float total = layout_service_->GetTotalHeight();
    viewport_.SyncMaxScroll(total, md_pane_height);
}

int App::FindFirstVisibleNode() const {
    return viewport_.FindFirstVisibleNode(layout_cache_, doc_.GetNodes().size());
}

void App::AnchorCompensateScroll(int anchor_idx, float anchor_y_before) {
    viewport_.AnchorCompensateScroll(anchor_idx, anchor_y_before, layout_cache_);
    SyncMaxScroll();
}

void App::AnchorCompensateScroll(int anchor_idx, float anchor_y_before, float md_pane_height) {
    viewport_.AnchorCompensateScroll(anchor_idx, anchor_y_before, layout_cache_);
    SyncMaxScroll(md_pane_height);
}

// ============================================================
// Deferred layout
// ============================================================

void App::OnResizeEnd() {
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;

    layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);

    SyncMaxScroll(md_height);
    UpdateScrollBar(md_height);
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (layout_service_->HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    RequestMermaidRenders();
}

void App::OnDeferredLayout() {
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;

    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;
    bool more = layout_service_->ProcessDirtyBatch(doc_, layout_cache_, md_width, 200);

    if (!viewport_.IsScrollbarTracking()) {
        AnchorCompensateScroll(anchor_idx, anchor_y_before, md_height);
    } else {
        SyncMaxScroll(md_height);
    }

    if (!more) {
        KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
        UpdateScrollBar(md_height);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::UpdateLayoutAndScroll(float desired_scroll) {
    auto pane_layout = GetPaneLayout();
    float md_width = pane_layout.md_rect.width;
    float md_height = pane_layout.md_rect.height;
    layout_service_->FullLayout(doc_, layout_cache_, md_width);

    viewport_.SetScrollY(desired_scroll);
    viewport_.SetScrollTarget(desired_scroll);
    SyncMaxScroll(md_height);

    UpdateScrollBar(md_height);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================
// Dark mode
// ============================================================

void App::ToggleDarkMode() {
    theme_service_.ToggleDarkMode();
    Theme new_theme = theme_service_.CreateTheme(viewport_.GetZoomIndex());
    renderer_.SetTheme(new_theme);
    ApplyDarkModeToWindow(hwnd_, theme_service_.IsDarkMode());

    // Invalidate all layouts and mermaid diagrams in a single pass
    for (size_t i = 0; i < doc_.GetNodes().size(); ++i) {
        auto& entry = layout_cache_[i];
        entry.text_layout.Reset();
        entry.effects_applied = false;
        entry.inline_code_bgs.clear();
        if (doc_.GetNodes()[i].code_language == SyntaxLanguage::Mermaid) {
            layout_cache_.GetDiagram(i).bitmap.Reset();
        }
    }
    mermaid_renderer_.CancelPending();
    mermaid_renderer_.ClearCache();

    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().UpdateTheme(renderer_.GetTheme());
    renderer_.GetLayout().RecreateFormats();
    renderer_.GetLayout().LayoutNodes(doc_.GetNodesMut(), layout_cache_, md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);
    float total_height = ComputeTotalContentHeight(layout_cache_, doc_.GetNodes().size(), renderer_.GetTheme().margin_top);
    auto* rt = renderer_.GetRenderTarget();
    float viewport_height;
    if (rt) {
        viewport_height = rt->GetSize().height;
    } else {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        viewport_height = static_cast<float>(rc.bottom - rc.top) / cached_dpi_scale_;
    }
    viewport_.SyncMaxScroll(total_height, viewport_height);

    RequestMermaidRenders();
    theme_service_.SaveDarkMode();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================
// Zoom
// ============================================================

void App::ZoomIn() {
    float z = viewport_.ZoomIn();
    if (z > 0.0f) ApplyZoom(z);
}

void App::ZoomOut() {
    float z = viewport_.ZoomOut();
    if (z > 0.0f) ApplyZoom(z);
}

void App::ZoomReset() {
    float z = viewport_.ZoomReset();
    if (z > 0.0f) ApplyZoom(z);
}

void App::ApplyZoom(float new_zoom) {
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;
    float anchor_offset = viewport_.GetScrollY() - anchor_y_before;

    float old_zoom = renderer_.GetTheme().zoom;
    float zoom_ratio = new_zoom / old_zoom;

    panes_.ApplyZoom(zoom_ratio);

    Theme base = theme_service_.CreateTheme();
    renderer_.ApplyZoomFromBase(base, new_zoom);

    layout_cache_.InvalidateAllLayouts();

    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().LayoutNodes(doc_.GetNodesMut(), layout_cache_,
        md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);

    if (anchor_idx >= 0 && anchor_idx < static_cast<int>(doc_.GetNodes().size())) {
        float anchor_y_after = layout_cache_[anchor_idx].y_position;
        viewport_.SetScrollY(anchor_y_after + anchor_offset * zoom_ratio);
    }

    SyncMaxScroll();
    viewport_.SetScrollTarget(viewport_.GetScrollY());

    UpdateScrollBar();
    UpdateTitleBar();
    theme_service_.SaveZoomLevel(viewport_.GetZoomIndex());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================
// Last file persistence
// ============================================================

void App::SaveLastFilePath() {
    if (doc_.GetFilePath().empty()) return;
    config_.SaveWString(L"last_file.txt", doc_.GetFilePath());
}

std::wstring App::LoadLastFilePath() const {
    std::wstring path = config_.LoadWString(L"last_file.txt");
    if (!path.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}
