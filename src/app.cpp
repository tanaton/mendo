#include "app.h"
#include "parser.h"
#include "resource.h"
#include "pane_layout.h"
#include "document_utils.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <variant>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 1809以降 / Windows 11でサポート)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#include "utility.h"

void ApplyDarkModeToWindow(HWND hwnd, bool dark)
{
    // ダークタイトルバー
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));

    // エクスプローラーテーマによるダークスクロールバー
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// ============================================================
// 初期化
// ============================================================

bool App::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    if (!renderer_.Init(hwnd_)) {
        return false;
    }

    layout_service_.emplace(renderer_.GetLayout(), viewport_);

    // PixelToDip用にDPIスケールをキャッシュ (OnDpiChangedで更新)
    float init_dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    cached_dpi_scale_ = (init_dpi > 0.0f) ? (init_dpi / 96.0f) : 1.0f;

    // Mermaidレンダラーを初期化 (WebView2、非同期)
    mermaid_renderer_.Init(hwnd_, renderer_.GetRenderTarget(), [this]() {
        RequestMermaidRenders();
    });

    // D2Dデバイスロスト時にレンダーターゲットが再作成されたら、MermaidRendererを更新
    renderer_.SetDeviceLostCallback([this](ID2D1RenderTarget* new_rt) {
        mermaid_renderer_.SetRenderTarget(new_rt);
    });

    // 保存済みのダークモードとズーム設定を適用
    theme_service_.LoadDarkMode();
    viewport_.SetZoomIndex(theme_service_.LoadZoomIndex());
    if (theme_service_.IsDarkMode() || viewport_.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
        renderer_.SetTheme(theme_service_.CreateTheme(viewport_.GetZoomIndex()));
        if (viewport_.GetZoomIndex() != ZOOM_DEFAULT_INDEX) {
            panes_.ApplyZoom(viewport_.GetCurrentZoom());
        }
    }
    if (theme_service_.IsDarkMode()) {
        ApplyDarkModeToWindow(hwnd_, true);
    }

    // システムカーソルをキャッシュ
    cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
    cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
    cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
    cursor_sizewe_ = LoadCursorW(nullptr, IDC_SIZEWE);

    // タイトルバーのレイアウト初期化
    {
        auto* rt = renderer_.GetRenderTarget();
        float window_w = rt ? rt->GetSize().width : 1600.0f;
        titlebar_.UpdateLayout(window_w);
    }

    LoadPaneState();

    // ファイル監視タイマーを設定 (250ms毎にチェック)
    SetTimer(hwnd_, TIMER_FILE_WATCH, 250, nullptr);

    return true;
}

// ============================================================
// ヘルパー
// ============================================================

App::DipPoint App::PixelToDip(int px, int py) const
{
    return { px / cached_dpi_scale_, py / cached_dpi_scale_ };
}

PaneScrollInfo App::ComputePaneScrollInfo(
    const PaneRect& rect, float total_content) const
{
    return ComputeScrollInfo(rect, renderer_.GetTheme().pane_header_height, total_content);
}

void App::HandleScrollbarClick(float dip_y, const PaneScrollInfo& info,
    ScrollState& scroll, bool& cache_dirty)
{
    float thumb_y = ComputeThumbY(info, scroll.scroll_y);

    if (dip_y >= thumb_y && dip_y <= thumb_y + info.thumb_height) {
        panes_.SetDragScrollOffset(dip_y - thumb_y);
    }
    else {
        panes_.SetDragScrollOffset(info.thumb_height * 0.5f);
        float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
        scroll.scroll_y = ScrollFromThumbY(info, new_thumb_y);
        scroll.max_scroll = info.max_scroll;
        cache_dirty = true;
        Invalidate();
    }
}

void App::HandleScrollbarDrag(float dip_y, const PaneScrollInfo& info,
    ScrollState& scroll, bool& cache_dirty)
{
    float new_thumb_y = dip_y - panes_.GetDragScrollOffset();
    scroll.scroll_y = ScrollFromThumbY(info, new_thumb_y);
    scroll.max_scroll = info.max_scroll;
    cache_dirty = true;
    Invalidate();
}

