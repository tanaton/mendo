#include "side_effect_executor.h"
#include "resource_manager.h"
#include "cursor_manager.h"
#include "document_service.h"
#include "config_service.h"
#include "app_state.h"
#include "layout_service.h"
#include "app_constants.h"
#include "win_handle.h"
#include "utility.h"
#include "ui_constants.h"
#include "file_io.h"
#include <shellapi.h>

// app.h のインクルードは循環依存を招くため前方宣言で代替
void ApplyDarkModeToWindow(HWND hwnd, bool dark);

void SideEffectExecutor::Init(HWND hwnd, ResourceManager& resource_manager,
    CursorManager& cursors, DocumentService& doc_service,
    ConfigService& config, AppState& state,
    LayoutService& layout_service, Callbacks cb)
{
    hwnd_ = hwnd;
    resource_manager_ = &resource_manager;
    cursors_ = &cursors;
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
            InvalidateRect(hwnd_, nullptr, FALSE);
        },
        [this](const effect::InvalidateTitleBar&) {
            if (!state_) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            RECT client;
            GetClientRect(hwnd_, &client);
            const float dpi_scale = state_->window.cached_dpi_scale;
            RECT tb_rect{ 0, 0, client.right,
                static_cast<LONG>(state_->window.titlebar.GetHeight() * dpi_scale + 0.5f) };
            InvalidateRect(hwnd_, &tb_rect, FALSE);
        },
        [this](const effect::SetTimer& e) {
            ::SetTimer(hwnd_, e.id, e.ms, nullptr);
        },
        [this](const effect::KillTimer& e) {
            ::KillTimer(hwnd_, e.id);
        },
        [this](const effect::SetCapture&) {
            ::SetCapture(hwnd_);
        },
        [this](const effect::ReleaseCapture&) {
            ::ReleaseCapture();
        },
        [this](const effect::SetCursor& e) {
            HCURSOR cursor = nullptr;
            switch (e.type) {
            case effect::CursorType::Arrow:
                cursor = cursors_->Arrow();
                break;
            case effect::CursorType::Hand:
                cursor = cursors_->Hand();
                break;
            case effect::CursorType::IBeam:
                cursor = cursors_->IBeam();
                break;
            case effect::CursorType::SizeWE:
                cursor = cursors_->SizeWE();
                break;
            }
            if (cursor) {
                ::SetCursor(cursor);
            }
        },
        [this](const effect::ClipboardWrite& e) {
            WriteClipboardText(hwnd_, e.text);
        },
        [this](const effect::ClipboardWriteHtml& e) {
            WriteClipboardHtml(hwnd_, e.html, e.plain);
        },
        [](const effect::ShellOpen& e) {
            ShellExecuteW(nullptr, L"open", e.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        },
        [this](const effect::ShowWindowCmd& e) {
            ShowWindow(hwnd_, e.cmd);
        },
        [this](const effect::PostMessage& e) {
            PostMessageW(hwnd_, e.msg, e.wp, e.lp);
        },
        [this](const effect::SetWindowTitle& e) {
            SetWindowTextW(hwnd_, e.title.c_str());
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
            POINT screen_pos{ e.px, e.py };
            ClientToScreen(hwnd_, &screen_pos);
            if (state_->interaction.tooltip.Update(e.target, screen_pos.x, screen_pos.y)) {
                ::SetTimer(hwnd_, app_timer::TOOLTIP, TOOLTIP_DELAY_MS, nullptr);
            }
            else if (e.target.IsEmpty()) {
                ::KillTimer(hwnd_, app_timer::TOOLTIP);
            }
        },
        [this](const effect::ClearTooltip&) {
            ::KillTimer(hwnd_, app_timer::TOOLTIP);
        },
        [this](const effect::ShowToast& e) {
            state_->interaction.toast.Show(e.message);
            ::SetTimer(hwnd_, app_timer::TOAST, app_timer::FRAME_INTERVAL_MS, nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);
        },
        [this](const effect::DeferredLayout&) {
            if (layout_service_->HasDirtyNodes()) {
                ::SetTimer(hwnd_, app_timer::DEFERRED_LAYOUT, app_timer::FRAME_INTERVAL_MS, nullptr);
            }
        },
        [this](const effect::BitmapManage&) {
            resource_manager_->ScheduleBitmapManage();
        },
        [this](const effect::MermaidBatch&) {
            resource_manager_->ScheduleMermaidBatch();
        },
        [this](const effect::StartFileWatch& e) {
            doc_service_->StartWatching(e.path, [hwnd = hwnd_]() {
                ::KillTimer(hwnd, app_timer::FILE_RELOAD_DEBOUNCE);
                ::SetTimer(hwnd, app_timer::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS, nullptr);
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
            ApplyDarkModeToWindow(hwnd_, e.dark);
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
            SetWindowPos(hwnd_, nullptr, e.x, e.y, e.cx, e.cy, SWP_NOZORDER | SWP_NOACTIVATE);
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
