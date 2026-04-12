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

void SideEffectExecutor::Execute(const SideEffectList& effects)
{
    for (const auto& e : effects) {
        std::visit(overloaded{
            [this](const effect::InvalidateWindow&) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            },
            [this](const effect::InvalidateRect&) { // TODO: 部分 Invalidate 実装
                InvalidateRect(hwnd_, nullptr, FALSE);
            },
            [this](const effect::InvalidateTitleBar&) {
                InvalidateRect(hwnd_, nullptr, FALSE);
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
                POINT screen_pos = { e.px, e.py };
                ClientToScreen(hwnd_, &screen_pos);
                if (state_->tooltip.Update(e.target, screen_pos)) {
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
                state_->toast.Show(e.message);
                ::SetTimer(hwnd_, app_timer::TOAST, 16, nullptr);
                InvalidateRect(hwnd_, nullptr, FALSE);
            },
            [this](const effect::DeferredLayout&) {
                if (layout_service_->HasDirtyNodes()) {
                    ::SetTimer(hwnd_, app_timer::DEFERRED_LAYOUT, 16, nullptr);
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
                    ::SetTimer(hwnd, app_timer::FILE_RELOAD_DEBOUNCE, 200, nullptr);
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
            [this](const effect::ApplyDarkMode& e) {
                ApplyDarkModeToWindow(hwnd_, e.dark);
            },
            }, e);
    }
}
