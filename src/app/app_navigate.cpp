#include "app.h"
#include "darkmode_util.h"
#include "nav.h"
#include "document_utils.h"
#include "string_convert.h"
#include <algorithm>

void App::HandleLinkClick(std::string_view url)
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
    case LinkClickResult::Type::ExternalUrl: {
        // ShellExecuteW 用に UTF-8 → wstring 変換。
        std::pmr::wstring url_wide;
        string_convert::Utf8ToWide(result.target, url_wide);
        EmitEffect(effect::ShellOpen{ std::move(url_wide) });
        break;
    }
    default:
        break;
    }
}

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
        renderer_.ApplyZoomFromBase(base, ZOOM_STEPS[e.zoom_index]);
        FinishThemeOrZoomChange();
        UpdateTitleBar();
        theme_service_.SaveZoomLevel(e.zoom_index);
    }
    else {
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