// ============================================================
// カスタムタイトルバー
// ============================================================

void App::OnActivate(bool active)
{
    if (window_active_ != active) {
        window_active_ = active;
        Invalidate();
    }
}

// ============================================================
// ペインレイアウト
// ============================================================

PaneLayout App::GetPaneLayout() const
{
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return {};
    }

    auto size = rt->GetSize();
    float tb_h = titlebar_.GetHeight();
    return panes_.ComputeLayout(size.width, size.height,
        renderer_.GetTheme().splitter_width, tb_h);
}

PaneZone App::PaneAtPoint(float dip_x, [[maybe_unused]] float dip_y) const
{
    auto* rt = renderer_.GetRenderTarget();
    if (!rt) {
        return PaneZone::None;
    }
    auto size = rt->GetSize();
    return panes_.DetectZone(dip_x, size.width, size.height,
        renderer_.GetTheme().splitter_width);
}

float App::GetMarkdownPaneWidth() const
{
    auto layout = GetPaneLayout();
    return layout.md_rect.width;
}

// ============================================================
// OnPaint用のレンダーステート構築ヘルパー
// ============================================================

GestureRenderState App::BuildGestureRenderState() const
{
    GestureRenderState gs;
    gs.trail_active = gesture_.IsGestureActive();
    gs.trail_points = &gesture_.GetTrailPoints();
    gs.overlay_visible = gesture_.IsOverlayVisible();
    gs.direction = (gesture_.GetDirection() == GestureDirection::Left) ? -1
        : (gesture_.GetDirection() == GestureDirection::Right) ? 1 : 0;
    gs.overlay_alpha = gesture_.GetOverlayAlpha();

    // タッチパッドスワイプのオーバーレイ（マウスジェスチャーが非アクティブの場合のみ）
    if (!gs.overlay_visible && swipe_detector_.IsOverlayVisible()) {
        gs.overlay_visible = true;
        gs.direction = swipe_detector_.GetOverlayDirection();
        gs.overlay_alpha = swipe_detector_.GetOverlayAlpha();
    }
    return gs;
}

SidePaneState App::BuildSidePaneState(const ::PaneLayout& layout) const
{
    return { layout.file_rect, layout.toc_rect,
             file_explorer_.GetEntries(), panes_.FileScroll(), panes_.GetHoveredFileIndex(),
             doc_.GetToc().GetEntries(), panes_.TocScroll(), panes_.GetHoveredTocIndex(),
             panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible(),
             panes_.IsFileCloseHovered(), panes_.IsFileRefreshHovered(),
             panes_.IsTocCloseHovered() };
}

TitleBarRenderState App::BuildTitleBarRenderState() const
{
    auto* rt = renderer_.GetRenderTarget();
    float window_w = rt ? rt->GetSize().width : 0.0f;
    TitleBarRenderState tb;
    tb.height = titlebar_.GetHeight();
    tb.window_width = window_w;
    tb.file_btn_rect = titlebar_.GetFileToggleButton().rect;
    tb.file_btn_hovered = titlebar_.GetFileToggleButton().hovered;
    tb.file_pane_visible = panes_.IsFilePaneVisible();
    tb.toc_btn_rect = titlebar_.GetTocToggleButton().rect;
    tb.toc_btn_hovered = titlebar_.GetTocToggleButton().hovered;
    tb.toc_pane_visible = panes_.IsTocPaneVisible();
    tb.minimize_btn_rect = titlebar_.GetMinimizeButton().rect;
    tb.minimize_btn_hovered = titlebar_.GetMinimizeButton().hovered;
    tb.maximize_btn_rect = titlebar_.GetMaximizeButton().rect;
    tb.maximize_btn_hovered = titlebar_.GetMaximizeButton().hovered;
    tb.is_maximized = IsZoomed(hwnd_) != FALSE;
    tb.close_btn_rect = titlebar_.GetCloseButton().rect;
    tb.close_btn_hovered = titlebar_.GetCloseButton().hovered;
    tb.title_text_rect = titlebar_.GetTitleTextRect();
    tb.title_text = cached_title_text_;
    tb.window_active = window_active_;
    return tb;
}

