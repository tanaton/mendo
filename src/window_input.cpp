#include "window.h"
#include "resource.h"
#include "pane_layout.h"
#include "document_utils.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

// ---- Mouse wheel / Keyboard ----

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

void MainWindow::OnDropFiles(HDROP hDrop) {
    wchar_t path[MAX_PATH];
    if (DragQueryFileW(hDrop, 0, path, MAX_PATH)) {
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

// ---- Mouse events ----

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

// ---- Selection / Clipboard ----

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
