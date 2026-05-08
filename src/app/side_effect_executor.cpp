#include "side_effect_executor.h"
#include "win32_host.h"
#include "resource_manager.h"
#include "document_service.h"
#include "app_state.h"
#include "layout.h"
#include "app_constants.h"
#include "overloaded.h"
#include "string_convert.h"
#include "utility.h"
#include "ui_constants.h"

void SideEffectExecutor::Init(
    IWin32Host& host,
    ResourceManager& resource_manager,
    DocumentService& doc_service,
    AppState& state,
    LayoutService& layout_service,
    Callbacks cb)
{
    host_ = &host;
    resource_manager_ = &resource_manager;
    doc_service_ = &doc_service;
    state_ = &state;
    layout_service_ = &layout_service;
    cb_ = std::move(cb);
}

void SideEffectExecutor::ExecuteOne(const SideEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
        [this](const UiEffect& sub) {
            ExecuteUi(sub);
        },
        [this](const WindowEffect& sub) {
            ExecuteWindow(sub);
        },
        [this](const NavigationEffect& sub) {
            ExecuteNavigation(sub);
        },
        [this](const LayoutEffect& sub) {
            ExecuteLayout(sub);
        },
        [this](const ResourceEffect& sub) {
            ExecuteResource(sub);
        },
        [this](const TimerEffect& sub) {
            ExecuteTimer(sub);
        },
        [this](const LifecycleEffect& sub) {
            ExecuteLifecycle(sub);
        },
    }, e);
    // clang-format on
}

void SideEffectExecutor::ExecuteUi(const UiEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
        [this](const effect::InvalidateWindow&) {
            host_->Invalidate();
        },
        [this](const effect::InvalidateTitleBar&) {
            if (!state_ || state_->pane_layout_cache.WindowWidth() <= 0.0f) {
                host_->Invalidate();
                return;
            }
            const float dpi_scale = state_->window.cached_dpi_scale;
            const int width_px = static_cast<int>(
                state_->pane_layout_cache.WindowWidth() * dpi_scale) + 1;
            const int height_px = static_cast<int>(
                state_->window.titlebar.GetHeight() * dpi_scale + 0.5f);
            host_->InvalidateTitleBarArea(width_px, height_px);
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
            // クリップボードは CF_UNICODETEXT (UTF-16) 必須のため UTF-8 → wstring 変換。
            std::pmr::wstring wide_text;
            string_convert::Utf8ToWide(ev.text, wide_text);
            host_->WriteClipboardText(wide_text);
        },
        [this](const effect::ClipboardWriteHtml& ev) {
            std::pmr::wstring wide_html;
            std::pmr::wstring wide_plain;
            string_convert::Utf8ToWide(ev.html, wide_html);
            string_convert::Utf8ToWide(ev.plain, wide_plain);
            host_->WriteClipboardHtml(wide_html, wide_plain);
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
    }, e);
    // clang-format on
}

void SideEffectExecutor::ExecuteWindow(const WindowEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
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
    }, e);
    // clang-format on
}

void SideEffectExecutor::ExecuteNavigation(const NavigationEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
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
    }, e);
    // clang-format on
}

void SideEffectExecutor::ExecuteLayout(const LayoutEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
        [this](const effect::DeferredLayout&) {
            if (layout_service_->HasDirtyNodes()) {
                host_->SetTimer(app_timer::Id::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS);
            }
        },
        [this](const effect::BitmapManage&) {
            resource_manager_->ScheduleBitmapManage();
        },
        [this](const effect::MermaidBatch&) {
            resource_manager_->ScheduleMermaidBatch();
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
            const float total = layout_service_->GetTotalHeight();
            state_->view.cached_total_height = total;
            state_->view.viewport.SyncMaxScroll(total, ev.md_pane_height);
        },
    }, e);
    // clang-format on
}

void SideEffectExecutor::ExecuteResource(const ResourceEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
        [this](const effect::LoadImages&) {
            resource_manager_->LoadImages();
        },
        [this](const effect::RequestMermaidRenders&) {
            resource_manager_->RequestMermaidRenders();
        },
        [this](const effect::CancelMermaidBatch&) {
            resource_manager_->CancelMermaidBatch();
        },
        [this](const effect::NotifyImageLoaded&) {
            resource_manager_->OnAppImageLoaded();
        },
        [this](const effect::ClearFileCache&) {
            cb_.clear_file_cache();
        },
        [this](const effect::StartFileWatch& ev) {
            doc_service_->StartWatching(ev.path, [host = host_]() {
                host->KillTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE);
                host->SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS);
            });
        },
        [this](const effect::StopFileWatch&) {
            doc_service_->StopWatching();
        },
        [this](const effect::ResumeFileWatch&) {
            doc_service_->ResumeWatching();
        },
        [this](const effect::CheckFileChanges&) {
            doc_service_->CheckForChanges();
        },
    }, e);
    // clang-format on
}

void SideEffectExecutor::ExecuteTimer(const TimerEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
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
    }, e);
    // clang-format on
}

void SideEffectExecutor::ExecuteLifecycle(const LifecycleEffect& e)
{
    // clang-format off
    std::visit(mendo::overloaded{
        [this](const effect::Destroy&) {
            cb_.destroy();
        },
        [this](const effect::HandleParseComplete&) {
            cb_.handle_parse_complete();
        },
    }, e);
    // clang-format on
}

void SideEffectExecutor::Execute(const SideEffectList& effects)
{
    for (const auto& e : effects) {
        ExecuteOne(e);
    }
}
