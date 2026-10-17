#pragma once
#include "app_callbacks.h"
#include "app_events.h"
#include "app_state.h"
#include "async_load_result.h"
#include "clipboard_manager.h"
#include "config_service.h"
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
#include "selection_html.h"
#include "task_scheduler.h"
#include "theme_service.h"
#include <windows.h>
#include <shellapi.h>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

class CursorManager {
public:
    void Init() noexcept
    {
        arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        hand_ = LoadCursorW(nullptr, IDC_HAND);
        ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
        sizewe_ = LoadCursorW(nullptr, IDC_SIZEWE);
    }

    constexpr HCURSOR Arrow() const noexcept
    {
        return arrow_;
    }
    constexpr HCURSOR Hand() const noexcept
    {
        return hand_;
    }
    constexpr HCURSOR IBeam() const noexcept
    {
        return ibeam_;
    }
    constexpr HCURSOR SizeWE() const noexcept
    {
        return sizewe_;
    }

private:
    HCURSOR arrow_ = nullptr;
    HCURSOR hand_ = nullptr;
    HCURSOR ibeam_ = nullptr;
    HCURSOR sizewe_ = nullptr;
};

class App {
    friend struct AppCallbacksBase;
    friend struct AppResourceManagerCallbacks;
    friend struct AppSearchBarCallbacks;

public:
    explicit App(ConfigService& config) noexcept : config_(config)
    {}
    bool Init(HWND hwnd);

    void LoadHelpDocument();
    // ウィンドウを保存済みの配置で表示した後、初回描画の前に呼ぶ。
    // 先に取り込むと既定サイズでレイアウトし、配置の復元でやり直しになる。
    // preload が無ければヘルプを開く。
    void LoadInitialDocument();
    // Init は描画デバイスを作らない。D3D デバイスの作成を待つ間に文書の取り込みまで進めるため、
    // LoadInitialDocument の後、初回描画の前に呼ぶ。
    bool CreateRenderDevice();
    // Init 前に呼ぶ。初回描画に含まれるため無効化はしない。
    void SetInitialDirectory(std::wstring_view dir_path);

    // 起動時にウィンドウ生成と並列で I/O + パースを開始する。LoadInitialDocument で
    // hwnd が解禁されると、worker は ::PostMessageW(PARSE_COMPLETE) を発行し、
    // 通常の async load 経路に合流する。
    void StartPreloadAsync(std::pmr::wstring path);

    // LoadInitialDocument より前に呼ぶこと。preload 即時完了パスはそこで復元情報を参照する。
    constexpr void SetPendingRestoreNode(int node, int offset) noexcept
    {
        state_.view.scroll_restore.SetNodeRestore(node, offset);
    }

    // can_defer: 前のフレームがまだ表示待ちなら描かずに戻ってよいか。メッセージループから直接届いた
    // WM_PAINT だけ true にする (モーダルループや UpdateWindow の中では待ちが再開されない)。
    void OnPaint(bool can_defer);
    void OnResize(UINT width, UINT height);
    void OnMinimize();
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
    // 描画を見送っている間だけ、メッセージループが待つハンドルを返す。
    HANDLE GetFrameWaitHandle() const noexcept
    {
        return frame_deferred_ ? renderer_.FrameWaitHandle() : nullptr;
    }
    // GetFrameWaitHandle の待ちが成立した、または待ちきれなかった。後者では GPU が止まっていても
    // 描いて Present させ、デバイスロストを検出させる (シグナルを待ち続けると画面が固まったままになる)。
    void OnFrameReady()
    {
        frame_deferred_ = false;
        renderer_.MarkFrameAcquired();
        Invalidate();
    }

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

