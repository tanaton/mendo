#pragma once
#include "renderer.h"
#include "task_scheduler.h"
#include "mermaid_file_cache.h"
#include "mermaid.h"
#include "image_loader.h"
#include "file_watcher.h"
#include "file_explorer.h"
#include "document.h"
#include "document_service.h"
#include "document_utils.h"
#include "hit_test_service.h"
#include "pane.h"
#include "pane_layout.h"
#include "pane_controller.h"
#include "titlebar.h"
#include "layout_cache.h"
#include "layout_service.h"
#include "viewport_manager.h"
#include "app_controller.h"
#include "nav_history.h"
#include "navigation_service.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "toast_notifier.h"
#include "config_service.h"
#include "theme_service.h"
#include "file_load_service.h"
#include "context_menu.h"
#include "search_state.h"
#include "search_bar_controller.h"
#include "resource_manager.h"
#include "scroll_restoration.h"
#include "session_service.h"
#include "cursor_manager.h"
#include "hover_throttle.h"
#include "tooltip.h"
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
    void OnMouseLeave();
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

    // 検索（Win32Windowから呼ばれるコールバック）
    void OnSearchTextChanged(std::wstring_view text) { search_bar_ctrl_.OnTextChanged(text, doc_.GetNodes()); }
    void OnSearchClose() { search_bar_ctrl_.OnClose(); }
    void OnSearchNext() { search_bar_ctrl_.OnNext(); }
    void OnSearchPrev() { search_bar_ctrl_.OnPrev(); }
    bool IsSearchBarVisible() const noexcept { return search_state_.IsVisible(); }
    void OnToggleCaseSensitive() { search_bar_ctrl_.OnToggleCaseSensitive(doc_.GetNodes()); }
    void OnToggleHighlight() { search_bar_ctrl_.OnToggleHighlight(); }
    void SetSearchSelection(int sel_start, int sel_end) noexcept { search_bar_ctrl_.SetSelection(sel_start, sel_end); }
    void SetImeComposition(std::wstring_view comp) { search_bar_ctrl_.SetImeComposition(comp); }
    RECT GetSearchEditRect() const;

    // 検索バーコントローラへのアクセス（app_mouse.cppでのドラッグ/ホバー処理用）
    SearchBarController& GetSearchBarCtrl() noexcept { return search_bar_ctrl_; }

    // 前回セッションのスクロール位置復元用（LoadMarkdownFileの前に呼ぶ）
    void SetPendingRestoreNode(int node, int offset, float scroll_y = -1.0f) noexcept
    {
        scroll_restore_.SetNodeRestore(node, offset, scroll_y);
    }

    // サイズ変更状態
    void OnEnterSizeMove();
    void OnExitSizeMove();

    // WM_SETCURSOR用のカーソル状態
    bool IsRenderReady() const noexcept { return renderer_.GetRenderTarget() != nullptr; }

    // ウィンドウ全体の再描画を要求する
    void Invalidate() noexcept { InvalidateRect(hwnd_, nullptr, FALSE); }

    // 指定ペイン領域のみ再描画を要求する
    void InvalidatePane(const PaneRect& rect) noexcept;

    // タイトルバー領域のみ再描画を要求する
    void InvalidateTitleBar() noexcept;

    // Win32Windowのカーソル/再描画用にDPIスケールを公開
    constexpr float GetDpiScale() const noexcept { return cached_dpi_scale_; }

    // カスタムタイトルバー
    float GetTitleBarHeightDip() const noexcept { return titlebar_.GetHeight(); }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept { return titlebar_.HitTest(dip_x, dip_y); }
    bool IsOverMdScrollbar(float dip_x, float dip_y) const;
    bool IsOverMdScrollbar(float dip_x, float dip_y, const ::PaneLayout& layout) const noexcept;
    void OnActivate(bool active);

