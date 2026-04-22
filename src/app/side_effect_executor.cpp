#include "side_effect_executor.h"
#include "win32_host.h"
#include "resource_manager.h"
#include "document_service.h"
#include "config_service.h"
#include "app_state.h"
#include "layout_service.h"
#include "timer_ids.h"
#include "utility.h"
#include "ui_constants.h"
#include "file_io.h"

void SideEffectExecutor::Init(IWin32Host& host, ResourceManager& resource_manager,
    DocumentService& doc_service, ConfigService& config,
    AppState& state, LayoutService& layout_service, Callbacks cb)
{
    host_ = &host;
    resource_manager_ = &resource_manager;
    doc_service_ = &doc_service;
    config_ = &config;
    state_ = &state;
    layout_service_ = &layout_service;
    cb_ = std::move(cb);
}

void SideEffectExecutor::ExecuteOne(const SideEffect& e)
{
    std::visit(overloaded{
        [this](const effect::InvalidateWindow&) {
            host_->Invalidate();
        },
        [this](const effect::InvalidateTitleBar&) {
            if (!state_ || state_->cached_window_width_for_layout <= 0.0f) {
                host_->Invalidate();
                return;
            }
            const float dpi_scale = state_->window.cached_dpi_scale;
            const int width_px = static_cast<int>(
                state_->cached_window_width_for_layout * dpi_scale) + 1;
            const int height_px = static_cast<int>(
                state_->window.titlebar.GetHeight() * dpi_scale + 0.5f);
            host_->InvalidateTitleBarArea(width_px, height_px);
        },
        [this](const effect::SetTimer& e) {
            host_->SetTimer(e.id, e.ms);
        },
        [this](const effect::KillTimer& e) {
            host_->KillTimer(e.id);
        },
        [this](const effect::SetCapture&) {
            host_->SetCapture();
        },
        [this](const effect::ReleaseCapture&) {
            host_->ReleaseCapture();
        },
        [this](const effect::SetCursor& e) {
            host_->SetCursor(e.type);
        },
        [this](const effect::ClipboardWrite& e) {
            host_->WriteClipboardText(e.text);
        },
        [this](const effect::ClipboardWriteHtml& e) {
            host_->WriteClipboardHtml(e.html, e.plain);
        },
        [this](const effect::ShellOpen& e) {
            host_->ShellOpen(e.url);
        },
        [this](const effect::ShowWindowCmd& e) {
            host_->ShowWindowCmd(e.cmd);
        },
        [this](const effect::PostMessage& e) {
            host_->PostWindowMessage(e.msg, e.wp, e.lp);
        },
        [this](const effect::SetWindowTitle& e) {
            host_->SetWindowTitle(e.title);
        },
        [this](const effect::LoadFile& e) {
            cb_.load_file(e.path);
        },
        [this](const effect::ReloadFile&) {
            cb_.reload_file();
        },
        [this](const effect::OpenFileDialog&) {
            cb_.open_file_dialog();
        },
        [](const effect::SaveFile& e) {
            WriteAllBytes(std::filesystem::path(e.path), e.data.data(), e.data.size());
        },
        [this](const effect::SaveConfig&) {
            config_->Flush();
        },
        [this](const effect::ShowTooltip& e) {
            const POINT screen_pos = host_->ClientToScreen({ e.px, e.py });
            if (state_->interaction.tooltip.Update(e.target, screen_pos.x, screen_pos.y)) {
                host_->SetTimer(app_timer::TOOLTIP, TOOLTIP_DELAY_MS);
            }
            else if (e.target.IsEmpty()) {
                host_->KillTimer(app_timer::TOOLTIP);
            }
        },
        [this](const effect::ClearTooltip&) {
            host_->KillTimer(app_timer::TOOLTIP);
        },
        [this](const effect::ShowToast& e) {
            state_->interaction.toast.Show(e.message);
            host_->SetTimer(app_timer::TOAST, app_timer::FRAME_INTERVAL_MS);
            host_->Invalidate();
        },
        [this](const effect::DeferredLayout&) {
            if (layout_service_->HasDirtyNodes()) {
                host_->SetTimer(app_timer::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS);
            }
        },
        [this](const effect::BitmapManage&) {
            resource_manager_->ScheduleBitmapManage();
        },
        [this](const effect::MermaidBatch&) {
            resource_manager_->ScheduleMermaidBatch();
        },
        [this](const effect::StartFileWatch& e) {
            doc_service_->StartWatching(e.path, [host = host_]() {
                host->KillTimer(app_timer::FILE_RELOAD_DEBOUNCE);
                host->SetTimer(app_timer::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS);
            });
        },
        [this](const effect::StopFileWatch&) {
            doc_service_->StopWatching();
        },
        [this](const effect::ResumeFileWatch&) {
            doc_service_->ResumeWatching();
        },
        [this](const effect::LoadImages&) {
            resource_manager_->LoadImages();
        },
        [this](const effect::RequestMermaidRenders&) {
            resource_manager_->RequestMermaidRenders();
        },
        [this](const effect::CancelMermaidBatch&) {
            resource_manager_->CancelMermaidBatch();
        },
        [this](const effect::InvalidatePaneCache& e) {
            cb_.invalidate_pane_cache(e.pane);
        },
        [this](const effect::RefreshPaneLayout&) {
            cb_.refresh_pane_layout();
        },
        [this](const effect::ApplyDarkMode& e) {
            host_->ApplyDarkMode(e.dark);
        },
        [this](const effect::CheckFileChanges&) {
            doc_service_->CheckForChanges();
        },
        [this](const effect::NotifyImageLoaded&) {
            resource_manager_->OnAppImageLoaded();
        },
        [this](const effect::RendererResize& e) {
            cb_.renderer_resize(e.width, e.height);
        },
        [this](const effect::RendererSetDpi& e) {
            cb_.renderer_set_dpi(e.dpi);
        },
        [this](const effect::SetWindowPosition& e) {
            host_->SetWindowPosition(e.x, e.y, e.cx, e.cy);
        },
        [this](const effect::ClearFileCache&) {
            cb_.clear_file_cache();
        },
        [this](const effect::PerformResizeEnd&) {
            cb_.perform_resize_end();
        },
        [this](const effect::PerformSizingUpdate&) {
            cb_.perform_sizing_update();
        },
        [this](const effect::ApplyThemeChange& e) {
            cb_.apply_theme_change(e);
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
        [this](const effect::Destroy&) {
            cb_.destroy();
        },
        [this](const effect::HandleParseComplete&) {
            cb_.handle_parse_complete();
        },
        [this](const effect::ShowContextMenu& e) {
            cb_.show_context_menu(e.screen_x, e.screen_y);
        },
        }, e);
}

void SideEffectExecutor::Execute(const SideEffectList& effects)
{
    for (const auto& e : effects) {
        ExecuteOne(e);
    }
}