    // reducer が返した副作用を実行する。
    void Execute(const SideEffectList& effects)
    {
        for (const auto& e : effects) {
            Execute(e);
        }
    }
    void Execute(const SideEffect& effect);
    void SetTimer(app_timer::Id id, UINT ms) noexcept
    {
        ::SetTimer(hwnd_, std::to_underlying(id), ms, nullptr);
    }
    void KillTimer(app_timer::Id id) noexcept
    {
        ::KillTimer(hwnd_, std::to_underlying(id));
    }
    void ShowToast(std::wstring_view message);
    // 変更通知はデバウンスしてから読み直す。
    void StartFileWatch(const std::pmr::wstring& path);
    void OpenFileDialog();

    bool IsRenderReady() const noexcept
    {
        return renderer_.GetRenderTarget() != nullptr;
    }
    void Invalidate() noexcept
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ---- 初期化 ----
    void RestoreTheme();
    void RestorePaneState();

    // ---- 描画 ----
    void UpdateTitleBar();
    SidePaneState BuildSidePaneState(const PaneLayout& layout) const;
    TitleBarRenderState BuildTitleBarRenderState() const;
    void SyncRendererSearchMatches();

    // ---- 終了 ----
    void SaveSession();

    // ---- 座標・ヒットテスト ----
    DipPoint PixelToDip(int px, int py) const noexcept;

    using HitResult = hit_test::HitResult;
    HitResult HitTest(int px, int py);
    std::optional<LinkHit> GetLinkAtHit(const HitResult& hit) const;
    MdPaneHitContext BuildMdPaneHitContext(int px, int py, const PaneLayout& pane_layout) const noexcept;

    const PaneLayout& GetPaneLayout();
    PaneZone PaneAtPoint(float dip_x);
    PaneZone ZoneAt(float dip_x, const PaneLayout& layout) const noexcept;
    bool IsOverMdScrollbar(float dip_x, float dip_y, const PaneLayout& layout) const noexcept;

    // MD ペインで点が指す領域。ホバーとクリックが同じ優先順位で判定するために共有する。
    struct MdPaneHit {
        enum class Kind : uint8_t {
            SearchBar,
            NavButton,
            OverlayButton, // コピー / ダイアグラムコピー / 保存
            Scrollbar,
            Content,
        };
        Kind kind = Kind::Content;
        SearchBarHitZone search_bar = SearchBarHitZone::None;
        NavButtonHover nav = NavButtonHover::None;
        OverlayButtonHit overlay_button{};
    };
    MdPaneHit ResolveMdPaneHit(float dip_x, float dip_y, const MdPaneHitContext& hit_ctx, const PaneLayout& layout) const;

    SearchBarLayout ComputeSearchBarLayoutForMd(const PaneRect& md_rect) const;
    // 検索バーが表示中で dip_y がバーの上にあるときだけ、そのレイアウトを返す。
    std::optional<SearchBarLayout> SearchBarLayoutAt(float dip_y, const PaneRect& md_rect) const;
    int HitTestSearchInputPos(const SearchBarLayout& sbl, std::wstring_view query_wide, float dip_x) const;

