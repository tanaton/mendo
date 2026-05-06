#include "app.h"
#include "app_constants.h"
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

static_assert(app_timer::SEARCH_CARET == SearchBarController::TIMER_CARET);
static_assert(app_timer::SEARCH_DEBOUNCE == SearchBarController::TIMER_DEBOUNCE);
static_assert(app_timer::MERMAID_BATCH == ResourceManager::TIMER_MERMAID_BATCH);
static_assert(app_timer::BITMAP_MANAGE == ResourceManager::TIMER_BITMAP_MANAGE);
static_assert(app_timer::MERMAID_INIT_RETRY == MermaidRenderer::TIMER_INIT_RETRY);

// DWMWA_USE_IMMERSIVE_DARK_MODE は Windows 10 1809 以降の SDK でしか定義されないため、
// 古い SDK でビルドできるようフォールバック定義する。
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

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
    if (!state_.pane_layout_valid) {
        auto* rt = renderer_.GetRenderTarget();
        if (!rt) {
            static const PaneLayout empty{};
            return empty;
        }
        const auto size = rt->GetSize();
        state_.cached_window_width_for_layout = size.width;
        const float tb_h = state_.window.titlebar.GetHeight();
        state_.cached_pane_layout = state_.view.panes.ComputeLayout(size.width, size.height, renderer_.GetTheme().splitter_width, tb_h);
        state_.pane_layout_valid = true;
    }
    return state_.cached_pane_layout;
}

void App::InvalidatePane(const PaneRect& rect) noexcept
{
    const float scale = state_.window.cached_dpi_scale;
    RECT rc;
    rc.left = static_cast<LONG>(rect.x * scale);
    rc.top = static_cast<LONG>(rect.y * scale);
    rc.right = static_cast<LONG>((rect.x + rect.width) * scale) + 1;
    rc.bottom = static_cast<LONG>((rect.y + rect.height) * scale) + 1;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void App::InvalidateTitleBar() noexcept
{
    const float tb_h = state_.window.titlebar.GetHeight();
    if (tb_h <= 0.0f) {
        return;
    }
    // 幅が未計算（初期化直後など）の場合はウィンドウ全体を無効化する
    if (state_.cached_window_width_for_layout <= 0.0f) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    InvalidatePane(PaneRect{ 0.0f, 0.0f, state_.cached_window_width_for_layout, tb_h });
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
    const auto tb = render_composer::BuildTitleBarState(state_, state_.cached_window_width_for_layout, theme_service_.IsDarkMode(), IsZoomed(hwnd_) != FALSE);
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
            renderer_.Render({ state_.document.doc.GetNodes(), state_.document.layout_cache,
                               state_.view.viewport.GetSelection(), layout.md_rect, sp, tb, gs, ts, sb,
                               state_.view.viewport.GetScrollY(), layout_service_->GetTotalHeight(),
                               std::to_underlying(state_.interaction.nav_hover), state_.interaction.hovered,
                               state_.view.nav_history.CanGoBack(), state_.view.nav_history.CanGoForward(),
                               layout_service_->HasDirtyNodes() });
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
        EmitEffect(effect::KillTimer{ app_timer::SWIPE_OVERLAY });
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
    Dispatch(TimerAction{ timer_id });
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
    SaveLastFilePath();
    SavePaneState();
    SaveScrollPosition();
    config_.SaveWString("General", "Language", i18n::GetLangKey());
    // 個別 Save 呼び出しでは write が遅延されるため、終了前に明示 flush で
    // すべての設定値を 1 度のディスク書き込みにまとめる。
    config_.Flush();
    for (UINT_PTR id : {
             app_timer::DEFERRED_LAYOUT,
             app_timer::LOADING_ANIM,
             app_timer::SWIPE_OVERLAY,
             app_timer::TOAST,
             app_timer::SEARCH_CARET,
             app_timer::SEARCH_DEBOUNCE,
             app_timer::TOOLTIP,
             app_timer::BITMAP_MANAGE,
             app_timer::MERMAID_BATCH,
             app_timer::MERMAID_INIT_RETRY,
             app_timer::FILE_RELOAD_DEBOUNCE,
         }) {
        KillTimer(hwnd_, id);
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

void App::SaveLastFilePath()
{
    if (!IsHelpPath(state_.document.doc.GetFilePath())) {
        session_.SaveLastFilePath(state_.document.doc.GetFilePath());
    }
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

void App::SavePaneState()
{
    session_.SavePaneState(state_.view.panes);
}

void App::LoadPaneState()
{
    float client_width = 0.0f;
    if (hwnd_) {
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            client_width = static_cast<float>(rc.right - rc.left);
        }
    }
    session_.LoadPaneState(state_.view.panes, client_width);
}

void App::SaveScrollPosition()
{
    const int node = FindFirstVisibleNode();
    if (node < 0) {
        return;
    }
    // 復元側 (NodeOffsetToScrollY) と同じ cache[node].text_top フィールドを読む。
    // Fenwick PrefixSum 経由 (TextTopOf) は float 加算順が違うためノード数が増えると誤差が累積する。
    const float text_top = state_.document.layout_cache[node].text_top;
    session_.SaveScrollPosition(node, state_.view.viewport.GetScrollY(), text_top);
}
