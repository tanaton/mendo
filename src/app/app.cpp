#include "app.h"
#include "app_constants.h"
#include "darkmode_util.h"
#include "render_composer.h"
#include "search_bar_controller.h"
#include "file_loader.h"
#include "parser.h"
#include "resource.h"
#include "i18n.h"
#include "pane_layout.h"
#include "document_utils.h"
#include "mermaid_util.h"
#include "layout.h"
#include "ui_constants.h"
#include "d2d_util.h"
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <variant>
#include <filesystem>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

#include "utility.h"
#include "profiler.h"

void ApplyDarkModeToWindow(HWND hwnd, bool dark)
{
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // エクスプローラーのダークテーマを適用すると非クライアントスクロールバーも
    // ダーク化される（Windows 標準の挙動を借用）。
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

App::DipPoint App::PixelToDip(int px, int py) const noexcept
{
    return { px / state_.window.cached_dpi_scale, py / state_.window.cached_dpi_scale };
}

PaneScrollInfo App::ComputePaneScrollInfo(
    const PaneRect& rect, float total_content) const
{
    return ComputeScrollInfo(rect, renderer_.GetTheme().pane_header_height, total_content);
}

void App::CancelPendingResources()
{
    resource_manager_.CancelMermaidBatch();
    image_loader_.CancelPending();
    resource_manager_.ClearResolvedPaths();
}

void App::ResetViewForNewDocument()
{
    state_.view.viewport.ClearSelection();
    CancelPendingResources();
    renderer_.ShrinkBuffers();
    state_.view.panes.ResetScrollStates();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();
}

void App::FinalizeLayout(float md_pane_height)
{
    resource_manager_.LoadImages();
    resource_manager_.RequestMermaidRenders();
    EmitEffect(effect::SyncMaxScroll{ md_pane_height });
    Invalidate();
    ScheduleDeferredLayoutIfNeeded();
}

const PaneLayout& App::GetPaneLayout()
{
    if (!state_.pane_layout_cache.IsValid()) {
        auto* rt = renderer_.GetRenderTarget();
        if (!rt) {
            static const PaneLayout empty{};
            return empty;
        }
        const auto size = rt->GetSize();
        const float tb_h = state_.window.titlebar.GetHeight();
        state_.pane_layout_cache.Set(
            size.width,
            state_.view.panes.ComputeLayout(size.width, size.height, renderer_.GetTheme().splitter_width, tb_h));
    }
    return state_.pane_layout_cache.Get();
}

void App::InvalidatePane(const PaneRect& rect) noexcept
{
    mendo::InvalidateDipRect(hwnd_, rect.x, rect.y, rect.width, rect.height,
                             state_.window.cached_dpi_scale);
}

PaneZone App::PaneAtPoint(float dip_x)
{
    const auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return PaneZone::None;
    }
    const auto size = rt->GetSize();
    return state_.view.panes.DetectZone(dip_x, size.width, size.height,
                                        renderer_.GetTheme().splitter_width);
}

float App::GetMarkdownPaneWidth()
{
    const auto layout = GetPaneLayout();
    return layout.md_rect.width;
}

