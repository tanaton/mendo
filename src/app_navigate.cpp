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
    const auto result = nav_service_.HandleLinkClick(url, doc_.GetFilePath());
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
    const int idx = doc_.FindAnchorIndex(anchor);
    if (idx < 0) {
        return;
    }

    const auto layout = GetPaneLayout();
    float target_y = layout_cache_[idx].y_position - renderer_.GetTheme().heading_spacing_above - layout.md_rect.y;
    target_y = std::max(0.0f, target_y);
    viewport_.ScrollTo(target_y);
    UpdateScrollBar();
    InvalidateMdPane(layout.md_rect);
    resource_manager_.ScheduleBitmapManage();
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
        scroll_restore_.pending_nav_scroll_y = result.scroll_y;
        LoadMarkdownFile(result.target);
        return;
    }
    viewport_.ScrollTo(result.scroll_y);
    const auto layout = GetPaneLayout();
    UpdateScrollBar();
    InvalidateMdPane(layout.md_rect);
    resource_manager_.ScheduleBitmapManage();
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

void App::FinishThemeOrZoomChange(const AnchorState& anchor, float offset_scale)
{
    auto layout = GetPaneLayout();
    float md_width = layout.md_rect.width;
    float md_height = layout.md_rect.height;

    // 表示領域を優先的にレイアウトし、残りは遅延処理に委ねる
    layout_service_->ViewportLayout(doc_, layout_cache_, md_width, md_height);

    RestoreAnchorWithScale(anchor, offset_scale);

    SyncMaxScroll(md_height);
    resource_manager_.RequestMermaidRenders();
    ScheduleDeferredLayoutIfNeeded();
    UpdateScrollBar();
    Invalidate();
}

void App::ToggleDarkMode()
{
    theme_service_.ToggleDarkMode();
    InvalidatePaneLayoutCache();

    const auto anchor = SaveAnchor();

    renderer_.SetTheme(theme_service_.CreateTheme(viewport_.GetZoomIndex()));
    const bool dark = theme_service_.IsDarkMode();
    ApplyDarkModeToWindow(hwnd_, dark);
    tooltip_.ApplyDarkMode(dark);

    // 全レイアウトとMermaid図を一括で無効化
    layout_cache_.InvalidateAllWithDiagrams(doc_.GetNodes());
    mermaid_renderer_.CancelPending();
    mermaid_renderer_.ClearCache();

    FinishThemeOrZoomChange(anchor, 1.0f);
    theme_service_.SaveDarkMode();
}

// ============================================================
// ズーム
// ============================================================

void App::ZoomIn()
{
    const float z = viewport_.ZoomIn();
    if (z > 0.0f) {
        ApplyZoom(z);
    }
}

void App::ZoomOut()
{
    const float z = viewport_.ZoomOut();
    if (z > 0.0f) {
        ApplyZoom(z);
    }
}

void App::ZoomReset()
{
    const float z = viewport_.ZoomReset();
    if (z > 0.0f) {
        ApplyZoom(z);
    }
}

void App::ApplyZoom(float new_zoom)
{
    InvalidatePaneLayoutCache();
    const auto anchor = SaveAnchor();

    const float old_zoom = renderer_.GetTheme().zoom;
    const float zoom_ratio = new_zoom / old_zoom;

    panes_.ApplyZoom(zoom_ratio);

    const Theme base = theme_service_.CreateTheme();
    renderer_.ApplyZoomFromBase(base, new_zoom);

    layout_cache_.InvalidateAllLayouts();

    FinishThemeOrZoomChange(anchor, zoom_ratio);
    UpdateTitleBar();
    theme_service_.SaveZoomLevel(viewport_.GetZoomIndex());
}
