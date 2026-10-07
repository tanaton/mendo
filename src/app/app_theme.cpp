#include "app.h"
#include "darkmode_util.h"

void App::FinishThemeOrZoomChange()
{
    const PaneRect md = GetPaneLayout().md_rect;

    ViewportLayout(md.width, md.height);
    SyncMaxScroll(md.height);
    resource_manager_.RequestMermaidRenders();
    ScheduleDeferredLayoutIfNeeded();
    Invalidate();

    EmitEffect(effect::SyncTocActive{});
}

void App::HandleApplyThemeChange(const effect::ApplyThemeChange& e)
{
    if (e.type == effect::ApplyThemeChange::Type::Zoom) {
        renderer_.ApplyZoomFromBase(theme_service_.CreateTheme(), state_.view.viewport.GetCurrentZoom());
        FinishThemeOrZoomChange();
        UpdateTitleBar();
        theme_service_.SaveZoomLevel(state_.view.viewport.GetZoomIndex());
        return;
    }

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
