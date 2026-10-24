#pragma once
#include "app_events.h"
#include "app_state.h"
#include "clipboard_manager.h"
#include "config_service.h"
#include "context_menu.h"
#include "file_load_service.h"
#include "file_watcher.h"
#include "hit_test_service.h"
#include "image_loader.h"
#include "layout.h"
#include "mermaid.h"
#include "mermaid_file_cache.h"
#include "document_utils.h"
#include "render_params.h"
#include "renderer.h"
#include "resource_manager.h"
#include "search_bar_controller.h"
#include "selection_html.h"
#include "settings_dialog.h"
#include "side_effect.h"
#include "task_scheduler.h"
#include "theme_service.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class App;

// ResourceManager と SearchBarController が App に問い合わせる窓口。
class AppHost final : public ResourceManagerHost, public SearchBarHost {
public:
    explicit AppHost(App& app) noexcept : app_(app)
    {}

    void SetTimer(app_timer::Id id, UINT ms) override;
    void KillTimer(app_timer::Id id) override;
    float MdContentWidth() override;
    float MdPaneHeight() override;
    float MdMaxScroll() override;
    const Theme& GetTheme() override;
    void RecomputeLayout(layout::HeightChangeRange changed) override;
    void EnsureNodeLayout(int node_index) override;

private:
    App& app_;
};

struct SystemCursors {
    HCURSOR arrow = nullptr;
    HCURSOR hand = nullptr;
    HCURSOR ibeam = nullptr;
    HCURSOR size_we = nullptr;

    void Load() noexcept
    {
        arrow = LoadCursorW(nullptr, IDC_ARROW);
        hand = LoadCursorW(nullptr, IDC_HAND);
        ibeam = LoadCursorW(nullptr, IDC_IBEAM);
        size_we = LoadCursorW(nullptr, IDC_SIZEWE);
    }
};

class App {
    friend class AppHost;

public:
    explicit App(ConfigService& config) noexcept : config_(config)
    {}
    bool Init(HWND hwnd);

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
    // 保存したスクロール位置を、文書を開いたときに戻す位置にする。
    void RestoreScrollPosition();

    void OnPaint();
    void OnResize(UINT width, UINT height);
    void OnMinimize();
    void OnMouseWheel(int px, int py, int delta, bool ctrl);
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
    // ポップアップ (PopupWindow) やシステムメニューのモーダルループに入った・出た (WM_ENTERMENULOOP / WM_EXITMENULOOP)。
    // 開いている間は本体に入力が来ないので、先にホバーとツールチップを消し、裏で本体を描き直しても出さない
    // (RefreshHoverAtCursor)。
    void OnModalLoop(bool entered)
    {
        in_modal_loop_ = entered;
        if (entered) {
            OnMouseLeave();
        }
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
    // 次のフレームを描いてよくなるまで (前のフレームの表示待ちの間) だけ返す。メッセージループはこれを待つ間
    // WM_PAINT を配らず、届いた入力を先に反映する。入力から表示までが 1 フレーム縮む。
    HANDLE GetFrameWaitHandle() const noexcept
    {
        return renderer_.FrameWaitHandle();
    }
    // GetFrameWaitHandle の待ちが成立した、または待ちきれなかった。後者では GPU が止まっていても
    // 描いて Present させ、デバイスロストを検出させる (シグナルを待ち続けると画面が固まったままになる)。
    void OnFrameReady() noexcept
    {
        renderer_.MarkFrameAcquired();
    }

    void HandleTimer(UINT_PTR timer_id);
    // 時間で区切った仕事 (遅延レイアウト・検索・GPU 転送) が残っている。WM_TIMER は約 15.6ms 刻みでしか
    // 発火せず仕事の合間が空くので、メッセージループが入力と描画を配り終えた空き時間に RunIdleWork で進める。
    bool HasIdleWork();
    void RunIdleWork();
    // 起動時の読み込みを LoadInitialDocument で取り込めず、結果を待っている。
    bool IsLoadingInitialDocument() const noexcept
    {
        return file_load_service_.IsAsyncLoading();
    }
    // 前のフレームの表示を待つ間に呼ぶ。次の WM_PAINT で同期計測するはずの可視範囲を先に計測し、
    // 計測時間を vblank 待ちに隠す。
    void PrepareFrame();
    void OnAppImageLoaded();
    void OnParseComplete();
    void OnMermaidDiskLoaded();
    void OnFileListed();
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
    // 関連付けの解決やブラウザの起動で数百 ms 止まりうるので、UI スレッドを待たせずに開く。
    static void ShellOpenAsync(std::pmr::wstring target);
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
        NavDirection nav = NavDirection::None;
        OverlayButtonHit overlay_button{};
    };
    MdPaneHit ResolveMdPaneHit(float dip_x, float dip_y, const MdPaneHitContext& hit_ctx, const PaneLayout& layout) const;

