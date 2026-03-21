#include "app.h"
#include "document_utils.h"
#include <shellapi.h>
#include <algorithm>

// ============================================================
// リンクナビゲーション
// ============================================================

void App::HandleLinkClick(std::wstring_view url) {
    if (url.empty()) return;

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

void App::NavigateToAnchor(std::wstring_view anchor) {
    int idx = FindAnchorNodeIndex(doc_.GetNodes(), anchor);
    if (idx < 0) return;

    float target_y = layout_cache_[idx].y_position - renderer_.GetTheme().heading_spacing_above;
    target_y = std::max(0.0f, target_y);
    viewport_.ScrollTo(target_y);
    UpdateScrollBar();
    InvalidateMdPane();
}

void App::PushNavHistory() {
    nav_service_.PushHistory(doc_.GetFilePath(), viewport_.GetScrollY());
}

void App::ApplyNavigateResult(const NavigationService::NavigateResult& result) {
    if (result.type == NavigationService::NavigateResult::Type::None) return;

    if (result.type == NavigationService::NavigateResult::Type::LoadFile) {
        file_load_service_.SetLoadingPath(result.target);
        DoLoadMarkdownFile();
    }
    viewport_.ScrollTo(result.scroll_y);
    UpdateScrollBar();
    InvalidateMdPane();
}

void App::NavigateBack() {
    ApplyNavigateResult(nav_service_.GoBack(doc_.GetFilePath(), viewport_.GetScrollY()));
}

void App::NavigateForward() {
    ApplyNavigateResult(nav_service_.GoForward(doc_.GetFilePath(), viewport_.GetScrollY()));
}

// ============================================================
// ダークモード
// ============================================================

void App::ToggleDarkMode() {
    theme_service_.ToggleDarkMode();
    Theme new_theme = theme_service_.CreateTheme(viewport_.GetZoomIndex());
    renderer_.SetTheme(new_theme);
    ApplyDarkModeToWindow(hwnd_, theme_service_.IsDarkMode());

    // 全レイアウトとMermaid図を一括で無効化
    for (size_t i = 0; i < doc_.GetNodes().size(); ++i) {
        auto& entry = layout_cache_[i];
        entry.text_layout.Reset();
        entry.effects_applied = false;
        entry.inline_code_bgs.clear();
        if (doc_.GetNodes()[i].code_language == SyntaxLanguage::Mermaid) {
            layout_cache_.GetDiagram(i).bitmap.Reset();
        }
    }
    mermaid_renderer_.CancelPending();
    mermaid_renderer_.ClearCache();

    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().UpdateTheme(renderer_.GetTheme());
    renderer_.GetLayout().RecreateFormats();
    renderer_.GetLayout().LayoutNodes(doc_.GetNodesMut(), layout_cache_, md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);
    float total_height = ComputeTotalContentHeight(layout_cache_, doc_.GetNodes().size(), renderer_.GetTheme().margin_top);
    auto* rt = renderer_.GetRenderTarget();
    float viewport_height;
    if (rt) {
        viewport_height = rt->GetSize().height;
    } else {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        viewport_height = static_cast<float>(rc.bottom - rc.top) / cached_dpi_scale_;
    }
    viewport_.SyncMaxScroll(total_height, viewport_height);

    RequestMermaidRenders();
    theme_service_.SaveDarkMode();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================
// ズーム
// ============================================================

void App::ZoomIn() {
    float z = viewport_.ZoomIn();
    if (z > 0.0f) ApplyZoom(z);
}

void App::ZoomOut() {
    float z = viewport_.ZoomOut();
    if (z > 0.0f) ApplyZoom(z);
}

void App::ZoomReset() {
    float z = viewport_.ZoomReset();
    if (z > 0.0f) ApplyZoom(z);
}

void App::ApplyZoom(float new_zoom) {
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;
    float anchor_offset = viewport_.GetScrollY() - anchor_y_before;

    float old_zoom = renderer_.GetTheme().zoom;
    float zoom_ratio = new_zoom / old_zoom;

    panes_.ApplyZoom(zoom_ratio);

    Theme base = theme_service_.CreateTheme();
    renderer_.ApplyZoomFromBase(base, new_zoom);

    layout_cache_.InvalidateAllLayouts();

    float md_width = GetMarkdownPaneWidth();
    renderer_.GetLayout().LayoutNodes(doc_.GetNodesMut(), layout_cache_,
        md_width - renderer_.GetTheme().margin_left - renderer_.GetTheme().margin_right);

    if (anchor_idx >= 0 && anchor_idx < static_cast<int>(doc_.GetNodes().size())) {
        float anchor_y_after = layout_cache_[anchor_idx].y_position;
        viewport_.SetScrollY(anchor_y_after + anchor_offset * zoom_ratio);
    }

    SyncMaxScroll();
    viewport_.SetScrollTarget(viewport_.GetScrollY());

    UpdateScrollBar();
    UpdateTitleBar();
    theme_service_.SaveZoomLevel(viewport_.GetZoomIndex());
    InvalidateRect(hwnd_, nullptr, FALSE);
}
