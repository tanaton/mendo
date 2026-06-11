#include "side_effect_executor.h"
#include "app_side_effect_callbacks.h"
#include "app.h"
#include "file_dialog_service.h"
#include "pane_layout.h"

void AppSideEffectCallbacks::load_file(std::wstring_view path)
{
    app->LoadMarkdownFile(path);
}

void AppSideEffectCallbacks::reload_file()
{
    app->ReloadCurrentFile();
}

void AppSideEffectCallbacks::open_file_dialog()
{
    const auto path = file_dialog_service::OpenMarkdownFileDialog(app->hwnd_);
    if (!path.empty()) {
        if (!app->state_.document.doc.GetFilePath().empty()) {
            PushCurrentNavEntry(app->state_);
        }
        app->LoadMarkdownFile(path);
    }
}

void AppSideEffectCallbacks::invalidate_pane_cache(PaneZone pane)
{
    if (const auto target = ToPaneTarget(pane)) {
        app->renderer_.InvalidateSidePaneCache(*target);
    }
}

void AppSideEffectCallbacks::refresh_pane_layout()
{
    app->RefreshPaneLayout();
}

void AppSideEffectCallbacks::renderer_resize(UINT w, UINT h)
{
    app->renderer_.Resize(w, h);
}

void AppSideEffectCallbacks::renderer_set_dpi(float dpi)
{
    app->renderer_.SetDpi(dpi);
}

void AppSideEffectCallbacks::clear_file_cache()
{
    app->file_cache_.ClearAll();
}

void AppSideEffectCallbacks::perform_resize_end()
{
    app->OnResizeEnd();
}

void AppSideEffectCallbacks::perform_sizing_update()
{
    const auto& sizing_layout = app->GetPaneLayout();
    app->EmitEffect(effect::SyncMaxScroll{ sizing_layout.md_rect.height });
    app->Invalidate();
}

void AppSideEffectCallbacks::apply_theme_change(const effect::ApplyThemeChange& e)
{
    app->HandleApplyThemeChange(e);
}

void AppSideEffectCallbacks::process_deferred_layout()
{
    app->OnDeferredLayout();
}

void AppSideEffectCallbacks::tick_loading_animation()
{
    app->file_load_service_.TickLoadingAnimation();
}

void AppSideEffectCallbacks::process_mermaid_batch_timer()
{
    app->resource_manager_.ProcessMermaidBatch();
}

void AppSideEffectCallbacks::process_bitmap_manage()
{
    app->resource_manager_.OnBitmapManageTimer();
}

void AppSideEffectCallbacks::mermaid_init_retry()
{
    app->mermaid_renderer_.OnInitRetryTimer();
}

void AppSideEffectCallbacks::destroy()
{
    app->OnDestroy();
}

void AppSideEffectCallbacks::handle_parse_complete()
{
    app->OnParseComplete();
}

void AppSideEffectCallbacks::show_context_menu(int x, int y)
{
    app->OnContextMenu(x, y);
}

void AppSideEffectCallbacks::sync_toc_active(bool auto_scroll)
{
    app->SyncTocActiveAndAutoScroll(auto_scroll);
}

void AppSideEffectCallbacks::schedule_bitmap_manage()
{
    app->resource_manager_.ScheduleBitmapManage();
}

void AppSideEffectCallbacks::on_app_image_loaded()
{
    app->resource_manager_.OnAppImageLoaded();
}

template class SideEffectExecutorT<AppSideEffectCallbacks>;
