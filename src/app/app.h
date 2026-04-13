#pragma once
#include "app_state.h"
#include "reducer.h"
#include "side_effect_executor.h"
#include "renderer.h"
#include "task_scheduler.h"
#include "mermaid_file_cache.h"
#include "mermaid.h"
#include "image_loader.h"
#include "file_watcher.h"
#include "document_service.h"
#include "document_utils.h"
#include "pane.h"
#include "layout_service.h"
#include "app_controller.h"
#include "config_service.h"
#include "theme_service.h"
#include "file_load_service.h"
#include "resource_manager.h"
#include "session_service.h"
#include "cursor_manager.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <memory_resource>

// ウィンドウのタイトルバーとスクロールバーにダークモードスタイルを適用する。
void ApplyDarkModeToWindow(HWND hwnd, bool dark);

class App {
public:
    bool Init(HWND hwnd);

    void LoadMarkdownFile(std::wstring_view path);
    void LoadHelpDocument();
    std::pmr::wstring LoadLastFilePath() const;
    void ShowDirectory(std::wstring_view dir_path);

    // ヘルプ用仮想パス（document_utils.h の定義を委譲）
    static constexpr std::wstring_view HELP_PATH = ::HELP_PATH;
    static bool IsHelpPath(std::wstring_view path) noexcept { return ::IsHelpPath(path); }

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
    void OnXButtonBack();
    void OnXButtonForward();

    // ファイル変更イベント（メッセージループから呼ばれる）
    HANDLE GetFileWatchEvent() const noexcept { return doc_service_.GetFileWatchEvent(); }
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
    void OnSearchTextChanged(std::wstring_view text) { Dispatch(SearchTextChangedAction{ std::pmr::wstring{text} }); }
    void OnSearchClose() { Dispatch(CloseSearchBarAction{}); }
    void OnSearchNext() { Dispatch(SearchNextAction{}); }
    void OnSearchPrev() { Dispatch(SearchPrevAction{}); }
    bool IsSearchBarVisible() const noexcept { return state_.search_state.IsVisible(); }
    void OnToggleCaseSensitive() { Dispatch(ToggleCaseSensitiveAction{}); }
    void OnToggleHighlight() { Dispatch(ToggleHighlightAction{}); }
    void SetSearchSelection(int sel_start, int sel_end) { Dispatch(SearchSelectionAction{ sel_start, sel_end }); }
    void SetImeComposition(std::wstring_view comp) { Dispatch(ImeCompositionAction{ std::pmr::wstring{comp} }); }
    RECT GetSearchEditRect() const;

    // 検索バーコントローラへのアクセス（app_mouse.cppでのドラッグ/ホバー処理用）
    SearchBarController& GetSearchBarCtrl() noexcept { return state_.search_bar_ctrl; }

    // 前回セッションのスクロール位置復元用（LoadMarkdownFileの前に呼ぶ）
    void SetPendingRestoreNode(int node, int offset, float scroll_y = -1.0f) noexcept
    {
        state_.scroll_restore.SetNodeRestore(node, offset, scroll_y);
    }

    // サイズ変更状態 — Reducer経由で状態変更
    void OnEnterSizeMove() { Dispatch(EnterSizeMoveAction{}); }
    void OnExitSizeMove() { Dispatch(ExitSizeMoveAction{}); }

    // WM_SETCURSOR用のカーソル状態
    bool IsRenderReady() const noexcept { return renderer_.GetRenderTarget() != nullptr; }

    // ウィンドウ全体の再描画を要求する
    void Invalidate() noexcept { InvalidateRect(hwnd_, nullptr, FALSE); }

    // 指定ペイン領域のみ再描画を要求する
    void InvalidatePane(const PaneRect& rect) noexcept;

    // タイトルバー領域のみ再描画を要求する
    void InvalidateTitleBar() noexcept;

    // Win32Windowのカーソル/再描画用にDPIスケールを公開
    constexpr float GetDpiScale() const noexcept { return state_.cached_dpi_scale; }

    // カスタムタイトルバー
    float GetTitleBarHeightDip() const noexcept { return state_.titlebar.GetHeight(); }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept { return state_.titlebar.HitTest(dip_x, dip_y); }
    bool IsOverMdScrollbar(float dip_x, float dip_y) const;
    bool IsOverMdScrollbar(float dip_x, float dip_y, const ::PaneLayout& layout) const noexcept;
    void OnActivate(bool active) { Dispatch(ActivateAction{ active }); }

private:
    // AppControllerが返すアクションを実行
    void Dispatch(const AppAction& action);

    // Init用コールバック構築ヘルパー
    ResourceManager::Callbacks BuildResourceManagerCallbacks();
    SearchBarController::Callbacks BuildSearchBarCallbacks();

    // アンカーベースのスクロール位置保存/復元 (AnchorState は app_state.h で定義)
    AnchorState SaveAnchor() const;
    void RestoreAnchor(const AnchorState& anchor, float md_pane_height);
    void RestoreAnchorWithScale(const AnchorState& anchor, float offset_scale);

