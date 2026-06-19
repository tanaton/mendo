#include "app.h"
#include "document_utils.h"

void App::OnLButtonDblClk(int px, int py)
{
    if (!IsRenderReady()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    // CS_DBLCLKS により連続クリックの2回目は WM_LBUTTONDBLCLK になるため、
    // タイトルバーボタンのクリックを先に処理する。
    if (HandleTitleBarClick(dip.x, dip.y)) {
        return;
    }
    const auto zone = PaneAtPoint(dip.x);
    if (zone != PaneZone::MdPane) {
        return;
    }
    // 検索バーのボタンも連打として扱う (タイトルバーボタンと同じ理由)。
    const auto& layout = GetPaneLayout();
    if (HandleSearchBarClick(dip.x, dip.y, layout, true)) {
        return;
    }
    const auto hit = HitTest(px, py);
    if (hit.node_index < 0) {
        return;
    }
    const auto& node = state_.document.doc.GetNodes()[hit.node_index];
    const std::string_view text = node.LinearizedText();
    if (text.empty()) {
        return;
    }
    const auto wb = FindWordBoundaries(text, hit.text_pos);
    if (!wb.found) {
        return;
    }

    state_.view.viewport.SetAnchor(hit.node_index, wb.start);
    state_.view.viewport.SetSelection(TextSelection::MakeOrdered(hit.node_index, wb.start, hit.node_index, wb.end));
    InvalidateMdPane(layout.md_rect);
}