void App::OnPaint()
{
    MENDO_PROFILE("OnPaint");

    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    const auto& layout = GetPaneLayout();
    const bool show_loading = file_load_service_.IsLoading() && !state_.pending_reload_retry;
    if (!show_loading) {
        EnsureScrollTarget();

        bool updated;
        {
            MENDO_PROFILE("EnsureVisibleLayout");
            updated = layout_service_->EnsureVisibleLayout(
                state_.document.doc, state_.document.layout_cache, layout.md_rect.width, layout.md_rect.height);
        }

        if (updated) {
            EmitEffect(effect::SyncMaxScroll{ layout.md_rect.height });
        }
    }

    const auto gs = render_composer::BuildGestureState(state_);
    const auto sp = render_composer::BuildSidePaneState(state_, layout);
    const auto tb = render_composer::BuildTitleBarState(state_, state_.pane_layout_cache.WindowWidth(), theme_service_.IsDarkMode(), IsZoomed(hwnd_) != FALSE);
    const auto ts = render_composer::BuildToastState(state_);
    const auto sb = render_composer::BuildSearchBarState(state_);

    if (show_loading) {
        renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, tb, gs, ts);
    }
    else {
        if (state_.search.search_state.IsVisible() && state_.search.search_state.IsHighlightEnabled() && !state_.search.search_state.GetMatches().empty()) {
            renderer_.SetSearchMatches(&state_.search.search_state.GetMatches(),
                                       state_.search.search_state.GetCurrentMatchIndex(),
                                       state_.search.search_state.GetGeneration());
        }
        else {
            renderer_.SetSearchMatches(nullptr, -1, 0);
        }

        // PrepareVisibleEffects は Render の前に実行する必要がある（描画コマンドが
        // 各ノードのエフェクト状態を参照するため）。
        renderer_.PrepareVisibleEffects(
            state_.document.doc.GetNodesMut(), state_.document.layout_cache,
            state_.view.viewport.GetScrollY(), layout.md_rect.height);

        {
            MENDO_PROFILE("Renderer::Render");
            const BlockHScrollContext h_scroll{
                .scroll_x = &state_.view.block_scroll_x,
                .hovered_block = state_.view.hovered_h_block,
                .drag_block = state_.view.h_drag_node,
            };
            renderer_.Render({ state_.document.doc.GetNodes(), state_.document.layout_cache,
                               state_.view.viewport.GetSelection(), layout.md_rect, sp, tb, gs, ts, sb,
                               state_.view.viewport.GetScrollY(), layout_service_->GetTotalHeight(),
                               std::to_underlying(state_.interaction.nav_hover), state_.interaction.hovered,
                               state_.view.nav_history.CanGoBack(), state_.view.nav_history.CanGoForward(),
                               layout_service_->HasDirtyNodes(), h_scroll });
        }
    }

    EndPaint(hwnd_, &ps);
    MENDO_FRAME_MARK();
}

void App::OnResize(UINT width, UINT height)
{
    Dispatch(ResizeAction{ width, height });
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested)
{
    // reducer をプラットフォーム非依存に保つため、Win32 の RECT を PixelRect に詰め替える。
    const PixelRect rc{
        static_cast<int32_t>(suggested->left),
        static_cast<int32_t>(suggested->top),
        static_cast<int32_t>(suggested->right),
        static_cast<int32_t>(suggested->bottom),
    };
    Dispatch(DpiChangedAction{ static_cast<uint32_t>(dpi), rc });
}

void App::OnAppImageLoaded()
{
    Dispatch(ImageLoadedAction{});
}

void App::OnMouseWheel(int px, int py, short delta, bool ctrl)
{
    if (!IsRenderReady()) {
        return;
    }

    if (ctrl) {
        const MouseWheelEvent event{ delta, true, PaneZone::MdPane };
        Dispatch(controller_.HandleMouseWheel(event));
        return;
    }

    // 縦スクロールが発生した時点で SwipeDetector の軸ロックを解除し、
    // 直後の水平ホイールがスワイプとして誤検出されないようにする。
    const bool had_overlay = state_.interaction.swipe_detector.IsOverlayVisible();
    state_.interaction.swipe_detector.NotifyVScroll(GetTickCount64());
    if (had_overlay) {
        EmitEffect(effect::KillTimer{ app_timer::Id::SWIPE_OVERLAY });
        Invalidate();
    }

    const auto dip = PixelToDip(px, py);
    const auto pane_layout = GetPaneLayout();
    const auto zone = DetectPaneZone(dip.x, pane_layout,
                                     renderer_.GetTheme().splitter_width,
                                     state_.view.panes.IsFilePaneVisible(), state_.view.panes.IsTocPaneVisible());

    const MouseWheelEvent event{ delta, false, zone };
    Dispatch(controller_.HandleMouseWheel(event));
}

