#pragma once
#include "app_state.h"
#include "reducer.h"
#include "side_effect_executor.h"
#include "win32_host_impl.h"
#include "renderer.h"
#include "task_scheduler.h"
#include "mermaid_file_cache.h"
#include "mermaid.h"
#include "image_loader.h"
#include "document_service.h"
#include "document_utils.h"
#include "layout.h"
#include "app_controller.h"
#include "config_service.h"
#include "theme_service.h"
#include "file_load_service.h"
#include "resource_manager.h"
#include "cursor_manager.h"
#include "hit_test_service.h"
#include "lru_cache.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <memory_resource>

void ApplyDarkModeToWindow(HWND hwnd, bool dark);

class App {
public:
    explicit App(ConfigService& config) noexcept : config_(config) {}
    bool Init(HWND hwnd);

    void LoadMarkdownFile(std::wstring_view path);
    void LoadHelpDocument();
    std::pmr::wstring LoadLastFilePath() const;
    void ShowDirectory(std::wstring_view dir_path);

    // Win32Windowから呼び出されるイベントハンドラ
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

    // ボタン押下なしのマウスホバー処理
    void OnMouseHover(int px, int py);
    void OnMouseLeave() { Dispatch(MouseLeaveAction{}); }
    void HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const ::PaneLayout& layout);

    // マウスXボタンによるナビゲーション
    void OnXButtonBack() { Dispatch(NavigateBackAction{}); }
    void OnXButtonForward() { Dispatch(NavigateForwardAction{}); }

    // ファイル変更イベント（メッセージループから呼ばれる）
    constexpr HANDLE GetFileWatchEvent() const noexcept { return doc_service_.GetFileWatchEvent(); }
    void OnFileWatchEvent();

    // タイマーコールバック
    void HandleTimer(UINT_PTR timer_id);
    void OnAppLoadFile();
    void OnAppReloadFile();
    void OnAppImageLoaded();
    void OnParseComplete();
    void OnCaptureChanged();
    void OnDestroy();

    // 検索（Win32Windowから呼ばれるコールバック）— Reducer経由で状態変更
    void OnSearchTextChanged(std::pmr::wstring text) { Dispatch(SearchTextChangedAction{ std::move(text) }); }
    void OnSearchClose() { Dispatch(CloseSearchBarAction{}); }
    void OnSearchNext() { Dispatch(SearchNextAction{}); }
    void OnSearchPrev() { Dispatch(SearchPrevAction{}); }
    constexpr bool IsSearchBarVisible() const noexcept { return state_.search.search_state.IsVisible(); }
    void OnToggleCaseSensitive() { Dispatch(ToggleCaseSensitiveAction{}); }
    void OnToggleHighlight() { Dispatch(ToggleHighlightAction{}); }
    void SetSearchSelection(int sel_start, int sel_end) { Dispatch(SearchSelectionAction{ sel_start, sel_end }); }
    void SetImeComposition(std::pmr::wstring comp) { Dispatch(ImeCompositionAction{ std::move(comp) }); }
    RECT GetSearchEditRect();

    constexpr void SetPendingRestoreNode(int node, int offset) noexcept
    {
        state_.view.scroll_restore.SetNodeRestore(node, offset);
    }

    // サイズ変更状態 — Reducer経由で状態変更
    void OnEnterSizeMove() { Dispatch(EnterSizeMoveAction{}); }
    void OnExitSizeMove() { Dispatch(ExitSizeMoveAction{}); }

    bool IsRenderReady() const noexcept { return renderer_.GetRenderTarget() != nullptr; }
    void Invalidate() noexcept { InvalidateRect(hwnd_, nullptr, FALSE); }
    void InvalidatePane(const PaneRect& rect) noexcept;
    void InvalidateTitleBar() noexcept;
    constexpr float GetDpiScale() const noexcept { return state_.window.cached_dpi_scale; }

    // カスタムタイトルバー
    constexpr float GetTitleBarHeightDip() const noexcept { return state_.window.titlebar.GetHeight(); }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept { return state_.window.titlebar.HitTest(dip_x, dip_y); }
    bool IsOverMdScrollbar(float dip_x, float dip_y);
    bool IsOverMdScrollbar(float dip_x, float dip_y, const ::PaneLayout& layout) const noexcept;
    void OnActivate(bool active) { Dispatch(ActivateAction{ active }); }