    // DIP変換
    struct DipPoint { float x, y; };
    DipPoint PixelToDip(int px, int py) const noexcept;

    // ヒットテスト
    using HitResult = HitTestService::HitResult;
    HitResult HitTest(int screen_x, int screen_y) const;
    std::optional<std::pmr::wstring> GetLinkAtHit(const HitResult& hit) const;

    // リンク・アンカーナビゲーション
    void HandleLinkClick(std::wstring_view url);
    void NavigateToAnchor(std::wstring_view anchor);
    void PushNavHistory();

    // クリップボード・選択
    void SetClipboardText(std::wstring_view text) const;
    void CopySelectionToClipboard() const;
    void CopyCodeBlockToClipboard(int node_index) const;
    void SaveDiagramAsPng(int node_index);

    // クリックハンドラ (OnLButtonDownから抽出)
    bool HandleTitleBarClick(float dip_x, float dip_y);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& layout);
    void HandleFilePaneClick(float dip_x, float dip_y, const PaneLayout& layout);
    void HandleTocPaneClick(float dip_x, float dip_y, const PaneLayout& layout);
    bool TryHandlePaneScrollbarClick(float dip_x, float dip_y, const PaneRect& rect,
        PaneController::DragTarget target,
        const PaneScrollInfo& scroll_info,
        float total_content, ScrollState& scroll,
        void (Renderer::* invalidate)());

    // スクロールバーヘルパー
    PaneScrollInfo ComputePaneScrollInfo(const PaneRect& rect, float total_content) const;
    void HandleScrollbarClick(float dip_y, const PaneScrollInfo& info,
        ScrollState& scroll, bool& cache_dirty);
    void HandleScrollbarDrag(float dip_y, const PaneScrollInfo& info,
        ScrollState& scroll, bool& cache_dirty);

    // サイドペイン スクロールバードラッグ共通処理
    void HandleSidePaneScrollDrag(float dip_y, const PaneRect& rect,
        float total_content, ScrollState& scroll,
        void (Renderer::* invalidate)());

    // レイアウト / スクロール
    void ScheduleDeferredLayoutIfNeeded();
    void UpdateScrollBar();
    void InvalidateMdPane(const PaneRect& md_rect);
    void InvalidateHitPositions() noexcept;
    void ScrollTo(float position);
    void SyncMaxScroll(float md_pane_height);
    int FindFirstVisibleNode() const noexcept;
    void OnResizeEnd();
    void RefreshPaneLayout();
    void RefreshFilePane();
    void OnDeferredLayout();

    // ファイル読み込み (file_load_service_に委譲)
    void ReloadCurrentFile();
    void DoReloadCurrentFile();
    void DoLoadMarkdownFile();
    void BeginAsyncLoad(const std::pmr::wstring& path);
    void FinishLoadMarkdownFile(bool heights_estimated = false);
    float CalcScrollForDiff(size_t diff_pos, float viewport_height, float fallback_scroll) const;
    void ApplyMermaidCacheHeights(float md_width);
    bool ShouldDeferForTruncateRewrite(bool is_prefix_only, size_t old_size, size_t new_size);
    void UpdateTitleBar();

    // リロード共通処理: 差分分析後のレイアウト更新・スクロール復元・検索再実行
    void FinishReload(bool is_prefix_only, size_t diff_pos, float old_scroll);

    // ファイル読み込み/リロード共通ヘルパー
    void CancelPendingResources();
    void FinalizeLayout(float md_pane_height);
    void SaveLastFilePath();
    void SavePaneState();
    void LoadPaneState();
    void SaveScrollPosition();

    // ペインレイアウト（結果はキャッシュされる）
    const ::PaneLayout& GetPaneLayout() const;
    void InvalidatePaneLayoutCache() noexcept { state_.pane_layout_valid = false; }
    ::PaneZone PaneAtPoint(float dip_x, float dip_y) const;
    float GetMarkdownPaneWidth() const;

    // Reducer 用テーマ定数キャッシュの同期
    void SyncPaneThemeCache();

    // ダークモード / ズーム (theme_service_に委譲)
    void HandleApplyThemeChange(const effect::ApplyThemeChange& e);

    // テーマ/ズーム変更後の共通後処理（ViewportLayout→スクロール復元→再描画）
    void FinishThemeOrZoomChange(const AnchorState& anchor, float offset_scale);

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
    ConfigService config_;
    ThemeService theme_service_{ config_ };
    SessionService session_{ config_ };
    FileLoadService file_load_service_{ doc_service_ };

    // ---- 全状態を集約 ----
    AppState state_;

    // ---- サービス（状態ではなく振る舞い） ----
    std::optional<LayoutService> layout_service_;
    ResourceManager resource_manager_;
    SideEffectExecutor effect_executor_;

    // NavButtonHover エイリアス
    using NavButtonHover = HitTestService::NavButtonHover;

    void ShowToast(std::wstring_view message);
    void UpdateTooltip(const TooltipTarget& target, int px, int py);
    void ClearTooltip();
};
