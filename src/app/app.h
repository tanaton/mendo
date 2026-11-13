#pragma once
#include "app_controller.h"
#include "app_events.h"
#include "app_state.h"
#include "async_load_coordinator.h"
#include "clipboard_manager.h"
#include "config_service.h"
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
#include "side_effect.h"
#include "task_scheduler.h"
#include "theme_service.h"
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace i18n {
struct Strings;
}

class Win32Window;

struct SystemCursors {
    // PointerCursor の順。
    std::array<HCURSOR, 4> cursors{};

    void Load() noexcept
    {
        cursors = { LoadCursorW(nullptr, IDC_ARROW), LoadCursorW(nullptr, IDC_IBEAM), LoadCursorW(nullptr, IDC_HAND), LoadCursorW(nullptr, IDC_SIZEWE) };
    }
    HCURSOR Get(app_controller::PointerCursor cursor) const noexcept
    {
        return cursors[std::to_underlying(cursor)];
    }
};

// ResourceManager と reducer が問い合わせる窓口も App が受ける。
class App final : private ResourceManagerHost, private NodeLayoutAccess {
public:
    App(ConfigService& config, Win32Window& window) noexcept : config_(config), window_(window)
    {}
    bool Init(HWND hwnd);
    // Init が保存済みの設定 (ペインなど) を読み終えた。終える前に閉じるなら、窓の配置もセッションも保存しない。
    // 読んでいない既定値で保存済みの設定を上書きしてしまう。
    bool IsInitialized() const noexcept
    {
        return initialized_;
    }

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
    // RestoreScrollPosition と同じく LoadInitialDocument より前に呼ぶ。起動時の文書を開いたら見出し (--anchor) へ飛ぶ。
    void SetInitialAnchor(std::wstring_view anchor);

    // 状態を変えるだけの Win32 メッセージは、Win32Window がアクションにしてここへ直接渡す。
    void Dispatch(const AppAction& action);

    void OnPaint();
    void OnMinimize();
    // WM_ACTIVATEAPP。自分のポップアップや図の拡大表示への切り替えでは来ない。
    void OnActivateApp(bool active);
    void OnMouseWheel(int px, int py, int delta, bool ctrl);
    // 割り当ての無いキーは false を返し、ウィンドウ側の既定の処理に回させる。
    bool OnKeyDown(WPARAM key, LPARAM flags);
    bool OnSearchEditKeyDown(WPARAM key, LPARAM flags);
    void OnDropFiles(HDROP hDrop);

    // is_double_click は WM_LBUTTONDBLCLK (連続クリックの 2 回目)。
    void OnLButtonDown(int px, int py, bool is_double_click);
    void OnLButtonUp(int px, int py);
    void OnMButtonDown(int px, int py);
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

    // 文書と本文中の画像の監視のイベントを out に入れ、数を返す。out は kMaxFileWatchEvents 以上。
    static constexpr size_t kMaxFileWatchEvents = 1 + ResourceManager::kMaxImageWatchEvents;
    size_t GetFileWatchEvents(std::span<HANDLE> out) const noexcept
    {
        size_t count = 0;
        if (const HANDLE doc = file_watcher_.GetEventHandle()) {
            out[count++] = doc;
        }
        return count + resource_manager_.ImageWatchEvents(out.subspan(count));
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
    void OnMermaidJobDone();
    void OnInlineMathRequested();
    void OnTableMeasured();
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
    void Execute(const SideEffectList& effects)
    {
        for (const auto& e : effects) {
            Execute(e);
        }
    }
    void Execute(const SideEffect& effect);
    bool DispatchKeyAction(const std::optional<AppAction>& action);
    void SetTimer(app_timer::Id id, UINT ms) noexcept override
    {
        ::SetTimer(hwnd_, std::to_underlying(id), ms, nullptr);
    }
    void KillTimer(app_timer::Id id) noexcept override
    {
        ::KillTimer(hwnd_, std::to_underlying(id));
    }
    bool IsLoadingOtherDocument() const noexcept override
    {
        return reload_.IsLoadingOther();
    }
    void ShowToast(std::wstring_view message);
    // 関連付けの解決やブラウザの起動で数百 ms 止まりうるので、UI スレッドを待たせずに開く。
    static void ShellOpenAsync(std::wstring target);
    // 変更の通知は reload_ に渡す。
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

    PaneZone PaneAtPoint(float dip_x);
    // タイトルバーより下の MD ペインの上か。
    bool IsOverMdPane(DipPoint dip);

    // 検索の入力欄で dip_x にあるキャレット位置。
    int SearchInputPosAt(float dip_x);

    // ---- クリック (判定は app_controller::Resolve*Hit) ----
    // new_window: Ctrl+クリック・中クリック。ローカルの文書を新しいウィンドウで開く。
    void HandleLinkClick(std::string_view url, bool new_window);
    bool HandleTitleBarClick(float dip_x, float dip_y);
    void HandleSearchBarClick(SearchBarHitZone zone, float dip_x, bool is_double_click);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, bool is_double_click);
    void HandleOverlayButtonClick(const OverlayButtonHit& hit);
    // ホバー中ブロックの水平スクロールバー上ならドラッグを開始して true。
    bool TryStartBlockHScrollDrag(const MdPaneHitContext& hit_ctx, float dip_x);
    void HandleSidePaneClick(PaneTarget target, float dip_x, float dip_y, bool is_double_click);
    void HandleFileEntryClick(const FileEntry& entry);
    // 見出し・スクロールバーの上は無し。
    const FileEntry* FileEntryAt(float dip_x, float dip_y);
    void RefreshFilePane();

