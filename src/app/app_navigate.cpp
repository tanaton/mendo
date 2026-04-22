#include "app.h"
#include "navigation_service.h"
#include "document_utils.h"
#include <algorithm>

// ============================================================
// リンクナビゲーション
// ============================================================

void App::HandleLinkClick(std::wstring_view url)
{
    if (url.empty()) {
        return;
    }
    const auto result = ::HandleLinkClick(url);
    switch (result.type) {
    case LinkClickResult::Type::Anchor:
        PushNavHistory();
        NavigateToAnchor(result.target);
        break;
    case LinkClickResult::Type::ExternalUrl: {
        // reducer を経由せず effect を直接発火する。外部 URL 起動は state 遷移を
        // 伴わない単発 I/O で、hybrid モデル（reducer.h 参照）の service 経路扱い。
        SideEffectList effects;
        effects.emplace_back(effect::ShellOpen{ result.target });
        effect_executor_.Execute(effects);
        break;
    }
    default:
        break;
    }
}

void App::NavigateToAnchor(std::wstring_view anchor)
{
    const int idx = state_.document.doc.FindAnchorIndex(anchor);
    if (idx < 0) {
        return;
    }

    const auto layout = GetPaneLayout();
    const auto target = MakeHeadingTopTarget(idx, renderer_.GetTheme().heading_spacing_above, layout.md_rect.y);
    state_.view.viewport.SetScrollTarget(target.node, target.offset);

    const float md_width = layout.md_rect.width;
    const float md_height = layout.md_rect.height;
    state_.view.viewport.ApplyScrollTarget(state_.document.layout_cache);
    layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache, md_width, md_height);
    SyncMaxScroll(md_height);

    UpdateScrollBar();
    InvalidateMdPane(layout.md_rect);
    resource_manager_.ScheduleBitmapManage();
}

void App::PushNavHistory()
{
    PushCurrentNavEntry(state_);
}

// ============================================================
// ダークモード
// ============================================================

void App::SyncPaneThemeCache()
{
    state_.window.cached_theme = renderer_.GetTheme().ToReducerConstants();
}

void App::FinishThemeOrZoomChange()
{
    SyncPaneThemeCache();

    auto layout = GetPaneLayout();
    float md_width = layout.md_rect.width;
    float md_height = layout.md_rect.height;

    // 表示領域を優先的にレイアウトし、残りは遅延処理に委ねる
    layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache, md_width, md_height);

    SyncMaxScroll(md_height);
    resource_manager_.RequestMermaidRenders();
    ScheduleDeferredLayoutIfNeeded();
    UpdateScrollBar();
    Invalidate();
}

void App::HandleApplyThemeChange(const effect::ApplyThemeChange& e)
{
    if (e.type == effect::ApplyThemeChange::Type::Zoom) {
        const Theme base = theme_service_.CreateTheme();
        renderer_.ApplyZoomFromBase(base, e.new_zoom);
        FinishThemeOrZoomChange();
        UpdateTitleBar();
        theme_service_.SaveZoomLevel(e.zoom_index);
    }
    else {
        // DarkMode
        theme_service_.ToggleDarkMode();
        renderer_.SetTheme(theme_service_.CreateTheme(state_.view.viewport.GetZoomIndex()));
        const bool dark = theme_service_.IsDarkMode();
        ApplyDarkModeToWindow(hwnd_, dark);
        state_.interaction.tooltip.ApplyDarkMode(dark);
        mermaid_renderer_.CancelPending();
        mermaid_renderer_.ClearCache();
        FinishThemeOrZoomChange();
        theme_service_.SaveDarkMode();
    }
}