private:
    // AppControllerが返すアクションを実行
    void Dispatch(const AppAction& action);

    // reducer を介さない経路から effect を単発発火するヘルパー。
    // App 内で発生する連鎖処理の最後で SyncTocActive 等を emit する際に使う。
    // SideEffectList を作らずに executor の単発 API へ直送し、毎呼び出しの
    // pmr vector アロケーションを避ける (ホット経路: OnDeferredLayout, OnPaint, OnSizing 等)。
    template <typename T>
    void EmitEffect(T&& e)
    {
        effect_executor_.ExecuteOne(side_effect_detail::WrapIntoDomain(std::forward<T>(e)));
    }

    // Init用コールバック構築ヘルパー
    ResourceManager::Callbacks BuildResourceManagerCallbacks();
    SearchBarController::Callbacks BuildSearchBarCallbacks();

    void EnsureScrollTarget();

    // DIP変換
    struct DipPoint { float x, y; };
    DipPoint PixelToDip(int px, int py) const noexcept;

    // ヒットテスト
    using HitResult = HitTestService::HitResult;
    HitResult HitTest(int screen_x, int screen_y);
    std::optional<std::pmr::wstring> GetLinkAtHit(const HitResult& hit) const;
    MdPaneHitContext BuildMdPaneHitContext(int px, int py, const PaneLayout& pane_layout) const noexcept;

    // リンク・アンカーナビゲーション
    void HandleLinkClick(std::wstring_view url);

    // クリップボード・選択
    void SetClipboardText(std::wstring_view text) const;
    void CopySelectionToClipboard() const;
    void CopyCodeBlockToClipboard(int node_index) const;
    void SaveDiagramAsPng(int node_index);
    void CopyDiagramAsSvg(int node_index);

    // クリックハンドラ (OnLButtonDownから抽出)
    bool HandleTitleBarClick(float dip_x, float dip_y);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& layout);
    void HandleFilePaneClick(float dip_x, float dip_y, const PaneLayout& layout);
    void HandleTocPaneClick(float dip_x, float dip_y, const PaneLayout& layout);
    static bool IsOverPaneScrollbar(float dip_x, const PaneRect& rect,
        float total_content, const PaneScrollInfo& scroll_info) noexcept;

    // スクロールバーヘルパー
    PaneScrollInfo ComputePaneScrollInfo(const PaneRect& rect, float total_content) const;

    // レイアウト / スクロール
    void ScheduleDeferredLayoutIfNeeded();
    void InvalidateMdPane(const PaneRect& md_rect);
    void InvalidateHitPositions();
    void ScrollTo(float position);
    int FindFirstVisibleNode() const noexcept;
    void OnResizeEnd();
    void RefreshPaneLayout();
    void RefreshFilePane();
    void OnDeferredLayout();

    void SyncTocActiveAndAutoScroll();

    // ファイル読み込み (file_load_service_に委譲)
    void ReloadCurrentFile();
    void DoReloadCurrentFile();
    void DoLoadMarkdownFile();
    void BeginAsyncLoad(std::pmr::wstring path);
    void FinishLoadMarkdownFile(bool heights_estimated = false);
    void HandleLoadFailureFallback();
    float CalcScrollForDiff(size_t diff_pos, float viewport_height) const;
    bool ApplyMermaidCacheHeights(float md_width);
    void UpdateTitleBar();

    void FinishReload(size_t diff_pos);

    // pending_reload_retry を確定し、NoChange / DeferPrefixShrink なら
    // ResumeFileWatch を発行して true (= 呼び出し元は早期 return) を返す。
    // PrefixGrowth / FullReload なら false を返し、呼び出し元が
    // decision.op で本格的な reload / load 処理を分岐する。
    bool ApplyReloadDecisionEarly(const ReloadDecision& decision);

    // 短縮リトライで再リロードを予約する。エディタの truncate→rewrite 中や
    // partial-read を検出した時に共通で使う。
    void DeferReloadRetry();

    // partial-read を検出したら defer して true を返す。
    bool DeferIfPartialWrite(const std::pmr::wstring& path, size_t read_size);

    void CancelPendingResources();
    void ResetViewForNewDocument();
    void FinalizeLayout(float md_pane_height);
    void SaveLastFilePath();
    void SavePaneState();
    void LoadPaneState();
    void SaveScrollPosition();

    const ::PaneLayout& GetPaneLayout();
    void InvalidatePaneLayoutCache() noexcept { state_.pane_layout_valid = false; }
    ::PaneZone PaneAtPoint(float dip_x);
    float GetMarkdownPaneWidth();
    void SyncPaneThemeCache();
    void HandleApplyThemeChange(const effect::ApplyThemeChange& e);
    void FinishThemeOrZoomChange();

private:
    // Win32ハンドル
    HWND hwnd_ = nullptr;

    CursorManager cursors_;

    // コアサービス
    Renderer renderer_;
    TaskScheduler scheduler_;
    MermaidFileCache file_cache_;         // mermaid_renderer_より先に宣言（破棄順序の保証）
    MermaidRenderer mermaid_renderer_;
    ImageLoader image_loader_;
    FileWatcher file_watcher_;
    DocumentService doc_service_{ file_watcher_ };
    AppController controller_;
    ConfigService& config_;
    ThemeService theme_service_{ config_ };
    SessionService session_{ config_ };
    FileLoadService file_load_service_{ doc_service_ };

    // ---- 全状態を集約 ----
    AppState state_;

    // ---- サービス（状態ではなく振る舞い） ----
    HitTestService hit_test_;
    std::optional<LayoutService> layout_service_;
    ResourceManager resource_manager_;
    Win32Host win32_host_;
    SideEffectExecutor effect_executor_;

    void ShowToast(std::wstring_view message);

    // SVG クリップボードコピーのセッション内 LRU キャッシュ。
    // キーは PNG キャッシュと同じ NodeDiagramHash。LatexMath は SVG コピー対象外。
    static constexpr size_t MAX_SVG_CACHE_ENTRIES = 64;
    LruCache<uint64_t, std::pmr::wstring> svg_cache_{ MAX_SVG_CACHE_ENTRIES };
    // 連続クリック中の重複リクエスト抑止
    bool svg_copy_in_flight_ = false;
};
