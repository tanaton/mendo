#pragma once
#include "pane.h"
#include "pane_layout.h"
#include "tooltip.h"
#include <algorithm>
#include <variant>
#include <string>
#include <memory_resource>
#include <windows.h>

// Reducer が返す副作用の型定義。
// 各副作用は「何をすべきか」をデータとして表現し、
// SideEffectExecutor が実際の Win32 API 呼び出しに変換する。

namespace effect {

// ---- 描画 ----
struct InvalidateWindow {};
struct InvalidateRect { PaneRect rect; };
struct InvalidateTitleBar {};

// ---- タイマー ----
struct SetTimer { UINT_PTR id; UINT ms; };
struct KillTimer { UINT_PTR id; };

// ---- マウスキャプチャ ----
struct SetCapture {};
struct ReleaseCapture {};

// ---- カーソル ----
enum class CursorType { Arrow, Hand, IBeam, SizeWE };
struct SetCursor { CursorType type; };

// ---- クリップボード ----
struct ClipboardWrite { std::pmr::wstring text; };

// ---- 外部プログラム ----
struct ShellOpen { std::pmr::wstring url; };

// ---- ウィンドウ操作 ----
struct ShowWindowCmd { int cmd; };
struct PostMessage { UINT msg; WPARAM wp; LPARAM lp; };
struct SetWindowTitle { std::pmr::wstring title; };

// ---- ファイルI/O ----
struct LoadFile { std::pmr::wstring path; };
struct ReloadFile {};
struct OpenFileDialog {};
struct SaveFile { std::pmr::wstring path; std::pmr::vector<uint8_t> data; };

// ---- 設定保存 ----
struct SaveConfig {};

// ---- ツールチップ・トースト ----
struct ShowTooltip { TooltipTarget target; int px; int py; };
struct ClearTooltip {};
struct ShowToast { std::pmr::wstring message; };

// ---- レイアウト ----
struct DeferredLayout {};
struct BitmapManage {};
struct MermaidBatch {};

// ---- ファイル監視 ----
struct StartFileWatch { std::pmr::wstring path; };
struct StopFileWatch {};
struct ResumeFileWatch {};

// ---- リソース ----
struct LoadImages {};
struct RequestMermaidRenders {};
struct CancelMermaidBatch {};

// ---- ペインキャッシュ ----
struct InvalidatePaneCache { PaneZone pane; };
struct RefreshPaneLayout {};

// ---- ダークモード ----
struct ApplyDarkMode { bool dark; };

} // namespace effect

using SideEffect = std::variant <
    effect::InvalidateWindow,
    effect::InvalidateRect,
    effect::InvalidateTitleBar,
    effect::SetTimer,
    effect::KillTimer,
    effect::SetCapture,
    effect::ReleaseCapture,
    effect::SetCursor,
    effect::ClipboardWrite,
    effect::ShellOpen,
    effect::ShowWindowCmd,
    effect::PostMessage,
    effect::SetWindowTitle,
    effect::LoadFile,
    effect::ReloadFile,
    effect::OpenFileDialog,
    effect::SaveFile,
    effect::SaveConfig,
    effect::ShowTooltip,
    effect::ClearTooltip,
    effect::ShowToast,
    effect::DeferredLayout,
    effect::BitmapManage,
    effect::MermaidBatch,
    effect::StartFileWatch,
    effect::StopFileWatch,
    effect::ResumeFileWatch,
    effect::LoadImages,
    effect::RequestMermaidRenders,
    effect::CancelMermaidBatch,
    effect::InvalidatePaneCache,
    effect::RefreshPaneLayout,
    effect::ApplyDarkMode
> ;

using SideEffectList = std::pmr::vector<SideEffect>;

// ヘルパー: 副作用リストに特定の副作用型が含まれるかチェック
template<typename T>
bool HasEffect(const SideEffectList& effects) noexcept
{
    return std::ranges::any_of(effects, [](const auto& e) {
        return std::holds_alternative<T>(e);
    });
}
