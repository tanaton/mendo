#pragma once
#include "app_controller.h"
#include "app_events.h"
#include "app_state.h"
#include "async_load_coordinator.h"
#include "clipboard_manager.h"
#include "config_service.h"
#include "context_menu.h"
#include "file_watcher.h"
#include "image_loader.h"
#include "layout.h"
#include "mermaid.h"
#include "mermaid_file_cache.h"
#include "reducer.h"
#include "document_utils.h"
#include "dwrite_measurer.h"
#include "render_params.h"
#include "renderer.h"
#include "resource_manager.h"
#include "settings_dialog.h"
#include "side_effect.h"
#include "task_scheduler.h"
#include "theme_service.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// App が本体のウィンドウ (Win32Window) に頼むこと。ポップアップのモーダルループも回させる。
// どれも呼び出し中に App::Dispatch へ入り直しうる (フォーカスの移動や入力欄の書き換えの通知)。
class WindowHost : public ModalLoop {
public:
    // 検索の入力欄 (非表示の EDIT) にフォーカスを移し、[anchor, caret) を選択する。caret が -1 なら末尾まで。
    virtual void FocusSearchEdit(int anchor, int caret) = 0;
    // 本体にフォーカスを戻す。clear_text なら入力欄を空にする (文書の切り替え)。
    virtual void UnfocusSearch(bool clear_text) = 0;

protected:
    ~WindowHost() = default;
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
    HCURSOR Get(app_controller::PointerCursor cursor) const noexcept
    {
        switch (cursor) {
        case app_controller::PointerCursor::Arrow:
            return arrow;
        case app_controller::PointerCursor::IBeam:
            return ibeam;
        case app_controller::PointerCursor::Hand:
            return hand;
        case app_controller::PointerCursor::SizeWE:
            return size_we;
        }
        std::unreachable();
    }
};

// ResourceManager と reducer が問い合わせる窓口も App が受ける。
class App final : private ResourceManagerHost, private NodeLayoutAccess {
public:
    App(ConfigService& config, WindowHost& window) noexcept : config_(config), window_(window)
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
    void StartPreloadAsync(std::wstring path);

    // LoadInitialDocument より前に呼ぶこと。起動時の読み込みの結果を取り込むときに復元情報を参照する。
    // 保存したスクロール位置を、文書を開いたときに戻す位置にする。
    void RestoreScrollPosition();

    // 状態を変えるだけの Win32 メッセージは、Win32Window がアクションにしてここへ直接渡す。
    void Dispatch(const AppAction& action);

    void OnPaint();
    void OnMinimize();
    void OnMouseWheel(int px, int py, int delta, bool ctrl);
    // 割り当ての無いキーは false を返し、ウィンドウ側の既定の処理に回させる。
    bool OnKeyDown(WPARAM key);
    bool OnSearchEditKeyDown(WPARAM key);
    void OnDropFiles(HDROP hDrop);

    // is_double_click は WM_LBUTTONDBLCLK (連続クリックの 2 回目)。
    void OnLButtonDown(int px, int py, bool is_double_click);
    void OnLButtonUp(int px, int py);
    void OnMouseMove(int px, int py);
    void OnContextMenu(int screen_x, int screen_y);
    bool OnRButtonDown(int px, int py);
    bool OnRButtonUp(int px, int py);
    void OnRButtonMove(int px, int py);

