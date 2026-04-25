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
#include "layout_service.h"
#include "app_controller.h"
#include "config_service.h"
#include "theme_service.h"
#include "file_load_service.h"
#include "resource_manager.h"
#include "session_service.h"
#include "cursor_manager.h"
#include "hit_test_service.h"
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
    bool IsSearchBarVisible() const noexcept { return state_.search.search_state.IsVisible(); }
    void OnToggleCaseSensitive() { Dispatch(ToggleCaseSensitiveAction{}); }
    void OnToggleHighlight() { Dispatch(ToggleHighlightAction{}); }
    void SetSearchSelection(int sel_start, int sel_end) { Dispatch(SearchSelectionAction{ sel_start, sel_end }); }
    void SetImeComposition(std::wstring_view comp) { Dispatch(ImeCompositionAction{ std::pmr::wstring{comp} }); }
    RECT GetSearchEditRect();

    void SetPendingRestoreNode(int node, int offset) noexcept
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
    // 外部から強制再描画を要求するためのフック（D2D デバイスロスト後など）。
    void InvalidatePaintCache() noexcept { last_paint_fp_ = {}; }
    constexpr float GetDpiScale() const noexcept { return state_.window.cached_dpi_scale; }

    // カスタムタイトルバー
    float GetTitleBarHeightDip() const noexcept { return state_.window.titlebar.GetHeight(); }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept { return state_.window.titlebar.HitTest(dip_x, dip_y); }
    bool IsOverMdScrollbar(float dip_x, float dip_y);
    bool IsOverMdScrollbar(float dip_x, float dip_y, const ::PaneLayout& layout) const noexcept;
    void OnActivate(bool active) { Dispatch(ActivateAction{ active }); }

private:
    // AppControllerが返すアクションを実行
    void Dispatch(const AppAction& action);

    // reducer を介さない経路から effect を単発発火するヘルパー。
    // App 内で発生する連鎖処理の最後で SyncTocActive 等を emit する際に使う。
    template <typename T>
    void EmitEffect(T&& e)
    {
        SideEffectList effects;
        PushEffect(effects, std::forward<T>(e));
        effect_executor_.Execute(effects);
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
    void BeginAsyncLoad(const std::pmr::wstring& path);
    void FinishLoadMarkdownFile(bool heights_estimated = false);
    void HandleLoadFailureFallback();
    float CalcScrollForDiff(size_t diff_pos, float viewport_height) const;
    void ApplyMermaidCacheHeights(float md_width);
    void UpdateTitleBar();

    void FinishReload(bool is_prefix_only, size_t diff_pos);

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

    // OnPaint で前フレームと一致したらコマンド生成と Render を skip するためのキー。
    struct PaintFingerprint {
        bool valid = false;
        bool show_loading = false;
        float scroll_y = 0.0f;
        float md_pane_w = 0.0f;
        float md_pane_h = 0.0f;
        uint32_t effects_gen = 0;
        uint32_t search_gen = 0;
        int search_current = -1;
        int hovered_copy = -1;
        int hovered_save = -1;
        int nav_hover = 0;
        int file_hover = -1;
        int toc_hover = -1;
        bool selection_active = false;
        uint32_t selection_id = 0;
        bool toast_visible = false;
        float toast_alpha = 0.0f;
        bool gesture_overlay_visible = false;
        bool gesture_trail_active = false;
        size_t gesture_trail_size = 0;
        bool can_back = false;
        bool can_fwd = false;
        bool window_active = false;
        bool zoomed = false;
        bool sb_visible = false;
        size_t sb_query_size = 0;
        size_t sb_ime_size = 0;
        int sb_caret = -1;
        bool sb_case = false;
        bool sb_highlight = false;
        bool dark = false;
        size_t doc_node_count = 0;
        bool friend operator==(const PaintFingerprint& a, const PaintFingerprint& b) noexcept = default;
    };
    PaintFingerprint last_paint_fp_{};
    PaintFingerprint ComputePaintFingerprint(bool show_loading, float md_w, float md_h) const noexcept;

    void ShowToast(std::wstring_view message);
};
