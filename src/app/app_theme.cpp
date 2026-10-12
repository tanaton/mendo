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

    // ズームは全レイアウトを作り直すので、スクロールを待たずに画面外を捨てる。
    EmitEffect(effect::BitmapManage{});
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
    // メモリキャッシュはキーにダークモードを含むので消さず、往復時に再利用する。
    mermaid_renderer_.CancelPending();
    FinishThemeOrZoomChange();
    theme_service_.SaveDarkMode();
}