ToastRenderState App::BuildToastRenderState() const
{
    ToastRenderState ts;
    ts.visible = toast_.IsVisible();
    ts.alpha = toast_.GetRenderAlpha();
    ts.message = toast_.GetMessage();
    return ts;
}

// ============================================================
// 描画 / リサイズ
// ============================================================

void App::OnPaint()
{
    // スムーススクロール中は描画前にデルタタイムでスクロール位置を進める。
    // SetTimerではなくWM_PAINTループで駆動することでディスプレイのリフレッシュレートに追従する。
    if (viewport_.IsSmoothScrolling()) {
        UpdateSmoothScroll();
    }

    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);

    auto layout = GetPaneLayout();
    if (!file_load_service_.IsLoading()) {
        // 現在表示中のダーティなノードを現在の幅でレイアウトする
        int anchor_idx = FindFirstVisibleNode();
        float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;

        bool updated = layout_service_->EnsureVisibleLayout(
            doc_, layout_cache_, layout.md_rect.width, layout.md_rect.height);

        if (updated) {
            AnchorCompensateScroll(anchor_idx, anchor_y_before, layout.md_rect.height);
        }
    }
    auto gs = BuildGestureRenderState();
    auto sp = BuildSidePaneState(layout);
    auto tb = BuildTitleBarRenderState();
    auto ts = BuildToastRenderState();

    if (file_load_service_.IsLoading()) {
        renderer_.DrawLoading(file_load_service_.GetLoadingAngle(), layout.md_rect, sp, tb, gs, ts);
    }
    else {
        renderer_.Render({
            doc_.GetNodesMut(), layout_cache_,
            viewport_.GetScrollY(), layout_service_->GetTotalHeight(),
            viewport_.GetSelection(), layout.md_rect, sp, tb,
            nav_service_.CanGoBack(), nav_service_.CanGoForward(),
            static_cast<int>(nav_hover_), hovered_copy_node_, gs, ts
            });
    }

    EndPaint(hwnd_, &ps);

    // スクロール継続中なら次フレームの再描画を要求（WM_PAINTループを維持）
    if (viewport_.IsSmoothScrolling()) {
        InvalidateMdPane(layout.md_rect);
    }
}

