#include "app.h"
#include "app_constants.h"
#include "app_controller.h"
#include "document_utils.h"
#include "gesture_overlay.h"
#include "i18n.h"
#include "pane_layout.h"
#include "profiler.h"
#include "reducer.h"
#include "search_bar_controller.h"
#include "ui_constants.h"
#include <utility>

#pragma comment(lib, "shell32.lib")

DipPoint App::PixelToDip(int px, int py) const noexcept
{
    return ::PixelToDip(px, py, state_.window.cached_dpi_scale);
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

void App::UpdateTitleBar()
{
    const int zoom_percent = static_cast<int>(ZOOM_STEPS[state_.view.viewport.GetZoomIndex()] * 100.0f + 0.5f);
    auto title = BuildTitleString(state_.document.doc.GetFilePath(), zoom_percent);
    if (title == state_.cached_title_text) {
        return;
    }
    SetWindowTextW(hwnd_, title.c_str());
    state_.cached_title_text = std::move(title);
    Invalidate();
}

PaneZone App::PaneAtPoint(float dip_x)
{
    if (!IsRenderReady()) {
        return PaneZone::None;
    }
    return ZoneAt(dip_x, GetPaneLayout());
}

// ホバー/ホイール/クリック系のゾーン判定を 1 箇所に揃え、食い違わないようにする。
PaneZone App::ZoneAt(float dip_x, const PaneLayout& layout) const noexcept
{
    return DetectPaneZone(
        dip_x,
        layout,
        renderer_.GetTheme().splitter_width,
        state_.view.panes.IsSidePaneVisible(PaneTarget::File),
        state_.view.panes.IsSidePaneVisible(PaneTarget::Toc));
}

SidePaneState App::BuildSidePaneState(const PaneLayout& layout) const
{
    const auto& panes = state_.view.panes;
    const bool can_reveal = state_.document.doc.HasBackingFile();
    // GetEntries は未列挙なら列挙するため、非表示のファイルペインでは呼ばない。
    static const std::pmr::vector<FileEntry> kNoEntries;
    const auto make_side_pane = [&panes, can_reveal](PaneTarget t, PaneRect rect) {
        return SidePaneInstance{
            .rect = rect,
            .scroll = panes.SidePaneScroll(t),
            .hovered_index = panes.GetHoveredSideIndex(t),
            .show = panes.IsSidePaneVisible(t),
            .hovered_button = panes.GetSideHoveredButton(t),
            .reveal_enabled = t == PaneTarget::File && can_reveal,
        };
    };
    return SidePaneState{
        .panes = {
                  make_side_pane(PaneTarget::File, layout.file_rect),
                  make_side_pane(PaneTarget::Toc, layout.toc_rect),
                  },
        .file_entries = panes.IsSidePaneVisible(PaneTarget::File) ? state_.file_explorer.GetEntries() : kNoEntries,
        .toc_entries = state_.document.doc.GetToc().GetEntries(),
        .nodes = state_.document.doc.GetNodes(),
        .active_toc_index = state_.view.active_toc_index,
    };
}

TitleBarRenderState App::BuildTitleBarRenderState() const
{
    const auto& tbar = state_.window.titlebar;
    const auto& panes = state_.view.panes;
    return TitleBarRenderState{
        .title_text = state_.cached_title_text,
        .open_file = tbar.GetOpenFileButton(),
        .help = tbar.GetHelpButton(),
        .theme_toggle = tbar.GetThemeToggleButton(),
        .search = tbar.GetSearchButton(),
        .file_toggle = tbar.GetFileToggleButton(),
        .toc_toggle = tbar.GetTocToggleButton(),
        .minimize = tbar.GetMinimizeButton(),
        .maximize = tbar.GetMaximizeButton(),
        .close = tbar.GetCloseButton(),
        .icon_rect = tbar.GetIconRect(),
        .title_text_rect = tbar.GetTitleTextRect(),
        .height = tbar.GetHeight(),
        .window_width = state_.pane_layout_cache.WindowWidth(),
        .hovered_zone = tbar.GetHovered(),
        .is_dark_mode = theme_service_.IsDarkMode(),
        .search_active = state_.search.search_state.IsVisible(),
        .file_pane_visible = panes.IsSidePaneVisible(PaneTarget::File),
        .toc_pane_visible = panes.IsSidePaneVisible(PaneTarget::Toc),
        .is_maximized = IsZoomed(hwnd_) != FALSE,
        .window_active = state_.window.window_active,
    };
}

void App::SyncRendererSearchMatches()
{
    const auto& ss = state_.search.search_state;
    if (ss.IsVisible() && ss.IsHighlightEnabled() && !ss.GetMatches().empty()) {
        renderer_.SetSearchMatches(&ss.GetMatches(), ss.GetCurrentMatchIndex(), ss.GetGeneration());
    }
    else {
        renderer_.SetSearchMatches(nullptr, -1, 0);
    }
}

void App::OnPaint()
{
    MENDO_PROFILE("OnPaint");

    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    const auto& layout = GetPaneLayout();
    const bool show_loading = file_load_service_.IsLoading();
    if (!show_loading) {
        EnsureScrollTarget();

        const bool updated = layout_service_->EnsureVisibleLayout(state_.document.doc, state_.document.layout_cache, layout.md_rect.width, layout.md_rect.height);
        if (updated) {
            SyncMaxScroll(layout.md_rect.height);
            // 未計測領域に入った。スクロール先を先読み計測させ、以降のフレームでの同期計測を減らす。
            ScheduleDeferredLayoutIfNeeded();
        }
    }

    const auto gs = ResolveGestureOverlay(state_.interaction.gesture, state_.interaction.swipe_detector);
    const SidePaneState sp = BuildSidePaneState(layout);
    const TitleBarRenderState tb = BuildTitleBarRenderState();
    const ToastRenderState ts{
        .visible = state_.interaction.toast.IsVisible(),
        .alpha = state_.interaction.toast.GetRenderAlpha(),
        .message = state_.interaction.toast.GetMessage(),
    };

    if (show_loading) {
        renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, tb, gs, ts);
    }
    else {
        SyncRendererSearchMatches();

        // 描画コマンドが各ノードのエフェクト状態を参照するため Render より前に行う。
        renderer_.PrepareVisibleEffects(
            state_.document.doc.GetNodesMut(), state_.document.layout_cache,
            state_.view.viewport.GetScrollY(), layout.md_rect.height);

        const auto sb = state_.search.search_bar_ctrl.BuildRenderState();
        const BlockHScrollContext h_scroll{
            .scroll_x = &state_.view.block_scroll_x,
            .hovered_block = state_.view.hovered_h_block,
            .drag_block = state_.view.h_drag_node,
        };
        renderer_.Render(
            { state_.document.doc.GetNodes(), state_.document.layout_cache,
              state_.view.viewport.GetSelection(), layout.md_rect, sp, tb, gs, ts, sb,
              state_.view.viewport.GetScrollY(), ScrollableContentHeight(),
              state_.interaction.nav_hover, state_.interaction.hovered,
              state_.view.nav_history.CanGoBack(), state_.view.nav_history.CanGoForward(),
              layout_service_->HasDirtyNodes(), h_scroll });
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

void App::OnMermaidDiskLoaded()
{
    resource_manager_.BatchMermaidCompletions([this] { mermaid_renderer_.ProcessDiskLoads(); });
}

void App::OnKeyDown(WPARAM key)
{
    const KeyDownEvent event{
        static_cast<int>(key),
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0
    };
    Dispatch(app_controller::HandleKeyDown(event));
}

void App::Dispatch(const AppAction& action)
{
    // reducer は pane_layout_cache を参照するだけなので、ここで確実に確定させておく。
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
    if (timer_id >= std::to_underlying(app_timer::kFirstTimer) && timer_id <= std::to_underlying(app_timer::kLastTimer)) {
        Dispatch(TimerAction{ static_cast<app_timer::Id>(timer_id) });
    }
}

void App::OnCaptureChanged()
{
    Dispatch(CaptureChangedAction{});
}

void App::ShowToast(std::wstring_view message)
{
    if (message.empty()) {
        return;
    }
    EmitEffect(effect::ShowToast{ std::pmr::wstring{ message } });
}

void App::SaveSession()
{
    // SaveScrollPosition だけガード外に置くと、Help 表示中に終了したとき
    // 前回 LastFilePath に Help の node index が紐付き、次回起動時に誤位置へジャンプする。
    const auto& doc = state_.document.doc;
    if (doc.HasBackingFile()) {
        session_.SaveLastFilePath(doc.GetFilePath());
        const auto& cache = state_.document.layout_cache;
        if (const int node = state_.view.viewport.FindFirstVisibleNode(cache, doc.GetNodes().size()); node >= 0) {
            // 復元側 (NodeOffsetToScrollY) と同じ cache[node].text_top を読む。
            session_.SaveScrollPosition(node, state_.view.viewport.GetScrollY(), cache.Top(static_cast<size_t>(node)));
        }
    }

    const auto& panes = state_.view.panes;
    session_.SavePaneState({
        .show_file = panes.IsSidePaneVisible(PaneTarget::File),
        .show_toc = panes.IsSidePaneVisible(PaneTarget::Toc),
        .file_width = panes.GetSidePaneWidth(PaneTarget::File),
        .toc_width = panes.GetSidePaneWidth(PaneTarget::Toc),
    });

    config_.SaveWString("General", "Language", i18n::GetLangKey());
    // 個別 Save 呼び出しでは write が遅延されるため、終了前に明示 flush で
    // すべての設定値を 1 度のディスク書き込みにまとめる。
    config_.Flush();
}

void App::OnDestroy()
{
    mermaid_renderer_.Shutdown();
    // 走行中タスクが latch.wait 中の参照を保ったまま解放されないよう、
    // LayoutEngine の参照解除 → Shutdown (join) → ターゲット deinit の順を守る。
    renderer_.SetLayoutScheduler(nullptr);
    layout_scheduler_.Shutdown();
    scheduler_.Shutdown();
    file_cache_.Shutdown();
    file_cache_.SaveIndex();

    SaveSession();

    for (UINT_PTR id = std::to_underlying(app_timer::kFirstTimer); id <= std::to_underlying(app_timer::kLastTimer); ++id) {
        KillTimer(hwnd_, id);
    }
}

SearchBarLayout App::ComputeSearchBarLayoutForMd(const PaneRect& md_rect) const
{
    return ComputeSearchBarLayout(
        md_rect.x,
        md_rect.width,
        md_rect.y + md_rect.height,
        !state_.search.search_state.GetQuery().empty());
}

int App::HitTestSearchInputPos(const SearchBarLayout& sbl, std::wstring_view query_wide, float dip_x) const
{
    return renderer_.HitTestSearchInput(query_wide, dip_x - sbl.text_left(), sbl.text_width());
}

RECT App::GetSearchEditRect()
{
    if (!state_.search.search_state.IsVisible()) {
        return { 0, 0, 1, 1 };
    }
    const auto sbl = ComputeSearchBarLayoutForMd(GetPaneLayout().md_rect);
    const float s = state_.window.cached_dpi_scale;
    return {
        DipToPixel(sbl.input_rect.left, s),
        DipToPixel(sbl.input_rect.top, s),
        DipToPixel(sbl.input_rect.right, s),
        DipToPixel(sbl.input_rect.bottom, s),
    };
}

void App::SetInitialDirectory(std::wstring_view dir_path)
{
    state_.file_explorer.SetDirectory(dir_path);
}
