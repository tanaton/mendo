#include "app_callbacks.h"
#include "app.h"
#include "file_dialog_service.h"
#include "pane_layout.h"

void AppCallbacksBase::set_timer(app_timer::Id id, UINT ms)
{
    app->win32_host_.SetTimer(id, ms);
}

void AppCallbacksBase::kill_timer(app_timer::Id id)
{
    app->win32_host_.KillTimer(id);
}

float AppCallbacksBase::get_md_pane_height()
{
    return app->GetPaneLayout().md_rect.height;
}

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
        PushCurrentNavEntry(app->state_, path);
        app->LoadMarkdownFile(path);
    }
}

void AppSideEffectCallbacks::cancel_other_file_load()
{
    app->CancelOtherFileLoad();
}

void AppSideEffectCallbacks::renderer_resize(UINT w, UINT h)
{
    app->renderer_.Resize(w, h);
}

void AppSideEffectCallbacks::renderer_set_dpi(float dpi)
{
    app->renderer_.SetDpi(dpi);
}

void AppSideEffectCallbacks::perform_resize_end()
{
    app->OnResizeEnd();
}

void AppSideEffectCallbacks::perform_sizing_update()
{
    app->SyncMaxScroll(get_md_pane_height());
    app->Invalidate();
}

void AppSideEffectCallbacks::apply_theme_change(const effect::ApplyThemeChange& e)
{
    app->HandleApplyThemeChange(e);
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

template class SideEffectExecutorT<AppSideEffectCallbacks>;

float AppResourceManagerCallbacks::get_content_width()
{
    return app->MdContentWidth();
}

const Theme& AppResourceManagerCallbacks::get_theme()
{
    return app->renderer_.GetTheme();
}

void AppResourceManagerCallbacks::recompute_layout(layout::HeightChangeRange changed)
{
    app->layout_service_->RecomputeAfterDiagram(
        app->state_.document.doc,
        app->state_.document.layout_cache,
        app->renderer_.GetTheme(),
        changed);
    // リフローで停止中のカーソル直下のノード/リンクが変わりうるため、ホバーの
    // 同一座標スキップを解除し次のマウス移動で再評価させる (カーソル形状の陳腐化対策)。
    app->InvalidateHitPositions();
    app->Invalidate();
}

void AppResourceManagerCallbacks::recompute_layout_anchored(layout::HeightChangeRange changed)
{
    app->EnsureScrollTarget();
    recompute_layout(changed);
    app->SyncMaxScroll(get_md_pane_height());
}

template class ResourceManagerT<AppResourceManagerCallbacks>;

void AppSearchBarCallbacks::invalidate()
{
    app->Invalidate();
}

void AppSearchBarCallbacks::focus_select_all()
{
    app->EmitEffect(effect::SearchFocus{});
}

void AppSearchBarCallbacks::unfocus()
{
    app->EmitEffect(effect::SearchUnfocus{});
}

void AppSearchBarCallbacks::ensure_node_layout(int node_index)
{
    app->layout_service_->EnsureNodeLayout(
        app->state_.document.doc, app->state_.document.layout_cache,
        app->GetPaneLayout().md_rect.width, node_index);
}

void AppSearchBarCallbacks::on_wrap_around()
{
    MessageBeep(MB_OK);
}

template class SearchBarControllerT<AppSearchBarCallbacks>;
