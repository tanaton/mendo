#pragma once
#include "app_events.h"
#include "app_state.h"
#include "app_resource_manager_callbacks.h"
#include "app_side_effect_callbacks.h"
#include "async_load_result.h"
#include "clipboard_manager.h"
#include "config_service.h"
#include "cursor_manager.h"
#include "file_load_service.h"
#include "file_watcher.h"
#include "hit_test_service.h"
#include "image_loader.h"
#include "layout.h"
#include "mermaid.h"
#include "mermaid_file_cache.h"
#include "reload.h"
#include "render_params.h"
#include "renderer.h"
#include "task_scheduler.h"
#include "theme_service.h"
#include "win32_host_impl.h"
#include <windows.h>
#include <shellapi.h>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

class App {
    friend struct AppSideEffectCallbacks;
    friend struct AppResourceManagerCallbacks;
    friend struct AppSearchBarCallbacks;

public:
    explicit App(ConfigService& config) noexcept : config_(config)
    {}
    bool Init(HWND hwnd);

    void LoadHelpDocument();
    // Init 前に呼ぶ。初回描画に含まれるため無効化はしない。
    void SetInitialDirectory(std::wstring_view dir_path);

    // 起動時にウィンドウ生成と並列で I/O + パースを開始する。Init 末尾の
    // OnInitComplete で hwnd が解禁されると、worker は ::PostMessageW(PARSE_COMPLETE)
    // を発行し、通常の async load 経路に合流する。
    void StartPreloadAsync(std::pmr::wstring path);

    // Init() より前に呼ぶこと。preload 即時完了パスは Init 内で復元情報を参照する。
    constexpr void SetPendingRestoreNode(int node, int offset) noexcept
    {
        state_.view.scroll_restore.SetNodeRestore(node, offset);
    }

    void OnPaint();
    void OnResize(UINT width, UINT height);
    void OnMouseWheel(int px, int py, short delta, bool ctrl = false);
    void OnMouseHWheel(short delta);
    void OnKeyDown(WPARAM key);
    void OnDropFiles(HDROP hDrop);
    void OnDpiChanged(UINT dpi, const RECT* suggested);

    void OnLButtonDown(int px, int py);
    void OnLButtonUp(int px, int py);
    void OnMouseMove(int px, int py);
    void OnLButtonDblClk(int px, int py);
    void OnContextMenu(int screen_x, int screen_y);
    bool OnRButtonDown(int px, int py);
    bool OnRButtonUp(int px, int py);
    void OnRButtonMove(int px, int py);

    void OnMouseHover(int px, int py);
    void OnMouseLeave()
    {
        Dispatch(MouseLeaveAction{});
    }

    void OnXButtonBack()
    {
        Dispatch(NavigateBackAction{});
    }
    void OnXButtonForward()
    {
        Dispatch(NavigateForwardAction{});
    }

    HANDLE GetFileWatchEvent() const noexcept
    {
        return file_watcher_.GetEventHandle();
    }
    void OnFileWatchEvent();

    void HandleTimer(UINT_PTR timer_id);
    void OnAppImageLoaded();
    void OnParseComplete();
    void OnMermaidDiskLoaded();
    void OnCaptureChanged();
    void OnDestroy();

    void OnSearchTextChanged(const std::pmr::wstring& text)
    {
        Dispatch(SearchTextChangedAction{ text });
    }
    void OnSearchClose()
    {
        Dispatch(CloseSearchBarAction{});
    }
    void OnSearchNext()
    {
        Dispatch(SearchNextAction{});
    }
    void OnSearchPrev()
    {
        Dispatch(SearchPrevAction{});
    }
    constexpr bool IsSearchBarVisible() const noexcept
    {
        return state_.search.search_state.IsVisible();
    }
    void SetSearchSelection(int sel_start, int sel_end)
    {
        Dispatch(SearchSelectionAction{ sel_start, sel_end });
    }
    void SetImeComposition(std::pmr::wstring comp)
    {
        Dispatch(ImeCompositionAction{ std::move(comp) });
    }
    RECT GetSearchEditRect();

    void OnEnterSizeMove()
    {
        Dispatch(EnterSizeMoveAction{});
    }
    void OnExitSizeMove()
    {
        Dispatch(ExitSizeMoveAction{});
    }
    void OnActivate(bool active)
    {
        Dispatch(ActivateAction{ active });
    }