void App::OnMouseHWheel(short delta)
{
    Dispatch(HWheelAction{ delta, GetTickCount64() });
}

void App::OnKeyDown(WPARAM key)
{
    const KeyDownEvent event{
        static_cast<int>(key),
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0
    };
    Dispatch(controller_.HandleKeyDown(event));
}

void App::Dispatch(const AppAction& action)
{
    GetPaneLayout();
    auto effects = Reduce(state_, action);
    effect_executor_.Execute(effects);
}

void App::OnDropFiles(HDROP hDrop)
{
    const UINT required = DragQueryFileW(hDrop, 0, nullptr, 0);
    if (required > 0) {
        std::pmr::wstring path(required, L'\0');
        if (DragQueryFileW(hDrop, 0, path.data(), required + 1)) {
            Dispatch(DropFilesAction{ std::move(path) });
        }
    }
    DragFinish(hDrop);
}

void App::OnFileWatchEvent()
{
    Dispatch(FileWatchAction{});
}

void App::HandleTimer(UINT_PTR timer_id)
{
    const auto it = std::ranges::find(app_timer::ALL_TIMERS, static_cast<app_timer::Id>(timer_id));
    if (it != std::ranges::end(app_timer::ALL_TIMERS)) {
        Dispatch(TimerAction{ *it });
    }
}

void App::OnAppLoadFile()
{
    DoLoadMarkdownFile();
}

void App::OnAppReloadFile()
{
    DoReloadCurrentFile();
}

void App::OnCaptureChanged()
{
    Dispatch(CaptureChangedAction{});
}

void App::ShowToast(std::wstring_view message)
{
    effect_executor_.ExecuteOne(effect::ShowToast{ std::pmr::wstring{ message } });
}

void App::OnDestroy()
{
    mermaid_renderer_.Shutdown();
    // 走行中タスクが latch.wait 中の参照を保ったまま解放されないよう、
    // LayoutEngine の参照解除 → Shutdown (join) → ターゲット deinit の順を守る。
    renderer_.GetLayout().SetLayoutScheduler(nullptr);
    layout_scheduler_.Shutdown();
    scheduler_.Shutdown();
    file_cache_.Shutdown();
    file_cache_.SaveIndex();
    persistence_.SaveLastFilePath();
    persistence_.SavePaneState();
    persistence_.SaveScrollPosition();
    config_.SaveWString("General", "Language", i18n::GetLangKey());
    // 個別 Save 呼び出しでは write が遅延されるため、終了前に明示 flush で
    // すべての設定値を 1 度のディスク書き込みにまとめる。
    config_.Flush();
    for (app_timer::Id id : app_timer::ALL_TIMERS) {
        KillTimer(hwnd_, std::to_underlying(id));
    }
}

RECT App::GetSearchEditRect()
{
    if (!state_.search.search_state.IsVisible()) {
        return { 0, 0, 1, 1 };
    }
    const auto& layout = GetPaneLayout();
    const auto& r = layout.md_rect;
    const auto sbl = ComputeSearchBarLayout(r.x, r.width, r.y + r.height, !state_.search.search_state.GetQuery().empty());
    const float s = state_.window.cached_dpi_scale;
    return {
        static_cast<LONG>(sbl.input_rect.left * s),
        static_cast<LONG>(sbl.input_rect.top * s),
        static_cast<LONG>(sbl.input_rect.right * s),
        static_cast<LONG>(sbl.input_rect.bottom * s),
    };
}

std::pmr::wstring App::LoadLastFilePath() const
{
    return session_.LoadLastFilePath();
}

void App::ShowDirectory(std::wstring_view dir_path)
{
    state_.file_explorer.SetDirectory(dir_path);
    renderer_.InvalidateFilePaneCache();
    Invalidate();
}
