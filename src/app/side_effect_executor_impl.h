#pragma once
#include "side_effect_executor.h"
#include "win32_host.h"
#include "file_watcher.h"
#include "app_state.h"
#include "layout.h"
#include "app_constants.h"
#include "overloaded.h"
#include "utility.h"
#include "ui_constants.h"

template <class Cb>
void SideEffectExecutorT<Cb>::Init(
    IWin32Host& host,
    FileWatcher& file_watcher,
    AppState& state,
    LayoutService& layout_service,
    Cb cb)
{
    host_ = &host;
    file_watcher_ = &file_watcher;
    state_ = &state;
    layout_service_ = &layout_service;
    cb_ = std::move(cb);
}

template <class Cb>
void SideEffectExecutorT<Cb>::ExecuteOne(const SideEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
        // ---- Ui ----
        [this](const effect::InvalidateWindow&) {
            host_->Invalidate();
        },
        [this](const effect::InvalidateTitleBar&) {
            if (!state_ || state_->pane_layout_cache.WindowWidth() <= 0.0f) {
                host_->Invalidate();
                return;
            }
            host_->InvalidateTitleBarArea(
                state_->pane_layout_cache.WindowWidth(),
                state_->window.titlebar.GetHeight(),
                state_->window.cached_dpi_scale);
        },
        [this](const effect::InvalidateMdPane&) {
            // layout キャッシュ未確立時はフルウィンドウ無効化にフォールバック。
            if (!state_ || !state_->pane_layout_cache.IsValid()) {
                host_->Invalidate();
                return;
            }
            const auto& md_rect = state_->pane_layout_cache.Get().md_rect;
            host_->InvalidateMdPaneArea(
                md_rect.x, md_rect.y, md_rect.width, md_rect.height,
                state_->window.cached_dpi_scale);
        },
        [this](const effect::SetCapture&) {
            host_->SetCapture();
        },
        [this](const effect::ReleaseCapture&) {
            host_->ReleaseCapture();
        },
        [this](const effect::SetCursor& ev) {
            host_->SetCursor(ev.type);
        },
        [this](const effect::ClipboardWrite& ev) {
            host_->WriteClipboardText(ev.text);
        },
        [this](const effect::ClipboardWriteHtml& ev) {
            host_->WriteClipboardHtml(ev.html, ev.plain);
        },
        [this](const effect::ShowTooltip& ev) {
            const POINT screen_pos = host_->ClientToScreen({ ev.px, ev.py });
            if (state_->interaction.tooltip.Update(ev.target, screen_pos.x, screen_pos.y)) {
                host_->SetTimer(app_timer::Id::TOOLTIP, TOOLTIP_DELAY_MS);
            }
            else if (ev.target.IsEmpty()) {
                host_->KillTimer(app_timer::Id::TOOLTIP);
            }
        },
        [this](const effect::ClearTooltip&) {
            host_->KillTimer(app_timer::Id::TOOLTIP);
        },
        [this](const effect::ShowToast& ev) {
            state_->interaction.toast.Show(ev.message);
            host_->SetTimer(app_timer::Id::TOAST, app_timer::FRAME_INTERVAL_MS);
            host_->Invalidate();
        },
        [this](const effect::ShowContextMenu& ev) {
            cb_.show_context_menu(ev.screen_x, ev.screen_y);
        },
        // ---- Window ----
        [this](const effect::ShowWindowCmd& ev) {
            host_->ShowWindowCmd(ev.cmd);
        },
        [this](const effect::PostWindowMessage& ev) {
            host_->PostWindowMessage(ev.msg, ev.wp, ev.lp);
        },
        [this](const effect::SearchFocus& ev) {
            host_->SearchFocus(ev);
        },
        [this](const effect::SearchUnfocus& ev) {
            host_->SearchUnfocus(ev);
        },
        [this](const effect::SetWindowTitle& ev) {
            host_->SetWindowTitle(ev.title);
        },
        [this](const effect::SetWindowPosition& ev) {
            host_->SetWindowPosition(ev.x, ev.y, ev.cx, ev.cy);
        },
        [this](const effect::ApplyDarkMode& ev) {
            host_->ApplyDarkMode(ev.dark);
        },
        [this](const effect::ApplyThemeChange& ev) {
            cb_.apply_theme_change(ev);
        },
        [this](const effect::PerformResizeEnd&) {
            cb_.perform_resize_end();
        },
        [this](const effect::PerformSizingUpdate&) {
            cb_.perform_sizing_update();
        },
        [this](const effect::RendererResize& ev) {
            cb_.renderer_resize(ev.width, ev.height);
        },
        [this](const effect::RendererSetDpi& ev) {
            cb_.renderer_set_dpi(ev.dpi);
        },
        // ---- Navigation ----
        [this](const effect::ShellOpen& ev) {
            host_->ShellOpen(ev.url);
        },
        [this](const effect::LoadFile& ev) {
            cb_.load_file(ev.path);
        },
        [this](const effect::ReloadFile&) {
            cb_.reload_file();
        },
        [this](const effect::OpenFileDialog&) {
            cb_.open_file_dialog();
        },
        // ---- Layout ----
        [this](const effect::DeferredLayout&) {
            if (layout_service_->HasDirtyNodes()) {
                host_->SetTimer(app_timer::Id::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS);
            }
        },
        [this](const effect::BitmapManage&) {
            cb_.schedule_bitmap_manage();
        },
        [this](const effect::MermaidBatch&) {
            cb_.schedule_mermaid_batch();
        },
        [this](const effect::InvalidatePaneCache& ev) {
            cb_.invalidate_pane_cache(ev.pane);
        },
        [this](const effect::RefreshPaneLayout&) {
            cb_.refresh_pane_layout();
        },
        [this](const effect::SyncTocActive&) {
            cb_.sync_toc_active();
        },
        [this](const effect::ViewportLayout& ev) {
            layout_service_->ViewportLayout(
                state_->document.doc,
                state_->document.layout_cache,
                ev.md_width,
                ev.md_height
            );
        },
        [this](const effect::SyncMaxScroll& ev) {
            state_->view.viewport.SyncMaxScroll(layout_service_->GetTotalHeight(), ev.md_pane_height);
        },
        // ---- Resource ----
        [this](const effect::LoadImages&) {
            cb_.load_images();
        },
        [this](const effect::RequestMermaidRenders&) {
            cb_.request_mermaid_renders();
        },
        [this](const effect::CancelMermaidBatch&) {
            cb_.cancel_mermaid_batch();
        },
        [this](const effect::NotifyImageLoaded&) {
            cb_.on_app_image_loaded();
        },
        [this](const effect::ClearFileCache&) {
            cb_.clear_file_cache();
        },
        [this](const effect::StartFileWatch& ev) {
            file_watcher_->StartWatching(ev.path, [host = host_]() {
                host->KillTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE);
                host->SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS);
            });
        },
        [this](const effect::StopFileWatch&) {
            file_watcher_->StopWatching();
        },
        [this](const effect::ResumeFileWatch&) {
            file_watcher_->ResumeWatching();
        },
        [this](const effect::CheckFileChanges&) {
            file_watcher_->CheckForChanges();
        },
        // ---- Timer ----
        [this](const effect::SetTimer& ev) {
            host_->SetTimer(ev.id, ev.ms);
        },
        [this](const effect::KillTimer& ev) {
            host_->KillTimer(ev.id);
        },
        [this](const effect::ProcessDeferredLayout&) {
            cb_.process_deferred_layout();
        },
        [this](const effect::TickLoadingAnimation&) {
            cb_.tick_loading_animation();
        },
        [this](const effect::ProcessMermaidBatchTimer&) {
            cb_.process_mermaid_batch_timer();
        },
        [this](const effect::ProcessBitmapManage&) {
            cb_.process_bitmap_manage();
        },
        [this](const effect::MermaidInitRetry&) {
            cb_.mermaid_init_retry();
        },
        // ---- Lifecycle ----
        [this](const effect::Destroy&) {
            cb_.destroy();
        },
        [this](const effect::HandleParseComplete&) {
            cb_.handle_parse_complete();
        },
    }, e);
    // clang-format on
}

template <class Cb>
void SideEffectExecutorT<Cb>::Execute(const SideEffectList& effects)
{
    for (const auto& e : effects) {
        ExecuteOne(e);
    }
}
