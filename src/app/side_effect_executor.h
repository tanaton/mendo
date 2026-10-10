#pragma once
#include "side_effect.h"
#include "win32_host.h"
#include "file_watcher.h"
#include "app_state.h"
#include "app_constants.h"
#include "overloaded.h"
#include "ui_constants.h"
#include <utility>

struct SideEffectExecutorDeps {
    IWin32Host* host = nullptr;
    FileWatcher* file_watcher = nullptr;
    AppState* state = nullptr;
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
                if (deps_.state->interaction.tooltip.Update(ev.target)) {
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
            [this](const effect::MermaidIdle&) {
                cb_.mermaid_idle();
            },
        }, e);
        // clang-format on
    }

private:
    SideEffectExecutorDeps deps_{};
    Cb cb_{};
};
