#include "app.h"
#include "document_utils.h"
#include "pane_layout.h"

void App::OnLButtonDblClk(int px, int py)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    const auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index < 0) {
        return;
    }
    const auto& text = doc_.GetNodes()[hit.node_index].GetText();
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) {
        return;
    }

    viewport_.SetAnchor(hit.node_index, wb.start);
    viewport_.SetSelection(TextSelection::MakeOrdered(hit.node_index, wb.start, hit.node_index, wb.end));
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

// ============================================================
// 選択 / クリップボード
// ============================================================

void App::ClearSelection()
{
    viewport_.ClearSelection();
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

void App::SelectAll()
{
    viewport_.SelectAll(doc_.GetNodes());
    const auto layout = GetPaneLayout();
    InvalidateMdPane(layout.md_rect);
}

void App::SetClipboardText(std::wstring_view text) const
{
    if (text.empty()) {
        return;
    }
    if (!OpenClipboard(hwnd_)) {
        return;
    }
    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* ptr = GlobalLock(hMem);
        if (ptr) {
            memcpy(ptr, text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(ptr)[text.size()] = L'\0';
            GlobalUnlock(hMem);
            if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                GlobalFree(hMem);
            }
        }
        else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

void App::CopySelectionToClipboard() const
{
    if (!viewport_.GetSelection().active) {
        return;
    }
    const std::pmr::wstring result = ExtractSelectedText(doc_.GetNodes(), viewport_.GetSelection());
    SetClipboardText(result);
}

void App::CopyCodeBlockToClipboard(int node_index) const
{
    const auto& nodes = doc_.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size())) {
        return;
    }
    SetClipboardText(nodes[node_index].GetText());
}