    // ---- ホバー (判定は app_controller::ResolvePointerHover) ----
    // カーソルがクライアント領域にあれば、その位置のホバーを引き直す。
    void RefreshHoverAtCursor();

    // ---- レイアウト ----
    // 後始末 (FinishLayout) に渡す、何が変わったか。
    enum class LayoutChange : uint8_t {
        // レイアウトを捨てた、または文書・幅・ズーム・テーマが変わった。可視範囲を推定し直して測る。
        Remeasure,
        // 見えている範囲を組んだか、ノードの高さが変わって Y を組み直した。
        Reflowed,
        // 画面の外だけを組んだ (先読み)。スクロールの位置が変わらなければ描き直さない。
        Offscreen,
    };
    // 後始末をどこから呼ぶか。
    enum class LayoutCaller : uint8_t {
        // 入力・メッセージ・タイマー・空き時間。画像と図をすぐ取り込む。
        Message,
        // 画像と図の取り込み (ResourceManagerHost の通知) の中。取り込みに入り直さない。
        Import,
        // 描画の直前の計測 (OnPaint・PrepareFrame)。描き直させない。
        Paint,
    };
    // レイアウトが変わった後の後始末の入口。可視範囲の計測・スクロールを上限に収める・画像と図の取り込み・描き直しの
    // 組み合わせと順番はここだけで決める (目次は描画の SyncActiveTocIndex)。遅延レイアウトの途中では呼ばない
    // (後続の計測で戻る中間的な上限にスクロールを収めて、見ている位置を引き下げないため)。
    void FinishLayout(LayoutChange change, LayoutCaller caller);
    // 可視範囲を計測し、変わっていれば後始末をする。
    void EnsureFrameLayout(const PaneLayout& layout);
    // key の数式を含む組を描かせる (結果を待つ間は 1 回)。計画に無い数式なら false。
    bool RequestInlineMathChunk(uint64_t key);
    // 描画命令を描画側に渡し、寸法を待って原文のまま組んだ段落を組み直す。chunk は頼んだ組、generation は頼んだときの計画の世代。
    void OnInlineMathRendered(InlineMathBatchResult result, size_t chunk, uint64_t generation);
    // 数式か画像の寸法を待って組んだレイアウトを、見ている位置を保って捨てる。表は force_tables でなければ間引く。捨てたら true。
    bool InvalidatePendingInline(bool force_tables);
    // InvalidatePendingInline で捨てた分を組み直す。
    void RelayoutPendingInline(bool force_tables);
    // 描いた数式の描画命令と寸法を捨て、計画を作り直す。全レイアウトを組み直すとき (ズーム・別の文書) に呼ぶ。
    void ForgetInlineMath();
    // 文書か文字の大きさが変わったら作り直す。
    void RebuildInlineMathPlan();
    void OnDeferredLayout();
    // 捨てたテキストレイアウトが多ければワーカーで解放する。
    void DisposeReleasedLayouts();
    // 図の高さが変わった範囲の Y を組み直し、スクロールを収めて描き直す。
    void RecomputeLayout(IndexRange changed) override;
    void OnInlineImagesUpdated(bool pending_resolved, bool sizes_changed) override;
    void EnsureNodeLayout(int node_index) override;

