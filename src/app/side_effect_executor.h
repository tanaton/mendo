#pragma once
#include "side_effect.h"
#include "win32_host.h"
#include "file_watcher.h"
#include "app_state.h"
#include "layout.h"
#include "app_constants.h"
#include "overloaded.h"
#include "utility.h"
#include "ui_constants.h"
#include <string_view>

class LayoutService;

struct SideEffectExecutorDeps {
    IWin32Host* host = nullptr;
    FileWatcher* file_watcher = nullptr;
    AppState* state = nullptr;
    LayoutService* layout_service = nullptr;
};

template <class Cb>
class SideEffectExecutorT {
public:
    constexpr void Init(const SideEffectExecutorDeps& deps, Cb cb) noexcept
    {
        deps_ = deps;
        cb_ = std::move(cb);
    }

    void Execute(const SideEffectList& effects)
    {
        for (const auto& e : effects) {
            ExecuteOne(e);
        }
    }

    void ExecuteOne(const SideEffect& e)
    {
        // clang-format off
        std::visit(mendo::overloaded{
            // ---- Ui ----
            [this](const effect::InvalidateWindow&) {
                deps_.host->Invalidate();
            },
            [this](const effect::InvalidateTitleBar&) {
                if (!deps_.state || deps_.state->pane_layout_cache.WindowWidth() <= 0.0f) {
                    deps_.host->Invalidate();
                    return;
                }
                deps_.host->InvalidateTitleBarArea(
                    deps_.state->pane_layout_cache.WindowWidth(),
                    deps_.state->window.titlebar.GetHeight(),
                    deps_.state->window.cached_dpi_scale);
            },
            [this](const effect::InvalidateMdPane&) {
                if (!deps_.state || !deps_.state->pane_layout_cache.IsValid()) {
                    deps_.host->Invalidate();
                    return;
                }
                const auto& md_rect = deps_.state->pane_layout_cache.Get().md_rect;
                deps_.host->InvalidateMdPaneArea(
                    md_rect.x, md_rect.y, md_rect.width, md_rect.height,
                    deps_.state->window.cached_dpi_scale);
            },
            [this](const effect::SetCapture&) {
                deps_.host->SetCapture();
            },
            [this](const effect::ReleaseCapture&) {
                deps_.host->ReleaseCapture();
            },
            [this](const effect::ClipboardWrite& ev) {
                deps_.host->WriteClipboardText(ev.text);
            },
            [this](const effect::ClipboardWriteHtml& ev) {
                deps_.host->WriteClipboardHtml(ev.html, ev.plain);
            },
            [this](const effect::ShowTooltip& ev) {
                const POINT screen_pos = deps_.host->ClientToScreen({ ev.px, ev.py });
                if (deps_.state->interaction.tooltip.Update(ev.target, screen_pos.x, screen_pos.y)) {
                    deps_.host->SetTimer(app_timer::Id::TOOLTIP, TOOLTIP_DELAY_MS);
                }
                else if (ev.target.IsEmpty()) {
                    deps_.host->KillTimer(app_timer::Id::TOOLTIP);
                }
            },
            [this](const effect::ClearTooltip&) {
                deps_.host->KillTimer(app_timer::Id::TOOLTIP);
            },
            [this](const effect::ShowToast& ev) {
                deps_.state->interaction.toast.Show(ev.message);
                deps_.host->SetTimer(app_timer::Id::TOAST, app_timer::FRAME_INTERVAL_MS);
                deps_.host->Invalidate();
            },
            [this](const effect::ShowContextMenu& ev) {
                cb_.show_context_menu(ev.screen_x, ev.screen_y);
            },
            // ---- Window ----
            [this](const effect::SearchFocus& ev) {
                deps_.host->SearchFocus(ev);
            },
            [this](const effect::SearchUnfocus& ev) {
                deps_.host->SearchUnfocus(ev);
            },
            [this](const effect::SetWindowPosition& ev) {
                deps_.host->SetWindowPosition(ev.x, ev.y, ev.cx, ev.cy);
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
                deps_.host->ShellOpen(ev.url);
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
            [this](const effect::BitmapManage&) {
                cb_.schedule_bitmap_manage();
            },
            [this](const effect::InvalidatePaneCache& ev) {
                cb_.invalidate_pane_cache(ev.pane);
            },
            [this](const effect::RefreshPaneLayout&) {
                cb_.refresh_pane_layout();
            },
            [this](const effect::SyncTocActive& ev) {
                cb_.sync_toc_active(ev.auto_scroll);
            },
            [this](const effect::ViewportLayout& ev) {
                deps_.layout_service->ViewportLayout(
                    deps_.state->document.doc,
                    deps_.state->document.layout_cache,
                    ev.md_width,
                    ev.md_height
                );
            },
            [this](const effect::SyncMaxScroll& ev) {
                const auto& ds = deps_.state->document;
                deps_.state->view.viewport.SyncMaxScroll(
                    deps_.layout_service->GetScrollableContentHeight(ds.doc, ds.layout_cache),
                    ev.md_pane_height);
            },
            // ---- Resource ----
            [this](const effect::NotifyImageLoaded&) {
                cb_.on_app_image_loaded();
            },
            [this](const effect::ClearFileCache&) {
                cb_.clear_file_cache();
            },
            [this](const effect::StartFileWatch& ev) {
                deps_.file_watcher->StartWatching(ev.path, [host = deps_.host]() {
                    host->KillTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE);
                    host->SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS);
                });
            },
            [this](const effect::StopFileWatch&) {
                deps_.file_watcher->StopWatching();
            },
            [this](const effect::ResumeFileWatch&) {
                deps_.file_watcher->ResumeWatching();
            },
            [this](const effect::CheckFileChanges&) {
                deps_.file_watcher->CheckForChanges();
            },
            // ---- Timer ----
            [this](const effect::SetTimer& ev) {
                deps_.host->SetTimer(ev.id, ev.ms);
            },
            [this](const effect::KillTimer& ev) {
                deps_.host->KillTimer(ev.id);
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

private:
    SideEffectExecutorDeps deps_{};
    Cb cb_{};
};
