// 右ボタンジェスチャー・Xボタンナビゲーション関連のマウス入力処理。
#include "app.h"
#include "app_events.h"
#include "pane_layout.h"

bool App::OnRButtonDown(int px, int py)
{
    if (!IsRenderReady()) {
        return false;
    }
    if (state_.view.viewport.IsDragging()) {
        return false;
    }
    const auto dip = PixelToDip(px, py);
    const auto zone = PaneAtPoint(dip.x, dip.y);
    if (zone != PaneZone::MdPane) {
        return false;
    }
    state_.interaction.gesture.OnRButtonDown(dip.x, dip.y);
    SetCapture(hwnd_);
    return true;
}

bool App::OnRButtonUp(int px, int py)
{
    if (state_.interaction.gesture.GetPhase() == GesturePhase::Idle) {
        return false;
    }
    const auto result = state_.interaction.gesture.OnRButtonUp();
    ReleaseCapture();

    switch (result) {
    case GestureResult::ShowContextMenu: {
        state_.interaction.gesture.Reset();
        POINT pt{ px, py };
        ClientToScreen(hwnd_, &pt);
        OnContextMenu(pt.x, pt.y);
        break;
    }
    case GestureResult::Back:
        Dispatch(NavigateBackAction{});
        Invalidate();
        break;
    case GestureResult::Forward:
        Dispatch(NavigateForwardAction{});
        Invalidate();
        break;
    case GestureResult::None:
        Invalidate();
        break;
    }
    return true;
}

void App::OnRButtonMove(int px, int py)
{
    if (!IsRenderReady()) {
        return;
    }
    const auto dip = PixelToDip(px, py);
    state_.interaction.gesture.OnMouseMove(dip.x, dip.y);

    if (state_.interaction.gesture.IsGestureActive()) {
        Invalidate();
    }
}

void App::OnXButtonBack()
{
    Dispatch(XButtonBackAction{});
}

void App::OnXButtonForward()
{
    Dispatch(XButtonForwardAction{});
}