private:
    // AppControllerが返すアクションを実行
    void ExecuteActions(const ActionList& actions);

    // Init用コールバック構築ヘルパー
    ResourceManager::Callbacks BuildResourceManagerCallbacks();
    SearchBarController::Callbacks BuildSearchBarCallbacks();

    // アンカーベースのスクロール位置保存/復元
    struct AnchorState {
        int idx = -1;
        float y_before = 0.0f;
        float offset = 0.0f;
    };
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
    void ApplyNavigateResult(const NavigationService::NavigateResult& result);
    void NavigateBack();
    void NavigateForward();
    void PushNavHistory();

    // クリップボード・選択
    void SetClipboardText(std::wstring_view text) const;
    void CopySelectionToClipboard() const;
    void CopyCodeBlockToClipboard(int node_index) const;
    void SaveDiagramAsPng(int node_index);
    void SelectAll();
    void ClearSelection();

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
        void (Renderer::*invalidate)());

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
    void FinishLoadMarkdownFile();
    float CalcScrollForDiff(size_t diff_pos, float viewport_height, float fallback_scroll) const;
    void ApplyMermaidCacheHeights(float md_width);
    bool ShouldDeferForTruncateRewrite(bool is_prefix_only, size_t old_size, size_t new_size);
    void UpdateTitleBar();

    // ファイル読み込み/リロード共通ヘルパー
    void CancelPendingResources();
    void FinalizeLayout(float md_pane_height);
    void SaveLastFilePath();
    void SavePaneState();
    void LoadPaneState();
    void SaveScrollPosition();

    // ペインレイアウト（結果はキャッシュされる）
    const ::PaneLayout& GetPaneLayout() const;
    void InvalidatePaneLayoutCache() noexcept { pane_layout_valid_ = false; }
    ::PaneZone PaneAtPoint(float dip_x, float dip_y) const;
    float GetMarkdownPaneWidth() const;


    // 検索
    void OnSearchOpen() { search_bar_ctrl_.OnOpen(doc_.GetNodes()); }

    // OnPaint用のレンダーステート構築ヘルパー
    GestureRenderState BuildGestureRenderState() const;
    SidePaneState BuildSidePaneState(const ::PaneLayout& layout) const;
    TitleBarRenderState BuildTitleBarRenderState(float window_width) const;
    ToastRenderState BuildToastRenderState() const;
    SearchBarRenderState BuildSearchBarRenderState() const;

    // ダークモード / ズーム (theme_service_に委譲)
    void ToggleDarkMode();
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    void ApplyZoom(float new_zoom);

    // テーマ/ズーム変更後の共通後処理（ViewportLayout→スクロール復元→再描画）
    void FinishThemeOrZoomChange(const AnchorState& anchor, float offset_scale);

private:
    // Win32ハンドル
    HWND hwnd_ = nullptr;
    float cached_dpi_scale_ = 1.0f;

    CursorManager cursors_;
    HoverThrottle hover_throttle_;

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

    // ドメイン状態
    Document doc_;
    LayoutCache layout_cache_;
    ViewportManager viewport_;
    std::optional<LayoutService> layout_service_;

    bool is_sizing_ = false;

    // カスタムタイトルバー
    TitleBar titlebar_;
    bool window_active_ = true;
    std::pmr::wstring cached_title_text_ = L"mendo";

    // 3ペイン状態
    FileExplorer file_explorer_;
    PaneController panes_;
    NavHistory nav_history_;
    NavigationService nav_service_{ nav_history_ };
    MouseGesture gesture_;
    SwipeDetector swipe_detector_;
    HitTestService hit_test_;

    ResourceManager resource_manager_;

    // PaneLayout キャッシュ（ウィンドウサイズ・ペイン状態が変わるまで再利用）
    mutable ::PaneLayout cached_pane_layout_{};
    mutable float cached_window_width_for_layout_ = 0.0f;
    mutable bool pane_layout_valid_ = false;

    // ナビゲーションオーバーレイ
    using NavButtonHover = HitTestService::NavButtonHover;
    NavButtonHover nav_hover_ = NavButtonHover::None;

    ScrollRestoration scroll_restore_;

    // 検索
    SearchState search_state_;
    SearchBarController search_bar_ctrl_;

    // カスタムコンテキストメニュー
    ContextMenu ctx_menu_;

    // コードブロック コピーボタン
    int hovered_copy_node_ = -1;
    // Mermaidダイアグラム 保存ボタン
    int hovered_save_node_ = -1;

    // 目次ペインの現在アクティブな見出しインデックス
    int active_toc_index_ = -1;

    // 非同期リロード時の差分スクロール用
    size_t reload_diff_pos_ = std::string_view::npos;
    float reload_old_scroll_ = 0.0f;

    // エディタの truncate→rewrite 2段階保存検出用。
    // prefix-only shrink をスキップし、次回リロードで元コンテンツとの
    // 正確な差分を検出する。
    bool pending_prefix_shrink_ = false;

    // トースト通知
    ToastNotifier toast_;
    void ShowToast(std::wstring_view message);

    // ツールチップ
    Tooltip tooltip_;
    void UpdateTooltip(const TooltipTarget& target, int px, int py);
    void ClearTooltip();
};