    // ---- クリック (app_mouse.cpp) ----
    void HandleLinkClick(std::string_view url);
    bool HandleTitleBarClick(float dip_x, float dip_y);
    bool HandleSearchBarClick(float dip_x, float dip_y, const PaneLayout& layout, bool is_double_click);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& layout);
    void HandleOverlayButtonClick(const OverlayButtonHit& hit, float content_width);
    // ホバー中ブロックの水平スクロールバー上ならドラッグを開始して true。
    bool TryStartBlockHScrollDrag(const MdPaneHitContext& hit_ctx, float dip_x);
    void HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout);
    void HandleFileEntryClick(const FileEntry& entry);
    void RefreshFilePane();

    // ---- ホバー (app_mouse.cpp) ----
    // ゾーン内のホバーを hover に書き、表示するカーソルを返す。状態の反映は reducer が行う。
    HCURSOR HoverMdPane(float dip_x, float dip_y, int px, int py, const PaneLayout& layout, PointerHover& hover) const;
    HCURSOR HoverSidePane(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout, PointerHover& hover) const;
    TooltipTarget BuildSidePaneTooltip(PaneTarget target, PaneHeaderButton hit, int idx) const;
    TooltipTarget BuildMdContentTooltip(int hit_node, std::optional<LinkHit> link) const;

    // ---- レイアウト ----
    void EnsureScrollTarget();
    // 可視範囲を即時計測し、scroll_target があれば scroll_y を再評価する。
    void ViewportLayout(float md_width, float md_height);
    // 合計コンテンツ高を max_scroll に反映し scroll_y をクランプする。
    void SyncMaxScroll(float md_height);
    // 遅延レイアウトを仕掛けたら true。
    bool ScheduleDeferredLayoutIfNeeded();
    void SettleLayout();
    void InvalidateHitPositions();
    void OnResizeEnd();
    void OnDeferredLayout();
    void FinalizeLayout(float md_pane_height);
    void SyncTocActiveAndAutoScroll(bool auto_scroll);
    float MdContentWidth();
    // スクロール上限/スクロールバー計算用のコンテンツ高さ。
    float ScrollableContentHeight() const noexcept;

    // ---- ファイル読み込み (app_file.cpp) ----
    void LoadMarkdownFile(std::wstring_view path);
    void CancelOtherFileLoad();
    // 表示文書とレイアウトキャッシュを一緒に差し替え、旧文書の破棄を worker に回す。
    // estimated は worker が MakeEstimatedLayoutCache で作ったもの。省略時は UI スレッドで推定する。
    void ReplaceDocument(Document next, LayoutCache estimated);
    void ReplaceDocument(Document next);
    void ReloadCurrentFile();
    void DoReloadCurrentFile();
    void DoLoadMarkdownFile();
    // reload_base は同一ファイルのリロード時の現在テキスト (worker でパース前に差分判定させる)。
    void BeginAsyncLoad(std::pmr::wstring path, bool show_animation,
                        std::shared_ptr<const std::pmr::string> reload_base = nullptr);
    void StopLoadingAnimation();
    void FinishLoadMarkdownFile(bool follow_file_pane);
    void HandleLoadFailureFallback();
    // Mermaid/画像キャッシュの実測値でノード高さを上書きし、変化があれば Y 位置を再計算する。
    // 呼び出し前にノード高さの初期化 (EstimateNodeHeights) は完了していること。
    void ApplyCachedHeightsAndRecompute();

    void FinishReload(size_t diff_pos);

    enum class ReloadFlow : uint8_t {
        Handled,            // ResumeFileWatch / DeferReloadRetry が発行済み、呼び出し元は return
        ContinueWithReload, // Replace を呼び出し元で実行する
    };
    // NoChange / Defer* をここで処理し、Replace は呼び出し元に任せる。
    // has_parsed_doc は新しい内容のパース済み文書が手元にあるか (prefix shrink を受け入れられるか)。
    ReloadFlow ApplyReloadDecisionEarly(const ReloadDecision& decision, bool has_parsed_doc);

    // 短縮リトライで再リロードを予約する。エディタの truncate→rewrite 中や
    // partial-read を検出した時に共通で使う。
    void DeferReloadRetry();

    void CancelPendingResources();
    // リロード用。中身が変わらないノードの計測結果を引き継いでから差し替える。
    void ReloadDocument(Document next, LayoutCache estimated);
    void ResetViewForNewDocument();
    void EndDocumentBoundDrag();

    // ---- テーマ ----
    void HandleApplyThemeChange(const effect::ApplyThemeChange& e);
    void FinishThemeOrZoomChange();

private:
    HWND hwnd_ = nullptr;
    // 前のフレームの表示待ちで描画を見送った。メッセージループが FrameWaitHandle を待つ。
    bool frame_deferred_ = false;

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
    PrefixShrinkRetry prefix_shrink_retry_;

    AppState state_;

    std::optional<LayoutService> layout_service_;
    ResourceManager resource_manager_;
};
