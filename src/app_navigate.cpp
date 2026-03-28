#include "app.h"
#include "document_utils.h"
#include <shellapi.h>
#include <algorithm>

// ============================================================
// リンクナビゲーション
// ============================================================

void App::HandleLinkClick(std::wstring_view url)
{
    if (url.empty()) {
        return;
    }
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

void App::NavigateToAnchor(std::wstring_view anchor)
{
    int idx = doc_.FindAnchorIndex(anchor);
    if (idx < 0) {
        return;
    }

    auto layout = GetPaneLayout();
    float target_y = layout_cache_[idx].y_position - renderer_.GetTheme().heading_spacing_above - layout.md_rect.y;
    target_y = std::max(0.0f, target_y);
    viewport_.ScrollTo(target_y);
    UpdateScrollBar();
    InvalidateMdPane(layout.md_rect);
}

void App::PushNavHistory()
{
    nav_service_.PushHistory(doc_.GetFilePath(), viewport_.GetScrollY());
}

void App::ApplyNavigateResult(const NavigationService::NavigateResult& result)
{
    if (result.type == NavigationService::NavigateResult::Type::None) {
        return;
    }

    if (result.type == NavigationService::NavigateResult::Type::LoadFile) {
        pending_nav_scroll_y_ = result.scroll_y;
        LoadMarkdownFile(result.target);
        return;
    }
    viewport_.ScrollTo(result.scroll_y);
    auto layout = GetPaneLayout();
    UpdateScrollBar();
    InvalidateMdPane(layout.md_rect);
}

void App::NavigateBack()
{
    ApplyNavigateResult(nav_service_.GoBack(doc_.GetFilePath(), viewport_.GetScrollY()));
}

void App::NavigateForward()
{
    ApplyNavigateResult(nav_service_.GoForward(doc_.GetFilePath(), viewport_.GetScrollY()));
}

// ============================================================
// ダークモード
// ============================================================

void App::ToggleDarkMode()
{
    theme_service_.ToggleDarkMode();
    InvalidatePaneLayoutCache();
    renderer_.SetTheme(theme_service_.CreateTheme(viewport_.GetZoomIndex()));
    ApplyDarkModeToWindow(hwnd_, theme_service_.IsDarkMode());

    // 全レイアウトとMermaid図を一括で無効化
    layout_cache_.InvalidateAllWithDiagrams(doc_.GetNodes());
    mermaid_renderer_.CancelPending();
    mermaid_renderer_.ClearCache();

    UpdateLayoutAndScroll(viewport_.GetScrollY());
    RequestMermaidRenders();
    theme_service_.SaveDarkMode();
}

// ============================================================
// ズーム
// ============================================================

void App::ZoomIn()
{
    float z = viewport_.ZoomIn();
    if (z > 0.0f) {
        ApplyZoom(z);
    }
}

void App::ZoomOut()
{
    float z = viewport_.ZoomOut();
    if (z > 0.0f) {
        ApplyZoom(z);
    }
}

void App::ZoomReset()
{
    float z = viewport_.ZoomReset();
    if (z > 0.0f) {
        ApplyZoom(z);
    }
}

void App::ApplyZoom(float new_zoom)
{
    InvalidatePaneLayoutCache();
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;
    float anchor_offset = viewport_.GetScrollY() - anchor_y_before;

    float old_zoom = renderer_.GetTheme().zoom;
    float zoom_ratio = new_zoom / old_zoom;

    panes_.ApplyZoom(zoom_ratio);

    Theme base = theme_service_.CreateTheme();
    renderer_.ApplyZoomFromBase(base, new_zoom);

    layout_cache_.InvalidateAllLayouts();

    auto layout = GetPaneLayout();
    float md_width = layout.md_rect.width;
    float md_height = layout.md_rect.height;

    // 表示領域を優先的にレイアウトし、残りは遅延処理に委ねる
    layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);

    if (anchor_idx >= 0 && anchor_idx < static_cast<int>(doc_.GetNodes().size())) {
        float anchor_y_after = layout_cache_[anchor_idx].y_position;
        viewport_.SetScrollY(anchor_y_after + anchor_offset * zoom_ratio);
    }

    SyncMaxScroll(md_height);
    viewport_.SetScrollTarget(viewport_.GetScrollY());

    RequestMermaidRenders();

    // 残りのダーティノードを遅延レイアウト
    if (layout_service_->HasDirtyNodes()) {
        SetTimer(hwnd_, TIMER_DEFERRED_LAYOUT, 16, nullptr);
    }

    UpdateScrollBar();
    UpdateTitleBar();
    theme_service_.SaveZoomLevel(viewport_.GetZoomIndex());
    Invalidate();
}