    constexpr float GetDpiScale() const noexcept
    {
        return state_.window.cached_dpi_scale;
    }
    float GetTitleBarHeightDip() const noexcept
    {
        return state_.window.titlebar.GetHeight();
    }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept
    {
        return state_.window.titlebar.HitTest(dip_x, dip_y);
    }
    bool IsOverMdScrollbar(float dip_x, float dip_y);

private:
    void Dispatch(const AppAction& action);

    // reducer を介さない経路から effect を単発発火するヘルパー。
    // SideEffectList を作らずに executor の単発 API へ直送し、毎呼び出しの
    // pmr vector アロケーションを避ける (ホット経路: OnDeferredLayout, OnPaint, OnSizing 等)。
    template <typename T>
    void EmitEffect(T&& e)
    {
        effect_executor_.ExecuteOne(SideEffect{ std::forward<T>(e) });
    }
    void ShowToast(std::wstring_view message);

    bool IsRenderReady() const noexcept
    {
        return renderer_.GetRenderTarget() != nullptr;
    }
    void Invalidate() noexcept
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void InvalidatePane(const PaneRect& rect) noexcept;
    void InvalidateTitleBar();

    // ---- 初期化 (app_init.cpp) ----
    void RestoreThemeAndZoom();
    void RestorePaneState();
    void AttachPreload();

    // ---- 描画 (app.cpp) ----
    SidePaneState BuildSidePaneState(const PaneLayout& layout) const;
    TitleBarRenderState BuildTitleBarRenderState() const;
    void SyncRendererSearchMatches();

    // ---- 終了 (app.cpp) ----
    void SaveSession();

    // ---- 座標・ヒットテスト ----
    DipPoint PixelToDip(int px, int py) const noexcept;

    using HitResult = HitTestService::HitResult;
    HitResult HitTest(int screen_x, int screen_y);
    HitResult HitTest(const MdPaneHitContext& ctx);
    std::optional<std::pmr::string> GetLinkAtHit(const HitResult& hit) const;
    MdPaneHitContext BuildMdPaneHitContext(int px, int py, const PaneLayout& pane_layout) const noexcept;

    const PaneLayout& GetPaneLayout();
    PaneZone PaneAtPoint(float dip_x);
    PaneZone ZoneAt(float dip_x, const PaneLayout& layout) const noexcept;
    bool IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept;
    static bool IsOverPaneScrollbar(float dip_x, const PaneRect& rect, const PaneScrollInfo& scroll_info) noexcept;

    SearchBarLayout ComputeSearchBarLayoutForMd(const PaneRect& md_rect) const;
    int HitTestSearchInputPos(const SearchBarLayout& sbl, std::wstring_view query_wide, float dip_x) const;