void App::OnResize(UINT width, UINT height)
{
    if (width == 0 || height == 0) {
        return;
    }

    renderer_.Resize(width, height);

    // タイトルバーボタン位置を再計算
    {
        float window_w_dip = width / cached_dpi_scale_;
        titlebar_.UpdateLayout(window_w_dip);
    }

    if (is_sizing_) {
        auto sizing_layout = GetPaneLayout();
        float sizing_h = sizing_layout.md_rect.height;
        SyncMaxScroll(sizing_h);
        UpdateScrollBar(sizing_h);
        Invalidate();
        return;
    }

    OnResizeEnd();
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested)
{
    cached_dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    if (cached_dpi_scale_ <= 0.0f) {
        cached_dpi_scale_ = 1.0f;
    }
    renderer_.SetDpi(static_cast<float>(dpi));

    layout_cache_.MarkAllDirty();

    SetWindowPos(hwnd_, nullptr,
        suggested->left, suggested->top,
        suggested->right - suggested->left,
        suggested->bottom - suggested->top,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

// ============================================================
// サイズ変更状態
// ============================================================

void App::OnEnterSizeMove()
{
    is_sizing_ = true;
    StopSmoothScroll();
}

void App::OnExitSizeMove()
{
    is_sizing_ = false;
    OnResizeEnd();
}

// ============================================================
// ファイル読み込み
// ============================================================

void App::LoadMarkdownFile(std::wstring_view path)
{
    if (!DocumentService::NeedsLoadingAnimation(path)) {
        file_load_service_.SetLoadingPath(path);
        DoLoadMarkdownFile();
    }
    else {
        file_load_service_.StartLoading(path);
        SetTimer(hwnd_, TIMER_LOADING_ANIM, 16, nullptr);
        Invalidate();
        UpdateWindow(hwnd_);
        PostMessage(hwnd_, WM_APP_LOAD_FILE, 0, 0);
    }
}

void App::DoLoadMarkdownFile()
{
    KillTimer(hwnd_, TIMER_LOADING_ANIM);

    viewport_.ClearSelection();
    mermaid_renderer_.CancelPending();
    renderer_.ShrinkBuffers();

    if (!file_load_service_.ExecuteLoad(doc_, layout_cache_)) {
        Invalidate();
        return;
    }

    std::pmr::wstring dir = doc_.GetDirectory();
    if (!dir.empty()) {
        file_explorer_.SetDirectory(dir);
        file_explorer_.SetCurrentFile(doc_.GetFilePath());
    }

    panes_.ResetScrollStates();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();

    UpdateLayoutAndScroll(0.0f);
    UpdateTitleBar();
    RequestMermaidRenders();

    doc_service_.StartWatching(doc_.GetFilePath(), [this]() {
        ReloadCurrentFile();
    });
}

void App::ReloadCurrentFile()
{
    if (doc_.GetFilePath().empty()) {
        return;
    }

    float old_scroll = viewport_.GetScrollY();
    mermaid_renderer_.CancelPending();

    if (file_load_service_.ExecuteReload(doc_, layout_cache_)) {
        renderer_.InvalidateTocPaneCache();
        UpdateLayoutAndScroll(old_scroll);
        RequestMermaidRenders();
    }
}

void App::UpdateTitleBar()
{
    int zoom_percent = static_cast<int>(ZOOM_STEPS[viewport_.GetZoomIndex()] * 100.0f + 0.5f);
    auto title = BuildTitleString(doc_.GetFilePath(), zoom_percent);
    SetWindowTextW(hwnd_, title.c_str());
    cached_title_text_ = std::move(title);
    Invalidate();
}

void App::RequestMermaidRenders()
{
    if (!mermaid_renderer_.IsReady()) {
        return;
    }

    float viewport_width = GetMarkdownPaneWidth();
    float content_width = viewport_width
        - renderer_.GetTheme().margin_left
        - renderer_.GetTheme().margin_right;

    // コンテンツ幅が0以下の場合（ズームでMDペインが極小になった場合など）は
    // 不正な幅でレンダリングしないようスキップする。
    // last_mermaid_content_width_ を更新しないことで、復帰時の幅変更検出を正しく保つ。
    if (content_width <= 0.0f) {
        return;
    }

    if (last_mermaid_content_width_ > 0.0f &&
        static_cast<int>(content_width) != static_cast<int>(last_mermaid_content_width_)) {
        // 図のサイズが新旧どちらのコンテンツ幅より小さければ
        // ビューポートに制約されていないため再生成不要
        float min_width = std::min(content_width, last_mermaid_content_width_);
        bool any_invalidated = false;
        for (size_t i = 0; i < doc_.GetNodes().size(); i++) {
            if (doc_.GetNodes()[i].code_language == SyntaxLanguage::Mermaid) {
                auto& diagram = layout_cache_.GetDiagram(i);
                if (diagram.bitmap && diagram.width > 0 &&
                    diagram.width + 1.0f < min_width) {
                    continue;
                }
                diagram.bitmap.Reset();
                diagram.width = 0;
                diagram.height = 0;
                any_invalidated = true;
            }
        }
        if (any_invalidated) {
            mermaid_renderer_.ClearCache();
        }
        mermaid_renderer_.ClearPendingQueue();
    }
    last_mermaid_content_width_ = content_width;

    for (size_t i = 0; i < doc_.GetNodes().size(); i++) {
        auto& node = doc_.GetNodesMut()[i];
        if (node.type != NodeType::CodeBlock) {
            continue;
        }
        if (node.code_language != SyntaxLanguage::Mermaid) {
            continue;
        }
        auto& diagram = layout_cache_.GetDiagram(i);
        if (diagram.bitmap) {
            continue;
        }

        mermaid_renderer_.RequestRender(node, layout_cache_[i], diagram,
            content_width, theme_service_.IsDarkMode(),
            [](void* ctx) { static_cast<App*>(ctx)->OnMermaidRenderComplete(); },
            this);
    }
}

void App::OnMermaidRenderComplete()
{
    int anchor_idx = FindFirstVisibleNode();
    float anchor_y_before = (anchor_idx >= 0) ? layout_cache_[anchor_idx].y_position : 0.0f;
    layout_service_->RecomputeAfterDiagram(doc_, layout_cache_, renderer_.GetTheme());
    auto layout = GetPaneLayout();
    float md_h = layout.md_rect.height;
    AnchorCompensateScroll(anchor_idx, anchor_y_before, md_h);
    Invalidate();
}

// ============================================================
// マウスホイール / キーボード
// ============================================================

void App::OnMouseWheel(int px, int py, short delta, bool ctrl)
{
    if (!renderer_.GetRenderTarget()) {
        return;
    }

    // 軸ロック用: 縦スクロール発生をスワイプ検出器に通知
    if (!ctrl) {
        bool had_overlay = swipe_detector_.IsOverlayVisible();
        swipe_detector_.NotifyVScroll(GetTickCount64());
        if (had_overlay) {
            KillTimer(hwnd_, TIMER_SWIPE_OVERLAY);
            Invalidate();
        }
    }

    if (ctrl) {
        MouseWheelEvent event{ delta, true, PaneZone::MdPane };
        ExecuteActions(controller_.HandleMouseWheel(event));
        return;
    }

    auto dip = PixelToDip(px, py);
    auto pane_layout = GetPaneLayout();
    auto zone = DetectPaneZone(dip.x, pane_layout,
        renderer_.GetTheme().splitter_width,
        panes_.IsFilePaneVisible(), panes_.IsTocPaneVisible());

    MouseWheelEvent event{ delta, false, zone };
    ExecuteActions(controller_.HandleMouseWheel(event));
}

void App::OnMouseHWheel(short delta)
{
    bool had_overlay = swipe_detector_.IsOverlayVisible();
    int old_direction = swipe_detector_.GetOverlayDirection();
    swipe_detector_.OnHWheel(delta, GetTickCount64());

    // 入力のたびにコミットタイマーをリセット。
    // 指を離して COMMIT_TIMEOUT_MS 経過後に Commit() でナビゲーション判定する。
    SetTimer(hwnd_, TIMER_SWIPE_OVERLAY,
        static_cast<UINT>(SwipeDetector::COMMIT_TIMEOUT_MS), nullptr);

    if (had_overlay != swipe_detector_.IsOverlayVisible()
        || old_direction != swipe_detector_.GetOverlayDirection()) {
        Invalidate();
    }
}

void App::OnKeyDown(WPARAM key)
{
    KeyDownEvent event{
        static_cast<int>(key),
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0
    };
    ExecuteActions(controller_.HandleKeyDown(event));
}

void App::ExecuteActions(const ActionList& actions)
{
    for (const auto& action : actions) {
        std::visit(overloaded{
            [this](const KeyScrollAction& a) {
                auto pane_layout = GetPaneLayout();
                float page_size = pane_layout.md_rect.height;
                switch (a.type) {
                    case ScrollType::LineUp:   SmoothScrollBy(-40.0f); break;
                    case ScrollType::LineDown: SmoothScrollBy(40.0f); break;
                    case ScrollType::PageUp:   SmoothScrollBy(-page_size * 0.9f); break;
                    case ScrollType::PageDown: SmoothScrollBy(page_size * 0.9f); break;
                    case ScrollType::Home:     SmoothScrollBy(-viewport_.GetScrollY()); break;
                    case ScrollType::End:      SmoothScrollBy(viewport_.GetMaxScroll() - viewport_.GetScrollY()); break;
                }
            },
            [this](const SmoothScrollByAction& a) {
                SmoothScrollBy(a.delta);
            },
            [this](const ScrollPaneAction& a) {
                auto pane_layout = GetPaneLayout();
                const auto& theme = renderer_.GetTheme();
                if (a.pane == PaneZone::FilePane) {
                    float max_file_scroll = std::max(0.0f,
                        static_cast<float>(file_explorer_.GetEntries().size()) * theme.pane_item_height
                        - (pane_layout.file_rect.height - theme.pane_header_height));
                    if (panes_.ScrollFilePaneBy(a.delta, max_file_scroll)) {
                        renderer_.InvalidateFilePaneCache();
                        Invalidate();
                    }
                }
                else if (a.pane == PaneZone::TocPane) {
                    float max_toc_scroll = std::max(0.0f, static_cast<float>(doc_.GetToc().GetEntries().size()) * theme.pane_item_height - (pane_layout.toc_rect.height - theme.pane_header_height));
                    if (panes_.ScrollTocPaneBy(a.delta, max_toc_scroll)) {
                        renderer_.InvalidateTocPaneCache();
                        Invalidate();
                    }
                }
            },
            [this](const CopyClipboardAction&) {
                CopySelectionToClipboard();
            },
            [this](const SelectAllAction&) {
                SelectAll();
            },
            [this](const ClearSelectionAction&) {
                ClearSelection();
            },
            [this](const TogglePaneAction& a) {
                if (a.file_pane) {
                    panes_.ToggleFilePane();
                }
 else {
  panes_.ToggleTocPane();
}
RefreshPaneLayout();
},
[this](const ZoomAction& a) {
    if (a.direction > 0) {
        ZoomIn();
    }
    else if (a.direction < 0) {
        ZoomOut();
    }
    else {
    ZoomReset();
}
},
[this](const ReloadFileAction&) {
    ReloadCurrentFile();
},
[this](const OpenFileAction&) {
    auto path = FileLoader::OpenFileDialog(hwnd_);
    if (!path.empty()) {
        if (!doc_.GetFilePath().empty()) {
            PushNavHistory();
        }
        LoadMarkdownFile(path);
    }
},
[this](const ToggleDarkModeAction&) {
    ToggleDarkMode();
},
[this](const NavigateBackAction&) {
    NavigateBack();
},
[this](const NavigateForwardAction&) {
    NavigateForward();
},
            }, action);
    }
}

void App::OnDropFiles(HDROP hDrop)
{
    UINT required = DragQueryFileW(hDrop, 0, nullptr, 0);
    if (required > 0) {
        std::pmr::wstring path(required, L'\0');
        if (DragQueryFileW(hDrop, 0, path.data(), required + 1)) {
            if (!doc_.GetFilePath().empty()) {
                PushNavHistory();
            }
            LoadMarkdownFile(path);
        }
    }
    DragFinish(hDrop);
}

void App::HandleTimer(UINT_PTR timer_id)
{
    switch (timer_id) {
    case TIMER_FILE_WATCH:     doc_service_.CheckForChanges(); break;
    case TIMER_DEFERRED_LAYOUT: OnDeferredLayout(); break;
    case TIMER_LOADING_ANIM:
        file_load_service_.TickLoadingAnimation();
        Invalidate();
        break;
    case TIMER_SWIPE_OVERLAY: {
        auto result = swipe_detector_.Commit();
        bool need_redraw = false;
        switch (result) {
        case SwipeResult::Back:
            NavigateBack();
            need_redraw = true;
            break;
        case SwipeResult::Forward:
            NavigateForward();
            need_redraw = true;
            break;
        default:
            break;
        }
        KillTimer(hwnd_, TIMER_SWIPE_OVERLAY);
        if (need_redraw) {
            Invalidate();
        }
        break;
    }
    case TIMER_TOAST: {
        if (!toast_.Tick()) {
            KillTimer(hwnd_, TIMER_TOAST);
        }
        Invalidate();
        break;
    }
    default: break;
    }
}

void App::OnAppLoadFile()
{
    DoLoadMarkdownFile();
}

void App::OnCaptureChanged()
{
    if (gesture_.GetPhase() != GesturePhase::Idle) {
        gesture_.Reset();
        Invalidate();
    }
}

void App::ShowToast(std::wstring_view message)
{
    toast_.Show(message);
    SetTimer(hwnd_, TIMER_TOAST, 16, nullptr);
    Invalidate();
}

void App::OnDestroy()
{
    SaveLastFilePath();
    SavePaneState();
    KillTimer(hwnd_, TIMER_FILE_WATCH);
    KillTimer(hwnd_, TIMER_DEFERRED_LAYOUT);
    KillTimer(hwnd_, TIMER_LOADING_ANIM);
    KillTimer(hwnd_, TIMER_SWIPE_OVERLAY);
    KillTimer(hwnd_, TIMER_TOAST);
}

// ============================================================
// 最後に開いたファイルの永続化
// ============================================================

void App::SaveLastFilePath()
{
    if (doc_.GetFilePath().empty()) {
        return;
    }
    config_.SaveWString(L"last_file.txt", doc_.GetFilePath());
}

std::pmr::wstring App::LoadLastFilePath() const
{
    std::pmr::wstring path = config_.LoadWString(L"last_file.txt");
    if (path.empty()) {
        return {};
    }
    // 安全なローカルファイルパスであることを検証
    // UNCパス (\\server\...) やデバイスパス (\\.\, \\?\) をブロック
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return {};
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

void App::ShowDirectory(std::wstring_view dir_path)
{
    file_explorer_.SetDirectory(dir_path);
    renderer_.InvalidateFilePaneCache();
    Invalidate();
}

// ============================================================
// ペイン状態の永続化
// ============================================================

void App::SavePaneState()
{
    config_.SaveBool(L"pane_show_file.txt", panes_.IsFilePaneVisible());
    config_.SaveBool(L"pane_show_toc.txt", panes_.IsTocPaneVisible());
    config_.SaveInt(L"pane_file_width.txt", static_cast<int>(std::lround(panes_.GetFilePaneWidth())));
    config_.SaveInt(L"pane_toc_width.txt", static_cast<int>(std::lround(panes_.GetTocPaneWidth())));
}

void App::LoadPaneState()
{
    panes_.SetFilePaneVisible(config_.LoadBool(L"pane_show_file.txt", true));
    panes_.SetTocPaneVisible(config_.LoadBool(L"pane_show_toc.txt", true));

    constexpr int kDefaultWidth = static_cast<int>(PaneController::PANE_DEFAULT_WIDTH);
    constexpr int kMinWidth = static_cast<int>(PaneController::PANE_MIN_WIDTH);

    // クライアント幅に基づいて有効な最大ペイン幅を計算する
    int dynamic_max = kDefaultWidth;
    if (hwnd_) {
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            int client_width = rc.right - rc.left;
            if (client_width > 0) {
                dynamic_max = std::max(kMinWidth, client_width - kMinWidth);
            }
        }
    }

    panes_.SetFilePaneWidth(static_cast<float>(
        config_.LoadInt(L"pane_file_width.txt", kDefaultWidth, kMinWidth, dynamic_max)));
    panes_.SetTocPaneWidth(static_cast<float>(
        config_.LoadInt(L"pane_toc_width.txt", kDefaultWidth, kMinWidth, dynamic_max)));
}
