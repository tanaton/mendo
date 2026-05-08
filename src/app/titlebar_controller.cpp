#include "titlebar_controller.h"
#include "app_state.h"
#include "d2d_util.h"
#include "document_utils.h"
#include "theme.h"
#include <cassert>

void TitleBarController::Init(HWND hwnd, AppState* state, Callbacks callbacks) noexcept
{
    assert(hwnd && state);
    hwnd_ = hwnd;
    state_ = state;
    callbacks_ = std::move(callbacks);
}

float TitleBarController::GetHeightDip() const noexcept
{
    return state_->window.titlebar.GetHeight();
}

TitleBarHitZone TitleBarController::HitTest(float dip_x, float dip_y) const noexcept
{
    return state_->window.titlebar.HitTest(dip_x, dip_y);
}

void TitleBarController::Invalidate() const noexcept
{
    const float tb_h = state_->window.titlebar.GetHeight();
    if (tb_h <= 0.0f) {
        return;
    }
    // 幅未計算 (初期化直後) は次のリサイズで全描画されるので何もしない。
    const float window_w = state_->pane_layout_cache.WindowWidth();
    if (window_w <= 0.0f) {
        return;
    }
    mendo::InvalidateDipRect(hwnd_, 0.0f, 0.0f, window_w, tb_h, state_->window.cached_dpi_scale);
}

bool TitleBarController::HandleClick(float dip_x, float dip_y)
{
    if (dip_y >= state_->window.titlebar.GetHeight()) {
        return false;
    }

    switch (state_->window.titlebar.HitTest(dip_x, dip_y)) {
    case TitleBarHitZone::OpenFile:
        callbacks_.dispatch(OpenFileAction{});
        break;
    case TitleBarHitZone::Help:
        callbacks_.dispatch(ShowHelpAction{});
        break;
    case TitleBarHitZone::Search:
        callbacks_.dispatch(OpenSearchBarAction{});
        break;
    case TitleBarHitZone::ThemeToggle:
        callbacks_.dispatch(ToggleDarkModeAction{});
        break;
    case TitleBarHitZone::FileToggle:
        callbacks_.dispatch(TogglePaneAction{ PaneTarget::File });
        break;
    case TitleBarHitZone::TocToggle:
        callbacks_.dispatch(TogglePaneAction{ PaneTarget::Toc });
        break;
    case TitleBarHitZone::Minimize:
        ShowWindow(hwnd_, SW_MINIMIZE);
        break;
    case TitleBarHitZone::Maximize:
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case TitleBarHitZone::Close:
        callbacks_.post_window_message(WM_CLOSE, 0, 0);
        break;
    default:
        // タイトルバーのドラッグ領域などは WM_NCHITTEST で処理済み。
        break;
    }
    return true;
}

void TitleBarController::Update()
{
    const int zoom_percent = static_cast<int>(ZOOM_STEPS[state_->view.viewport.GetZoomIndex()] * 100.0f + 0.5f);
    auto title = BuildTitleString(state_->document.doc.GetFilePath(), zoom_percent);
    if (title == state_->cached_title_text) {
        return;
    }
    SetWindowTextW(hwnd_, title.c_str());
    state_->cached_title_text = std::move(title);
    Invalidate();
}