    SearchBarLayout ComputeSearchBarLayoutForMd(const PaneRect& md_rect) const;
    // 検索バーが表示中で dip_y がバーの上にあるときだけ、そのレイアウトを返す。
    std::optional<SearchBarLayout> SearchBarLayoutAt(float dip_y, const PaneRect& md_rect) const;
    int HitTestSearchInputPos(const SearchBarLayout& sbl, float dip_x) const;

    // ---- クリック (app_mouse.cpp) ----
    void HandleLinkClick(std::string_view url);
    bool HandleTitleBarClick(float dip_x, float dip_y);
    bool HandleSearchBarClick(float dip_x, float dip_y, const PaneLayout& layout, bool is_double_click);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& layout);
    void HandleOverlayButtonClick(const OverlayButtonHit& hit);
    // ホバー中ブロックの水平スクロールバー上ならドラッグを開始して true。
    bool TryStartBlockHScrollDrag(const MdPaneHitContext& hit_ctx, float dip_x);
    void HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout);
    void HandleFileEntryClick(const FileEntry& entry);
    void RefreshFilePane();

    // ---- ホバー (app_mouse.cpp) ----
    // カーソルがクライアント領域にあれば、その位置のホバーを引き直す。
    void RefreshHoverAtCursor();
    // ゾーン内のホバーを hover に書き、表示するカーソルを返す。状態の反映は reducer が行う。
    HCURSOR HoverMdPane(float dip_x, float dip_y, int px, int py, const PaneLayout& layout, PointerHover& hover) const;
    HCURSOR HoverSidePane(PaneTarget target, float dip_x, float dip_y, const PaneLayout& layout, PointerHover& hover) const;
    TooltipTarget BuildSidePaneTooltip(PaneTarget target, PaneHeaderButton hit, int idx) const;
    TooltipTarget BuildMdContentTooltip(int hit_node, std::optional<LinkHit> link) const;

    // ---- レイアウト ----
    float MdMaxScroll();
    // 計測や図の確定で内容が縮んだら、scroll_y を上限に収める。遅延レイアウトの途中では呼ばない
    // (後続の計測で戻る中間的な上限で、見ている位置を引き下げないため)。
    void ClampScrollToContent();
    // 可視範囲を計測し、変わればスクロール上限と遅延レイアウトを追従させる。
    void EnsureFrameLayout(const PaneLayout& layout);
    // dirty が残っていれば、空き時間の遅延レイアウトに回して true。
    bool RequestDeferredLayout();
    void SettleLayout();
    // 可視範囲の図と画像の取りに行き方。
    enum class RelayoutResources : uint8_t {
        Diagrams,          // 図を今すぐ描き直す (テーマ・ズーム)
        DiagramsBatched,   // 図のディスク読み込みを時間予算付きバッチに回す (リサイズ)
        ImagesAndDiagrams, // 画像も読み込む (ロード・リロード)
    };
    // 可視範囲を計測し直し、スクロール上限・図と画像・遅延レイアウト・目次を追従させる。
    void RelayoutViewport(RelayoutResources resources);
    void OnResizeEnd();
    void OnDeferredLayout();
    // 捨てたテキストレイアウトが多ければワーカーで解放する。
    void DisposeReleasedLayouts();
    void SyncTocActiveAndAutoScroll(bool auto_scroll);
    float MdContentWidth();
    // スクロール上限/スクロールバー計算用のコンテンツ高さ。
    float ScrollableContentHeight() const noexcept;

    // ---- ファイル読み込み (app_file.cpp) ----
    void LoadHelpDocument();
    // 表示中の言語のヘルプに差し替える。
    void ShowHelpResource();
    void LoadMarkdownFile(std::wstring_view path);
    void CancelOtherFileLoad();
    // 表示文書とレイアウトキャッシュを一緒に差し替え、旧文書の破棄を worker に回す。
    // estimated は worker が MakeEstimatedLayoutCache で作ったもの。省略時は UI スレッドで推定する。
    void ReplaceDocument(Document next, LayoutCache estimated);
    void ReplaceDocument(Document next);
    void ReloadCurrentFile();
    // reload_base は同一ファイルのリロード時の現在の文書 (worker で差分判定させる)。
    void BeginAsyncLoad(std::pmr::wstring path, bool show_animation, ReloadBase reload_base = {});
    void StartLoadingAnimation();
    // 読み込みが LOADING_SPINNER_DELAY_MS より長引いたらスピナーを出す。
    void ScheduleLoadingAnimation();
    void StopLoadingAnimation();
    // StartPreloadAsync が Init より前に呼ぶことがある。2 回目は何もしない。
    void InitSharedScheduler();
    void FinishLoadMarkdownFile(bool follow_file_pane);
    // error があればトーストで知らせる。
    void HandleLoadFailureFallback(std::optional<FileLoadError> error);
    // nodes の範囲の推定高さを、図と画像のキャッシュにある実測の寸法で上書きする。変えたら true (Y は組み直さない)。
    // 呼び出し前にノード高さの初期化 (EstimateNodeHeights) は完了していること。
    bool ApplyCachedHeights(VisibleRange nodes);

    // carry は ReloadDocument の結果。
    void FinishReload(size_t diff_pos, const layout::CarryOverResult& carry);

    enum class ReloadFlow : uint8_t {
        Handled,            // ResumeWatching / DeferReloadRetry 済み、呼び出し元は return
        ContinueWithReload, // Replace を呼び出し元で実行する
    };
    // NoChange / Defer* をここで処理し、Replace は呼び出し元に任せる。
    // has_parsed_doc は新しい内容のパース済み文書が手元にあるか (prefix shrink を受け入れられるか)。
    ReloadFlow ApplyReloadDecisionEarly(const ReloadDecision& decision, bool has_parsed_doc);

    // 短縮リトライで再リロードを予約する。エディタの truncate→rewrite 中や
    // partial-read を検出した時に共通で使う。
    void DeferReloadRetry();
    // テキスト選択のドラッグ中なら、リロードをドラッグの終わりまで待たせて true。
    bool DeferReloadWhileSelecting();

    void CancelPendingResources();
    // リロード用。中身が変わらないノードの計測結果を引き継いでから差し替える。
    // ends は worker が数えた変わらなかったノード数。無ければここで数える。Y の組み直しは FinishReload に任せる。
    layout::CarryOverResult ReloadDocument(Document next, LayoutCache estimated, std::optional<UnchangedEnds> ends);
    void ResetViewForNewDocument();
    void EndDocumentBoundDrag();

    // ---- テーマ ----
    void ApplyZoom();
    void ToggleDarkMode();

    // ---- 設定 ----
    void ShowSettingsDialog();
    PopupEnv MakePopupEnv() const noexcept;
    // 設定ダイアログで選んだ言語を、開いている画面にすぐ反映する。
    void ApplyLanguage(const i18n::Strings* chosen);

private:
    HWND hwnd_ = nullptr;

    SystemCursors cursors_;
    Tooltip tooltip_;
    bool in_modal_loop_ = false;

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
    bool deferred_layout_pending_ = false;
    // 列挙スレッドの結果の受け渡し。スレッドは App より長生きしうるので共有で持たせる。
    struct FileListingSlot {
        std::mutex mutex;
        std::optional<std::pair<uint32_t, std::pmr::vector<FileEntry>>> result;
    };
    std::shared_ptr<FileListingSlot> file_listing_ = std::make_shared<FileListingSlot>();

    AppState state_;

    LayoutService layout_service_{ renderer_.GetLayout(), state_.view.viewport };
    AppHost host_{ *this };
    ResourceManager resource_manager_;
};
