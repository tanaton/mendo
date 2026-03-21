#include "app.h"
#include "pane_layout.h"
#include "document_utils.h"
#include "resource.h"
#include <shellapi.h>

// ============================================================
// コンテキストメニュー
// ============================================================

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
// 右クリックジェスチャー
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
// ヒットテスト
// ============================================================

App::HitResult App::HitTest(int screen_x, int screen_y) const {
    auto pane_layout = GetPaneLayout();
    return hit_test_.HitTest(doc_.GetNodes(), layout_cache_,
                             renderer_.GetTheme(), viewport_.GetScrollY(),
                             pane_layout.md_rect.x, cached_dpi_scale_,
                             screen_x, screen_y);
}

std::optional<std::pmr::wstring> App::GetLinkAtHit(const HitResult& hit) const {
    if (hit.node_index < 0 || hit.node_index >= static_cast<int>(doc_.GetNodes().size()))
        return std::nullopt;

    return FindLinkAtPosition(doc_.GetNodes()[hit.node_index], hit.text_pos);
}

// ============================================================
// マウスイベント
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

    // MDペイン: 選択ロジック
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

    // MDペイン: ドラッグ選択
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
// 選択 / クリップボード
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

    std::pmr::wstring result = ExtractSelectedText(doc_.GetNodes(), viewport_.GetSelection());
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
