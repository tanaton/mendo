#include "app.h"

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
    Dispatch(SelectWordAction{ hit.node_index, hit.text_pos });
}
