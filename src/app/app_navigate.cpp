#include "app.h"
#include "nav.h"
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
    auto result = ::HandleLinkClick(url);
    switch (result.type) {
    case LinkClickResult::Type::Anchor:
        PushCurrentNavEntry(state_);
        Dispatch(NavigateAnchorAction{ std::move(result.target) });
        break;
    case LinkClickResult::Type::ExternalUrl:
        EmitEffect(effect::ShellOpen{ std::move(result.target) });
        break;
    default:
        break;
    }
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
    EmitEffect(effect::ViewportLayout{ md_width, md_height });

    EmitEffect(effect::SyncMaxScroll{ md_height });
    resource_manager_.RequestMermaidRenders();
    ScheduleDeferredLayoutIfNeeded();
    Invalidate();

    EmitEffect(effect::SyncTocActive{});
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

