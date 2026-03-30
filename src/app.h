#pragma once
#include "renderer.h"
#include "mermaid_file_cache.h"
#include "mermaid.h"
#include "image_loader.h"
#include "file_loader.h"
#include "file_explorer.h"
#include "document.h"
#include "document_service.h"
#include "hit_test_service.h"
#include "pane.h"
#include "pane_layout.h"
#include "pane_controller.h"
#include "titlebar.h"
#include "document_utils.h"
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
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory>
#include <memory_resource>
#include <unordered_map>

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
    void HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const ::PaneLayout& layout);

    // マウスXボタンによるナビゲーション
    void OnXButtonBack();
    void OnXButtonForward();

    // タイマーコールバック
    void HandleTimer(UINT_PTR timer_id);
    void OnAppLoadFile();
    void OnAppReloadFile();
    void OnAppImageLoaded();
    void OnCaptureChanged();
    void OnDestroy();

    // サイズ変更状態
    void OnEnterSizeMove();
    void OnExitSizeMove();

    // WM_SETCURSOR用のカーソル状態
    bool IsRenderReady() const noexcept { return renderer_.GetRenderTarget() != nullptr; }

    // ウィンドウ全体の再描画を要求する
    void Invalidate() noexcept { InvalidateRect(hwnd_, nullptr, FALSE); }

    // 指定ペイン領域のみ再描画を要求する
    void InvalidatePane(const PaneRect& rect) noexcept;

    // Win32Windowのカーソル/再描画用にDPIスケールを公開
    constexpr float GetDpiScale() const noexcept { return cached_dpi_scale_; }

    // カスタムタイトルバー
    float GetTitleBarHeightDip() const noexcept { return titlebar_.GetHeight(); }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const { return titlebar_.HitTest(dip_x, dip_y); }
    bool IsOverMdScrollbar(float dip_x, float dip_y) const;
    bool IsOverMdScrollbar(float dip_x, float dip_y, const ::PaneLayout& layout) const;
    void OnActivate(bool active);

private:
    // AppControllerが返すアクションを実行
    void ExecuteActions(const ActionList& actions);

    // DIP変換
    struct DipPoint { float x, y; };
    DipPoint PixelToDip(int px, int py) const;

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
    void SelectAll();
    void ClearSelection();

    // ペインクリックハンドラ (OnLButtonDownから抽出)
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

    // レイアウト / スクロール
    void UpdateLayoutAndScroll(float desired_scroll);
    void UpdateScrollBar();
    void InvalidateMdPane(const PaneRect& md_rect);
    void ScrollTo(float position);
    void SmoothScrollBy(float delta);
    void UpdateSmoothScroll();
    void StopSmoothScroll();
    void SyncMaxScroll(float md_pane_height);
    int FindFirstVisibleNode() const;
    void AnchorCompensateScroll(int anchor_idx, float anchor_y_before, float md_pane_height);
    void OnResizeEnd();
    void RefreshPaneLayout();
    void RefreshFilePane();
    void OnDeferredLayout();

    // ファイル読み込み (file_load_service_に委譲)
    void ReloadCurrentFile();
    void DoReloadCurrentFile();
    void DoLoadMarkdownFile();
    void UpdateTitleBar();
    void SaveLastFilePath();
    void SavePaneState();
    void LoadPaneState();

    // ペインレイアウト（結果はキャッシュされる）
    const ::PaneLayout& GetPaneLayout() const;
    void InvalidatePaneLayoutCache() noexcept { pane_layout_valid_ = false; }
    ::PaneZone PaneAtPoint(float dip_x, float dip_y) const;
    float GetMarkdownPaneWidth() const;

    void RequestMermaidRenders();
    void OnMermaidRenderComplete();
    void LoadImages();
    void OnImageLoadComplete();
    int ApplyCachedImages();

    // OnPaint用のレンダーステート構築ヘルパー
    GestureRenderState BuildGestureRenderState() const;
    SidePaneState BuildSidePaneState(const ::PaneLayout& layout) const;
    TitleBarRenderState BuildTitleBarRenderState(float window_width) const;
    ToastRenderState BuildToastRenderState() const;

    // ダークモード / ズーム (theme_service_に委譲)
    void ToggleDarkMode();
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    void ApplyZoom(float new_zoom);

public:
    // タイマーID (メッセージルーティング用にWin32Windowと共有)
    static constexpr UINT_PTR TIMER_FILE_WATCH = 2;
    static constexpr UINT_PTR TIMER_DEFERRED_LAYOUT = 3;
    static constexpr UINT_PTR TIMER_LOADING_ANIM = 4;
    static constexpr UINT_PTR TIMER_SWIPE_OVERLAY = 5;
    static constexpr UINT_PTR TIMER_TOAST = 6;
    static constexpr UINT WM_APP_LOAD_FILE = WM_APP + 1;
    static constexpr UINT WM_APP_IMAGE_LOADED = WM_APP + 2;
    static constexpr UINT WM_APP_RELOAD_FILE = WM_APP + 3;

private:
    // Win32ハンドル
    HWND hwnd_ = nullptr;
    float cached_dpi_scale_ = 1.0f;

    // キャッシュ済みシステムカーソル
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_ibeam_ = nullptr;
    HCURSOR cursor_sizewe_ = nullptr;

    // ヒットテストのスロットリング
    POINT last_md_hit_pos_ = { LONG_MIN, LONG_MIN };
    bool last_md_cursor_hand_ = false;
    POINT last_copy_hit_pos_ = { LONG_MIN, LONG_MIN };

    // コアサービス
    Renderer renderer_;
    MermaidFileCache file_cache_;         // mermaid_renderer_より先に宣言（破棄順序の保証）
    MermaidRenderer mermaid_renderer_;
    ImageLoader image_loader_;
    FileLoader file_loader_;
    DocumentService doc_service_{ file_loader_ };
    AppController controller_;
    ConfigService config_;
    ThemeService theme_service_{ config_ };
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

    float last_mermaid_content_width_ = 0.0f;

    // 画像パス解決キャッシュ（canonical() のディスクI/Oを回避）
    std::unordered_map<size_t, std::wstring> resolved_image_paths_;

    // PaneLayout キャッシュ（ウィンドウサイズ・ペイン状態が変わるまで再利用）
    mutable ::PaneLayout cached_pane_layout_{};
    mutable float cached_window_width_for_layout_ = 0.0f;
    mutable bool pane_layout_valid_ = false;

    // ナビゲーションオーバーレイ
    using NavButtonHover = HitTestService::NavButtonHover;
    NavButtonHover nav_hover_ = NavButtonHover::None;

    // 戻る/進むナビゲーション時の遅延スクロール復元用
    float pending_nav_scroll_y_ = -1.0f;

    // カスタムコンテキストメニュー
    ContextMenu ctx_menu_;

    // コードブロック コピーボタン
    int hovered_copy_node_ = -1;

    // 目次ペインの現在アクティブな見出しインデックス
    int active_toc_index_ = -1;

    // トースト通知
    ToastNotifier toast_;
    void ShowToast(std::wstring_view message);

    // スムーススクロールのフレーム間タイミング
    std::chrono::steady_clock::time_point last_scroll_time_{};
};