    // ---- クリック (app_mouse_click.cpp) ----
    void HandleLinkClick(std::string_view url);
    bool HandleTitleBarClick(float dip_x, float dip_y);
    bool HandleSearchBarClick(float dip_x, float dip_y, const PaneLayout& layout, bool is_double_click);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& layout);
    // コピー/ダイアグラムコピー/保存ボタンを処理したら true。
    bool HandleCodeBlockButtonClick(const MdPaneHitContext& hit_ctx);
    // ホバー中ブロックの水平スクロールバー上ならドラッグを開始して true。
    bool TryStartBlockHScrollDrag(float dip_x, float dip_y, const PaneLayout& layout);
    void HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout);
    void HandleFileEntryClick(const FileEntry& entry);

    // ---- ホバー (app_mouse_hover.cpp) ----
    void HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const PaneLayout& layout);
    // 戻り値はホバー中の項目 index (なければ -1)。
    int HandleSidePaneHover(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout);
    TooltipTarget BuildSidePaneTooltip(PaneTarget target, PaneHeaderButton hit, int idx) const;
    TooltipTarget BuildMdContentTooltip(const HitResult& hit, const std::optional<std::pmr::string>& link) const;
    // サイドペインのホバー状態をリセットし、変化があれば invalidate する。
    // reset_hover_index=true のとき hover index もリセット（タイトルバー移動時など）。
    void ResetSidePaneHover(PaneTarget t, const PaneLayout& pane_layout, bool reset_hover_index);
    // サイドペインキャッシュ無効化とペイン再描画リクエストをまとめて発行する。
    void InvalidateSidePaneAndPane(PaneTarget t, const PaneLayout& pane_layout);

    // ---- レイアウト (app_layout.cpp) ----
    void EnsureScrollTarget();
    // 可視範囲を即時計測し、scroll_target があれば scroll_y を再評価する。
    void ViewportLayout(float md_width, float md_height);
    // 合計コンテンツ高を max_scroll に反映し scroll_y をクランプする。
    void SyncMaxScroll(float md_height);
    void ScheduleDeferredLayoutIfNeeded();
    void InvalidateHitPositions();
    void OnResizeEnd();
    void RefreshPaneLayout();
    void RefreshFilePane();
    void OnDeferredLayout();
    void FinalizeLayout(float md_pane_height);
    void SyncTocActiveAndAutoScroll(bool auto_scroll);
    float MdContentWidth();
    // スクロール上限/スクロールバー計算用のコンテンツ高さ。
    float ScrollableContentHeight() const noexcept;

    // ---- ファイル読み込み (app_file.cpp) ----
    void LoadMarkdownFile(std::wstring_view path);
    // 表示文書とレイアウトキャッシュを一緒に差し替え、旧文書の破棄を worker に回す。
    // estimated は worker が MakeEstimatedLayoutCache で作ったもの。省略時は UI スレッドで推定する。
    void ReplaceDocument(Document next, LayoutCache estimated);
    void ReplaceDocument(Document next);
    void ReloadCurrentFile();
    void DoReloadCurrentFile();
    void DoLoadMarkdownFile();
    // reload_base は同一ファイルのリロード時の現在テキスト (worker でパース前に差分判定させる)。
    void BeginAsyncLoad(std::pmr::wstring path, bool suppress_animation = false,
                        std::shared_ptr<const std::pmr::string> reload_base = nullptr);
    void StopLoadingAnimation();
    // reload_diff_pos: 同一パス再読込時の差分位置 (UTF-8 byte offset)。npos なら差分なし。
    void FinishLoadMarkdownFile(bool follow_file_pane, size_t reload_diff_pos = std::string_view::npos);
    void HandleLoadFailureFallback();
    bool ApplyMermaidCacheHeights();
    // Mermaid/画像キャッシュの実測値でノード高さを上書きし、変化があれば Y 位置を再計算する。
    // 呼び出し前にノード高さの初期化 (EstimateNodeHeights) は完了していること。
    void ApplyCachedHeightsAndRecompute();
    void UpdateTitleBar();

    void FinishReload(size_t diff_pos);

    enum class ReloadFlow : uint8_t {
        Handled,            // ResumeFileWatch / DeferReloadRetry が発行済み、呼び出し元は return
        ContinueWithReload, // PrefixGrowth / FullReload を呼び出し元で実行する
    };
    // NoChange / DeferPrefixShrink をここで処理し、残りは呼び出し元に任せる。
    ReloadFlow ApplyReloadDecisionEarly(const ReloadDecision& decision);

    // 短縮リトライで再リロードを予約する。エディタの truncate→rewrite 中や
    // partial-read を検出した時に共通で使う。
    void DeferReloadRetry();

    // partial-read を検出したら defer して true を返す。
    bool DeferIfPartialWrite(const std::pmr::wstring& path, size_t read_size);

    void CancelPendingResources();
    void ResetViewForNewDocument();
    void ReleaseCaptureIfDragDropped(bool had_left_drag);

    // ---- テーマ (app_theme.cpp) ----
    void HandleApplyThemeChange(const effect::ApplyThemeChange& e);
    void FinishThemeOrZoomChange();

private:
    HWND hwnd_ = nullptr;

    CursorManager cursors_;

    Renderer renderer_;
    TaskScheduler scheduler_;
    TaskScheduler layout_scheduler_;
    MermaidFileCache file_cache_; // mermaid_renderer_ より先に宣言する（mermaid_renderer_ は破棄時に file_cache_ を参照する）
    ClipboardManager clipboard_manager_; // mermaid_renderer_ より先に宣言する（~MermaidRenderer の CancelPending が in-flight コールバック経由で clipboard_manager_ に触るため、後に破棄させる）
    MermaidRenderer mermaid_renderer_;
    ImageLoader image_loader_;
    FileWatcher file_watcher_;
    ConfigService& config_;
    ThemeService theme_service_{ config_ };
    SessionService session_{ config_ };
    FileLoadService file_load_service_;

    AppState state_;

    HitTestService hit_test_;
    std::optional<LayoutService> layout_service_;
    ResourceManager resource_manager_;
    Win32Host win32_host_;
    SideEffectExecutor effect_executor_;
};