    // ---- ファイル読み込み ----
    // 埋め込みの文書 (IsEmbeddedPath) を開く。ヘルプは表示中の言語のもので、表示中でも読み直す (言語の切り替え)。
    // ヘルプに留まる操作 (表示中に F1・履歴で同じヘルプへ) は reducer が LoadFile を出さずに済ませる。
    void ShowEmbeddedResource(std::wstring_view path);
    void LoadMarkdownFile(std::wstring_view path);
    void CancelOtherFileLoad();
    // 別の文書を表示する。estimated は next 向けに推定済みのキャッシュ (読み込みの worker か ShowEmbeddedResource が MakeEstimatedLayoutCache で作ったもの)。
    // ビュー状態の後始末は DocumentOpenedAction で reducer に任せる。
    // watch は next の変更の監視 (worker が OpenWatchTarget で作ったもの)。ヘルプは空で、監視をやめる。
    void OpenDocument(Document next, LayoutCache estimated, bool follow_file_pane, FileWatchTarget watch);
    // 同じ文書の新しい版に差し替える。中身が変わらないノードの計測結果を引き継ぎ、diff_pos の近くが見えるようにスクロールする。
    // ends は worker が数えた変わらなかったノード数。後始末は DocumentReloadedAction / ReloadSettledAction で reducer に任せる。
    void ReloadDocument(Document next, LayoutCache estimated, const UnchangedEnds& ends, size_t diff_pos);
    // 拡大表示している図のノード (MermaidRenderer::ViewerNode) を新しい文書に移す。差し替える前の文書と比べるので、
    // ReplaceDocument より先に呼ぶ。
    void FollowViewerNode(const Document& next, const UnchangedEnds& ends);
    // 表示文書とレイアウトキャッシュを一緒に差し替え、旧文書の破棄を worker に回す。OpenDocument / ReloadDocument の中だけで使う。
    void ReplaceDocument(Document next, LayoutCache estimated);
    void RunReloadCommand(const ReloadCommand& cmd);
    // 表示中のファイルなら開き直しも含めてリロードとして読む (ReloadBaseFor)。
    void BeginAsyncLoad(std::wstring path, bool show_animation);
    void StartLoadingAnimation();
    // 読み込みが LOADING_SPINNER_DELAY_MS より長引いたらスピナーを出す。
    void ScheduleLoadingAnimation();
    void StopLoadingAnimation();
    // 理由をトーストで知らせ、文書が無ければヘルプを出す。
    void HandleLoadFailureFallback(FileLoadError error);

    // ---- テーマ ----
    void ApplyZoom();
    void ToggleDarkMode();

    // ---- 設定 ----
    void ShowSettingsDialog();
    // Renderer の今の描画先を、図と画像のローダーへ渡す。
    void ShareRenderTarget();
    // 起動時、D3D の作成を待つ間に画像のフォルダを開き終えたら画像を頼む。
    void AwaitImageFoldersBeforeDevice();
    void OnRenderDeviceRecreated();
    PopupEnv MakePopupEnv() const noexcept;
    // 設定ダイアログで選んだ言語を、開いている画面にすぐ反映する。
    void ApplyLanguage(const i18n::Strings* chosen);

private:
    HWND hwnd_ = nullptr;

    SystemCursors cursors_;
    Tooltip tooltip_;
    bool in_modal_loop_ = false;
    bool initialized_ = false;
    // 続けて呼ぶ先読みの計測 (OnDeferredLayout) の一連。組み終えたときに、取り込みと描き直しを決める。
    struct DeferredLayoutRun {
        // 組み終えるまで画像と図の整理 (FinishLayout の BitmapManage) を待たせた。
        bool settle = false;
        // 何かを組んだ。
        bool measured = false;
        // 見えているノードに未計測のものがあった。
        bool visible_dirty = false;
        // 一連の最初の、本文のスクロールバー。
        struct Scrollbar {
            float thumb_y = 0.0f;
            float thumb_height = 0.0f;
            bool shown = false;
        };
        std::optional<Scrollbar> scrollbar;
    };
    DeferredLayoutRun deferred_layout_;
    Win32Window& window_;
    // SetWindowTextW は変わったときだけ呼ぶ。
    std::wstring title_text_ = L"mendo";
    // 空なら非表示。フェードは毎フレームの全画面再描画を伴うので、表示と消去の 2 回だけ描き直す。
    std::wstring toast_message_;