    void OnMouseHover(int px, int py);
    // ポップアップ (PopupWindow) やシステムメニュー (アイコンのクリック・Alt+Space・キャプションの右クリック) の
    // モーダルループに入った・出た (WM_ENTERMENULOOP / WM_EXITMENULOOP)。
    // 開いている間は本体に入力が来ないので、先にホバーとツールチップを消し、裏で本体を描き直しても出さない
    // (RefreshHoverAtCursor)。
    void OnModalLoop(bool entered)
    {
        in_modal_loop_ = entered;
        if (entered) {
            Dispatch(MouseLeaveAction{});
        }
    }
    // 開いている間に届いた右クリックで、ジェスチャーや右クリックメニューを重ねて始めない。
    constexpr bool IsInModalLoop() const noexcept
    {
        return in_modal_loop_;
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
    // 起動時の読み込みの結果をまだ取り込んでいない。
    bool IsLoadingInitialDocument() const noexcept
    {
        return load_coordinator_.IsActive();
    }
    // 前のフレームの表示を待つ間に呼ぶ。次の WM_PAINT で同期計測するはずの可視範囲を先に計測し、
    // 計測時間を vblank 待ちに隠す。
    void PrepareFrame();
    void OnAppImageLoaded();
    void OnParseComplete();
    void OnMermaidDiskLoaded();
    void OnInlineMathRequested();
    void OnFileListed();
    void OnDestroy();

    constexpr bool IsSearchBarVisible() const noexcept
    {
        return state_.search.search_state.IsVisible();
    }
    RECT GetSearchEditRect();

    constexpr float GetDpiScale() const noexcept
    {
        return state_.window.cached_dpi_scale;
    }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept
    {
        return state_.window.titlebar.HitTest(dip_x, dip_y);
    }
    bool IsOverMdScrollbar(float dip_x, float dip_y);

private:
    // reducer が返した副作用を実行する。
    void Execute(const SideEffectList& effects)
    {
        for (const auto& e : effects) {
            Execute(e);
        }
    }
    void Execute(const SideEffect& effect);
    bool DispatchKeyAction(const AppAction& action);
    void SetTimer(app_timer::Id id, UINT ms) noexcept override
    {
        ::SetTimer(hwnd_, std::to_underlying(id), ms, nullptr);
    }
    void KillTimer(app_timer::Id id) noexcept override
    {
        ::KillTimer(hwnd_, std::to_underlying(id));
    }
    void ShowToast(std::wstring_view message);
    // 関連付けの解決やブラウザの起動で数百 ms 止まりうるので、UI スレッドを待たせずに開く。
    static void ShellOpenAsync(std::wstring target);
    // 変更通知はデバウンスしてから読み直す。
    void StartFileWatch(FileWatchTarget target);
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

    // ---- 描画 ----
    void UpdateTitleBar();
    // ファイルペインが表示中なら、一覧の列挙も始める。
    SidePaneState BuildSidePaneState(const PaneLayout& layout);
    TitleBarRenderState BuildTitleBarRenderState() const;

    // ---- 終了 ----
    void SaveSession();

    // ---- 座標・ヒットテスト ----
    DipPoint PixelToDip(int px, int py) const noexcept;

    using HitResult = hit_test::HitResult;
    HitResult HitTest(int px, int py);

    const PaneLayout& GetPaneLayout();
    PaneZone PaneAtPoint(float dip_x);

    // 検索の入力欄で dip_x にあるキャレット位置。
    int SearchInputPosAt(float dip_x);

    // ---- クリック (判定は app_controller::Resolve*Hit) ----
    void HandleLinkClick(std::string_view url);
    bool HandleTitleBarClick(float dip_x, float dip_y);
    void HandleSearchBarClick(SearchBarHitZone zone, float dip_x, bool is_double_click);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, bool is_double_click);
    void HandleOverlayButtonClick(const OverlayButtonHit& hit);
    // ホバー中ブロックの水平スクロールバー上ならドラッグを開始して true。
    bool TryStartBlockHScrollDrag(const MdPaneHitContext& hit_ctx, float dip_x);
    void HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, bool is_double_click);
    void HandleFileEntryClick(const FileEntry& entry);
    void RefreshFilePane();

    // ---- ホバー (判定は app_controller::ResolvePointerHover) ----
    // カーソルがクライアント領域にあれば、その位置のホバーを引き直す。
    void RefreshHoverAtCursor();

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
    // 型を描画側に渡し、寸法を待って原文のまま組んだ段落を組み直す。
    void OnInlineMathRendered(InlineMathBatchResult result);
    // 描いた数式の型と寸法を捨てる。全レイアウトを組み直すとき (ズーム・別の文書) に呼ぶ。
    void ForgetInlineMath();
    void SettleViewport();
    void OnDeferredLayout();
    // 捨てたテキストレイアウトが多ければワーカーで解放する。
    void DisposeReleasedLayouts();
    float MdContentWidth() override;
    float MdPaneHeight() override;
    const Theme& GetTheme() override;
    // 画像・図の高さが変わった範囲の Y を組み直し、スクロールを収めて描き直す。
    void RecomputeLayout(layout::HeightChangeRange changed) override;
    void EnsureNodeLayout(int node_index) override;

    // ---- ファイル読み込み ----
    // 表示中の言語のヘルプに差し替える。ヘルプを表示中でも読み直す (言語の切り替え)。
    // ヘルプに留まる操作 (表示中に F1・履歴で同じヘルプへ) は reducer が LoadFile を出さずに済ませる。
    void ShowHelpResource();
    void LoadMarkdownFile(std::wstring_view path);
    void CancelOtherFileLoad();
    // 別の文書を表示する。estimated は next 向けに推定済みのキャッシュ (worker が MakeEstimatedLayoutCache で作ったもの)。
    // ビュー状態の後始末は DocumentOpenedAction で reducer に任せる。
    // watch は next の変更の監視 (worker が OpenWatchTarget で作ったもの)。ヘルプは空で、監視をやめる。
    void OpenDocument(Document next, LayoutCache estimated, bool follow_file_pane, FileWatchTarget watch);
    // 同じ文書の新しい版に差し替える。中身が変わらないノードの計測結果を引き継ぎ、diff_pos の近くが見えるようにスクロールする。
    // ends は worker が数えた変わらなかったノード数。後始末は DocumentReloadedAction / ReloadSettledAction で reducer に任せる。
    void ReloadDocument(Document next, LayoutCache estimated, const UnchangedEnds& ends, size_t diff_pos);
    // 表示文書とレイアウトキャッシュを一緒に差し替え、旧文書の破棄を worker に回す。OpenDocument / ReloadDocument の中だけで使う。
    void ReplaceDocument(Document next, LayoutCache estimated);
    void ReloadCurrentFile();
    // 表示中のファイルなら開き直しも含めてリロードとして読む (ReloadBaseFor)。
    void BeginAsyncLoad(std::wstring path, bool show_animation);
    void StartLoadingAnimation();
    // 読み込みが LOADING_SPINNER_DELAY_MS より長引いたらスピナーを出す。
    void ScheduleLoadingAnimation();
    void StopLoadingAnimation();
    // StartPreloadAsync が Init より前に呼ぶことがある。2 回目は何もしない。
    void InitSharedScheduler();
    // error があればトーストで知らせる。
    void HandleLoadFailureFallback(std::optional<FileLoadError> error);
    // nodes の範囲の推定高さを、図と画像のキャッシュにある実測の寸法で上書きする。変えたら true (Y は組み直さない)。
    // 呼び出し前にノード高さの初期化 (EstimateNodeHeights) は完了していること。
    bool ApplyCachedHeights(VisibleRange nodes);

    // 短縮リトライで再リロードを予約する。エディタの truncate→rewrite 中や
    // partial-read を検出した時に共通で使う。
    void DeferReloadRetry();
    // テキスト選択のドラッグ中なら、リロードをドラッグの終わりまで待たせて true。
    bool DeferReloadWhileSelecting();

    void CancelPendingResources();
    // 読み込みの開始時に捨てた画像・図の待ちを、文書を差し替えずに終わったら (失敗・取り消し・開き直し・
    // 読み直しの見送り) 表示を続ける文書のぶんだけ要求し直す。
    void RequestDroppedResourcesAgain();

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
    WindowHost& window_;
    // SetWindowTextW は変わったときだけ呼ぶ。
    std::wstring title_text_ = L"mendo";
    // 空なら非表示。フェードは毎フレームの全画面再描画を伴うので、表示と消去の 2 回だけ描き直す。
    std::wstring toast_message_;
    // 別のファイルを読み始めたときに旧文書の画像・図の待ちを捨てた。文書を差し替えたら外す。
    bool resources_dropped_for_load_ = false;

    Renderer renderer_;
    TaskScheduler scheduler_;
    TaskScheduler layout_scheduler_;
    MermaidFileCache file_cache_; // mermaid_renderer_ より先に宣言する（mermaid_renderer_ は破棄時に file_cache_ を参照する）
    ClipboardManager clipboard_manager_; // mermaid_renderer_ より先に宣言する（~MermaidRenderer の CancelPending が in-flight コールバック経由で clipboard_manager_ に触るため、後に破棄させる）
    // インライン数式の寸法表。計測ワーカーが引き、描画の結果を OnInlineMathRendered で書く。
    InlineMathTable inline_math_;
    MermaidRenderer mermaid_renderer_;
    ImageLoader image_loader_;
    FileWatcher file_watcher_;
    ConfigService& config_;
    ThemeService theme_service_{ config_ };
    // 起動時の読み込み (preload) も Init より前にここで始め、通知先は LoadInitialDocument の Attach で渡す。
    AsyncLoadCoordinator load_coordinator_;
    // スピナーは読み込みが長引いたときだけ出し、出ている間はタイマーごとに回す。
    bool spinner_shown_ = false;
    float spinner_angle_ = 0.0f;
    PrefixShrinkRetry prefix_shrink_retry_;
    bool deferred_layout_pending_ = false;
    // 列挙スレッドの結果の受け渡し。スレッドは App より長生きしうるので共有で持たせる。
    struct FileListingSlot {
        std::mutex mutex;
        std::optional<std::pair<uint32_t, std::vector<FileEntry>>> result;
    };
    std::shared_ptr<FileListingSlot> file_listing_ = std::make_shared<FileListingSlot>();

    AppState state_;

    // テーマは theme_service_ のものを参照する。
    DWriteTextMeasurer measurer_;
    LayoutEngine layout_engine_;
    LayoutService layout_service_{ layout_engine_, state_.view.viewport };
    ResourceManager resource_manager_;
};
