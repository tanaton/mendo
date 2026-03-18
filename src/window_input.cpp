#include "window.h"
#include "resource.h"
#include "pane_layout.h"
#include "document_utils.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <variant>

// ── Visitor helper for std::visit ──
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

// ---- Mouse wheel / Keyboard ----

void MainWindow::OnMouseWheel(int px, int py, short delta) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip.x, pane_layout,
                                renderer_.GetTheme().splitter_width,
                                panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

    MouseWheelEvent event{delta, false, zone};
    ExecuteActions(controller_.HandleMouseWheel(event));
}

void MainWindow::OnKeyDown(WPARAM key) {
    KeyDownEvent event{
        static_cast<int>(key),
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0
    };
    ExecuteActions(controller_.HandleKeyDown(event));
}

void MainWindow::ExecuteActions(const ActionList& actions) {
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
                        static_cast<float>(toc_.GetEntries().size()) * theme.pane_item_height
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
                    if (!current_file_.empty()) PushNavHistory();
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

void MainWindow::OnDropFiles(HDROP hDrop) {
    wchar_t path[MAX_PATH];
    if (DragQueryFileW(hDrop, 0, path, MAX_PATH)) {
        if (!current_file_.empty()) PushNavHistory();
        LoadMarkdownFile(path);
    }
    DragFinish(hDrop);
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
        bool has_selection = viewport_.GetSelection().active && viewport_.GetSelection().start_node >= 0;
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

// ---- Right-click gesture ----

void MainWindow::OnRButtonDown(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    // Don't start gesture during left-button drag
    if (viewport_.IsDragging()) return;

    auto dip = PixelToDip(px, py);
    auto zone = PaneAtPoint(dip.x, dip.y);

    // Only handle gesture in MD pane
    if (zone != PaneZone::MdPane) return;

    gesture_.OnRButtonDown(dip.x, dip.y);
    SetCapture(hwnd_);
}

void MainWindow::OnRButtonUp(int px, int py) {
    // OnRButtonUp must be called BEFORE ReleaseCapture().
    // ReleaseCapture() sends WM_CAPTURECHANGED synchronously,
    // which would Reset() the gesture before we read the result.
    auto result = gesture_.OnRButtonUp();
    ReleaseCapture();

    switch (result) {
        case GestureResult::ShowContextMenu: {
            gesture_.Reset();
            // Convert client coords to screen coords for context menu
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
}

void MainWindow::OnRButtonMove(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    gesture_.OnMouseMove(dip.x, dip.y);

    if (gesture_.IsGestureActive()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---- Hit Testing ----

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
    float dip_y = screen_y / scale + viewport_.GetScrollY();

    // Offset by MD pane position
    auto pane_layout = GetPaneLayout();
    float md_left = pane_layout.md_rect.x;
    dip_x -= md_left;

    // Binary search for the node containing dip_y
    int lo = 0, hi = static_cast<int>(nodes_.size()) - 1;
    int candidate = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (layout_cache_[mid].y_position <= dip_y) {
            candidate = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (candidate >= 0 && dip_y <= layout_cache_[candidate].y_position + layout_cache_[candidate].height) {
        const auto& node = nodes_[candidate];
        const auto& entry = layout_cache_[candidate];

        if (node.type == NodeType::Table) {
            return HitTestTable(node, entry, candidate, dip_x, dip_y);
        }

        if (entry.text_layout) {
            float indent = node.indent_level * theme.indent_width;
            float local_x = dip_x - theme.margin_left - indent;
            float local_y = dip_y - entry.y_position;

            BOOL is_trailing = FALSE;
            BOOL is_inside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            entry.text_layout->HitTestPoint(local_x, local_y,
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

MainWindow::HitResult MainWindow::HitTestTable(const Node& node, const NodeLayoutEntry& entry,
                                                int node_index,
                                                float dip_x, float dip_y) const {
    HitResult result;
    result.node_index = node_index;

    const auto& theme = renderer_.GetTheme();
    float indent = node.indent_level * theme.indent_width;
    float base_x = theme.margin_left + indent;
    float cell_padding = 8.0f;
    float border = 1.0f;

    // Find which row was clicked
    float ry = entry.y_position;
    int hit_row = -1;
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        float row_h = (r < entry.row_heights.size()) ? entry.row_heights[r] : (theme.font_size_body * 1.4f);
        float row_bottom = ry + row_h + border;
        if (dip_y < row_bottom) {
            hit_row = static_cast<int>(r);
            break;
        }
        ry += row_h + border;
    }
    if (hit_row < 0) {
        result.text_pos = static_cast<uint32_t>(node.text.size());
        return result;
    }

    // Find which column was clicked
    float cx = base_x + border;
    int hit_col = static_cast<int>(entry.col_widths.size()) - 1; // default to last
    for (size_t c = 0; c < entry.col_widths.size(); c++) {
        float col_right = cx + entry.col_widths[c] + cell_padding * 2.0f;
        if (dip_x < col_right) {
            hit_col = static_cast<int>(c);
            break;
        }
        cx += entry.col_widths[c] + cell_padding * 2.0f + border;
    }
    if (hit_col < 0) hit_col = 0;

    // Compute flat text offset for cell (hit_row, hit_col)
    uint32_t flat_offset = 0;
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        const auto& row_cells = node.table_rows[r].cells;
        for (size_t c = 0; c < row_cells.size(); c++) {
            if (static_cast<int>(r) == hit_row && static_cast<int>(c) == hit_col) {
                // Hit test within the cell's text layout
                IDWriteTextLayout* cell_layout = nullptr;
                if (r < entry.cell_layouts.size() && c < entry.cell_layouts[r].size()) {
                    cell_layout = entry.cell_layouts[r][c].Get();
                }
                if (cell_layout) {
                    // Compute cell text position
                    float cell_x = base_x + border;
                    for (size_t cc = 0; cc < c; cc++) {
                        cell_x += entry.col_widths[cc] + cell_padding * 2.0f + border;
                    }
                    float cell_text_x = cell_x + cell_padding;

                    float cell_y = entry.y_position;
                    for (size_t rr = 0; rr < r; rr++) {
                        float rh = (rr < entry.row_heights.size()) ? entry.row_heights[rr] : (theme.font_size_body * 1.4f);
                        cell_y += rh + border;
                    }
                    float cell_text_y = cell_y + cell_padding;

                    BOOL is_trailing = FALSE, is_inside = FALSE;
                    DWRITE_HIT_TEST_METRICS metrics{};
                    cell_layout->HitTestPoint(
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

// ---- Mouse events ----

void MainWindow::OnLButtonDown(int px, int py) {
    if (!renderer_.GetRenderTarget()) return;

    auto dip = PixelToDip(px, py);
    float dip_x = dip.x;
    float dip_y = dip.y;

    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip_x, pane_layout,
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
        case PaneZone::FilePane: {
            const auto& theme = renderer_.GetTheme();
            float total_content = static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height;
            auto scroll_info = ComputePaneScrollInfo(pane_layout.file_rect, total_content);
            float local_x = dip_x - pane_layout.file_rect.x;

            // Check if click is on scrollbar area
            if (local_x >= pane_layout.file_rect.width - PANE_SCROLLBAR_WIDTH - 4.0f
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
                    if (!current_file_.empty()) {
                        file_explorer_.SetCurrentFile(current_file_);
                    }
                    panes_.FileScroll() = {};
                    renderer_.InvalidateFilePaneCache();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else if (!file_entry.is_current) {
                    PushNavHistory();
                    LoadMarkdownFile(file_entry.full_path);
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
                panes_.StartDrag(PaneController::DragTarget::TocScrollbar);
                bool dirty = false;
                HandleScrollbarClick(dip_y, scroll_info, panes_.TocScroll(), dirty);
                if (dirty) renderer_.InvalidateTocPaneCache();
                return;
            }

            float local_y = dip_y - scroll_info.content_top + panes_.TocScroll().scroll_y;
            int idx = toc_.HitTest(local_y, theme.pane_item_height);
            if (idx >= 0 && idx < static_cast<int>(toc_.GetEntries().size())) {
                PushNavHistory();
                NavigateToAnchor(toc_.GetEntries()[idx].anchor_id);
            }
            return;
        }
        case PaneZone::MdPane: {
            // Check nav overlay buttons first
            auto nav_hit = NavButtonHitTest(dip_x, dip_y, pane_layout.md_rect);
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

    // MD pane: existing selection logic
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

void MainWindow::OnLButtonUp(int px, int py) {
    ReleaseCapture();

    if (panes_.GetDragTarget() != PaneController::DragTarget::None) {
        panes_.EndDrag();
        // Recalculate layout after splitter drag
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

        // If it was a click (not a drag), check for link
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

void MainWindow::OnMouseMove(int px, int py) {
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) return;

    auto dip = PixelToDip(px, py);
    float dip_x = dip.x;
    auto size = rt->GetSize();
    float splitter_w = renderer_.GetTheme().splitter_width;

    // Handle splitter dragging
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
        float total_content = static_cast<float>(toc_.GetEntries().size()) * renderer_.GetTheme().pane_item_height;
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

    // MD pane: existing drag selection logic
    if (!viewport_.IsDragging()) return;
    auto hit = HitTest(px, py);
    if (hit.node_index >= 0) {
        viewport_.SetSelection(TextSelection::MakeOrdered(
            viewport_.GetAnchorNode(), viewport_.GetAnchorPos(), hit.node_index, hit.text_pos));
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

    viewport_.SetAnchor(hit.node_index, wb.start);
    viewport_.SetSelection(TextSelection::MakeOrdered(
        hit.node_index, wb.start, hit.node_index, wb.end));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- Selection / Clipboard ----

void MainWindow::ClearSelection() {
    viewport_.ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SelectAll() {
    viewport_.SelectAll(nodes_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CopySelectionToClipboard() const {
    if (!viewport_.GetSelection().active) return;

    std::wstring result = ExtractSelectedText(nodes_, viewport_.GetSelection());
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

// ---- Link navigation ----

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
        PushNavHistory();
        NavigateToAnchor(anchor);
        return;
    }

    // External link: open in default browser
    ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::NavigateToAnchor(const std::wstring& anchor) {
    int idx = FindAnchorNodeIndex(nodes_, anchor);
    if (idx < 0) return;

    float target_y = layout_cache_[idx].y_position - renderer_.GetTheme().heading_spacing_above;
    target_y = std::max(0.0f, target_y);
    viewport_.ScrollTo(target_y);
    UpdateScrollBar();
    InvalidateMdPane();
}

void MainWindow::PushNavHistory() {
    nav_history_.Push({current_file_, viewport_.GetScrollY()});
}

void MainWindow::NavigateToEntry(const NavEntry& entry) {
    if (entry.file_path != current_file_ && !entry.file_path.empty()) {
        // Load a different file without pushing history (this IS a history navigation)
        loading_path_ = entry.file_path;
        DoLoadMarkdownFile();
        // After loading, override scroll to the remembered position
        viewport_.ScrollTo(entry.scroll_y);
    } else {
        viewport_.ScrollTo(entry.scroll_y);
    }
    UpdateScrollBar();
    InvalidateMdPane();
}

void MainWindow::NavigateBack() {
    NavEntry out;
    if (nav_history_.GoBack({current_file_, viewport_.GetScrollY()}, out)) {
        NavigateToEntry(out);
    }
}

void MainWindow::NavigateForward() {
    NavEntry out;
    if (nav_history_.GoForward({current_file_, viewport_.GetScrollY()}, out)) {
        NavigateToEntry(out);
    }
}

MainWindow::NavButtonHover MainWindow::NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const {
    // Must match the constants in renderer.cpp
    constexpr float BTN_SIZE = 32.0f;
    constexpr float BTN_MARGIN = 16.0f;
    constexpr float BTN_GAP = 2.0f;

    float base_x = md_rect.x + md_rect.width - BTN_MARGIN - BTN_SIZE * 2 - BTN_GAP - 16.0f;
    float base_y = md_rect.y + md_rect.height - BTN_MARGIN - BTN_SIZE;

    if (dip_y < base_y || dip_y > base_y + BTN_SIZE) return NavButtonHover::None;

    // Back button
    if (dip_x >= base_x && dip_x <= base_x + BTN_SIZE)
        return NavButtonHover::Back;
    // Forward button
    float fwd_x = base_x + BTN_SIZE + BTN_GAP;
    if (dip_x >= fwd_x && dip_x <= fwd_x + BTN_SIZE)
        return NavButtonHover::Forward;

    return NavButtonHover::None;
}
