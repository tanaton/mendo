#include "side_effect_executor.h"
#include "resource_manager.h"
#include "win_handle.h"
#include "utility.h"
#include <shellapi.h>

void SideEffectExecutor::Init(HWND hwnd, ResourceManager& resource_manager) noexcept
{
    hwnd_ = hwnd;
    resource_manager_ = &resource_manager;
}

void SideEffectExecutor::Execute(const SideEffectList& effects)
{
    for (const auto& effect : effects) {
        std::visit(overloaded{
            [this](const effect::InvalidateWindow&) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            },
            [this](const effect::InvalidateRect&) {
                // TODO: 部分 Invalidate 実装
                InvalidateRect(hwnd_, nullptr, FALSE);
            },
            [this](const effect::InvalidateTitleBar&) {
                // タイトルバー領域のみ再描画
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
            [](const effect::SetCursor&) {
                // TODO: カーソル変更実装
            },
            [this](const effect::ClipboardWrite& e) {
                if (e.text.empty() || !OpenClipboard(hwnd_)) {
                    return;
                }
                EmptyClipboard();
                const size_t bytes = (e.text.size() + 1) * sizeof(wchar_t);
                UniqueGlobalMem hMem{ GlobalAlloc(GMEM_MOVEABLE, bytes) };
                if (hMem) {
                    if (auto* dest = static_cast<wchar_t*>(GlobalLock(hMem.get()))) {
                        std::char_traits<wchar_t>::copy(dest, e.text.data(), e.text.size());
                        dest[e.text.size()] = L'\0';
                        GlobalUnlock(hMem.get());
                        if (SetClipboardData(CF_UNICODETEXT, hMem.get())) {
                            hMem.release();
                        }
                    }
                }
                CloseClipboard();
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
            [](const effect::SetWindowTitle&) {
                // TODO
            },
            [](const effect::LoadFile&) {
                // TODO: ファイル読み込みは App が処理
            },
            [](const effect::ReloadFile&) {
                // TODO
            },
            [](const effect::OpenFileDialog&) {
                // TODO
            },
            [](const effect::SaveFile&) {
                // TODO
            },
            [](const effect::SaveConfig&) {
                // TODO
            },
            [](const effect::ShowTooltip&) {
                // TODO
            },
            [](const effect::ClearTooltip&) {
                // TODO
            },
            [](const effect::ShowToast&) {
                // TODO
            },
            [](const effect::DeferredLayout&) {
                // TODO
            },
            [this](const effect::BitmapManage&) {
                resource_manager_->ScheduleBitmapManage();
            },
            [](const effect::MermaidBatch&) {
                // TODO
            },
            [](const effect::StartFileWatch&) {
                // TODO
            },
            [](const effect::StopFileWatch&) {
                // TODO
            },
            [](const effect::ResumeFileWatch&) {
                // TODO
            },
            [](const effect::LoadImages&) {
                // TODO
            },
            [](const effect::RequestMermaidRenders&) {
                // TODO
            },
            [](const effect::CancelMermaidBatch&) {
                // TODO
            },
            [](const effect::ApplyDarkMode&) {
                // TODO
            },
        }, effect);
    }
}
