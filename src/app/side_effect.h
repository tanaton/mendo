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
struct ClipboardWriteHtml { std::pmr::wstring html; std::pmr::wstring plain; };

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
struct ShowToast { std::wstring message; };

// ---- コンテキストメニュー ----
struct ShowContextMenu { int screen_x; int screen_y; };

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

// ---- テーマ・ダークモード ----
struct ApplyDarkMode { bool dark; };

// テーマ/ズーム変更の複合副作用。
// Reducer が SaveAnchor + 状態変更を行った後、executor がレンダラー適用 + レイアウトを実行する。
struct ApplyThemeChange {
    enum class Type { Zoom, DarkMode };
    Type type;
    int anchor_idx;
    float anchor_y_before;
    float anchor_offset;
    float offset_scale;   // Zoom: zoom_ratio, DarkMode: 1.0f
    float new_zoom;       // Zoom 用: 新しいズーム値
    int zoom_index;       // Zoom 用: ズームインデックス（設定保存用）
};

// ノード指定ジャンプ（TOCクリック・戻る/進む同一ファイル）後に発行する補償副作用。
// Reducer で推定 y_position を基準に ScrollTo した後、executor が ViewportLayout で
// 目的地周辺を本計測し、推定と実測の差を AnchorCompensateScroll で吸収する。
struct CompensateScrollAfterLayout {
    int anchor_idx;
    float anchor_y_before;
};

// ---- ファイル監視・リソース ----
struct CheckFileChanges {};
struct NotifyImageLoaded {};
struct ClearFileCache {};

// ---- レンダラー操作 ----
struct RendererResize { UINT width; UINT height; };
struct RendererSetDpi { float dpi; };

// ---- ウィンドウ操作（追加） ----
struct SetWindowPosition { int x; int y; int cx; int cy; };

// ---- リサイズ ----
struct PerformResizeEnd {};
struct PerformSizingUpdate {};

// ---- タイマー処理委譲 ----
struct ProcessDeferredLayout {};
struct TickLoadingAnimation {};
struct ProcessMermaidBatchTimer {};
struct ProcessBitmapManage {};
struct MermaidInitRetry {};

// ---- ライフサイクル ----
struct Destroy {};
struct HandleParseComplete {};

} // namespace effect

using SideEffect = std::variant<
    effect::InvalidateWindow,
    effect::InvalidateTitleBar,
    effect::SetTimer,
    effect::KillTimer,
    effect::SetCapture,
    effect::ReleaseCapture,
    effect::SetCursor,
    effect::ClipboardWrite,
    effect::ClipboardWriteHtml,
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
    effect::ShowContextMenu,
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
    effect::ApplyDarkMode,
    effect::CheckFileChanges,
    effect::NotifyImageLoaded,
    effect::RendererResize,
    effect::RendererSetDpi,
    effect::SetWindowPosition,
    effect::ClearFileCache,
    effect::PerformResizeEnd,
    effect::PerformSizingUpdate,
    effect::ApplyThemeChange,
    effect::CompensateScrollAfterLayout,
    effect::ProcessDeferredLayout,
    effect::TickLoadingAnimation,
    effect::ProcessMermaidBatchTimer,
    effect::ProcessBitmapManage,
    effect::MermaidInitRetry,
    effect::Destroy,
    effect::HandleParseComplete
>;

using SideEffectList = std::pmr::vector<SideEffect>;

// ヘルパー: 副作用リストに特定の副作用型が含まれるかチェック
template<typename T>
bool HasEffect(const SideEffectList& effects) noexcept
{
    return std::ranges::any_of(effects, [](const auto& e) {
        return std::holds_alternative<T>(e);
    });
}