    Renderer renderer_;
    TaskScheduler scheduler_{ SharedWorkerCount(std::thread::hardware_concurrency()) };
    // Mermaid 共有の scheduler_ と詰まり合わないよう分ける。
    TaskScheduler layout_scheduler_{ LayoutWorkerCount(std::thread::hardware_concurrency()) };
    MermaidFileCache file_cache_;        // mermaid_renderer_ より先に宣言する（mermaid_renderer_ は破棄時に file_cache_ を参照する）
    ClipboardManager clipboard_manager_;
    // インライン数式の寸法表。計測ワーカーが引き、描画の結果を OnInlineMathRendered で書く。
    InlineMathTable inline_math_;
    // 本文中の画像の寸法表。計測ワーカーが引き、ResourceManager が読み込みの結果を書く。
    InlineImageTable inline_images_;
    // インライン数式の描画命令と画像のビットマップ。描画が読む。
    InlineDrawings inline_drawings_;
    // 文書の数式の組。組ごとに bg のワーカーで描く。
    InlineMathPlan inline_math_plan_;
    // 数式か画像を待つ表を組み直した時刻と、その間隔 (InvalidatePendingInline)。
    static constexpr std::chrono::milliseconds kInlineTableRelayoutInterval{ 1000 };
    std::chrono::steady_clock::time_point last_inline_table_relayout_{};
    MermaidRenderer mermaid_renderer_;
    ImageLoader image_loader_;
    FileWatcher file_watcher_{ &scheduler_ };
    ConfigService& config_;
    ThemeService theme_service_;
    // 起動時の読み込み (preload) も Init より前にここで始め、通知先は LoadInitialDocument の Attach で渡す。
    AsyncLoadCoordinator load_coordinator_{ scheduler_ };
    // 読み込みのスピナー。読み込みが長引いたときだけ出し (ViewState::loading_shown)、出ている間はタイマーごとに回す。
    // LOADING_ANIM のタイマーは Delayed (出すまでの遅延) と Spinning (コマ送り) の間だけ掛かっている。
    struct SpinnerOff {};
    struct SpinnerDelayed {};
    struct SpinnerSpinning {
        float angle = 0.0f;
    };
    std::variant<SpinnerOff, SpinnerDelayed, SpinnerSpinning> spinner_;
    std::optional<float> SpinnerAngle() const noexcept
    {
        const auto* spinning = std::get_if<SpinnerSpinning>(&spinner_);
        return spinning ? std::optional{ spinning->angle } : std::nullopt;
    }
    // 表示中のファイルの読み直し。読み込みを始める・取り消す・結果を取り出すたびに知らせる。
    ReloadScheduler reload_;
    // 列挙スレッドの結果の受け渡し。スレッドは App より長生きしうるので共有で持たせる。
    struct FileListingSlot {
        std::mutex mutex;
        std::optional<std::pair<uint32_t, std::vector<FileEntry>>> result;
    };
    std::shared_ptr<FileListingSlot> file_listing_ = std::make_shared<FileListingSlot>();

    AppState state_{ .theme = &theme_service_.GetTheme() };

    // テーマは theme_service_ のものを参照する。
    DWriteTextMeasurer measurer_;
    // 巨大な表の最初の計測。組むワーカーが measurer_ と layout_scheduler_ を使うので、それより後に宣言する (先に待って壊す)。
    TableMeasureJobs table_jobs_{ layout_scheduler_, measurer_ };
    LayoutEngine layout_engine_;
    LayoutService layout_service_{ layout_engine_, state_.view.viewport };
    ResourceManager resource_manager_{ state_, image_loader_, mermaid_renderer_, file_cache_, inline_images_, inline_drawings_, *this };
};
