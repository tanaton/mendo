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
    explicit App(ConfigService& config) noexcept : config_(config)
    {}
    bool Init(HWND hwnd);

    void LoadMarkdownFile(std::wstring_view path);
    void LoadHelpDocument();
    std::pmr::wstring LoadLastFilePath() const;
    void ShowDirectory(std::wstring_view dir_path);

    // 起動時にウィンドウ生成と並列で I/O + パースを開始する。Init 末尾の
    // OnInitComplete で hwnd が解禁されると、worker は ::PostMessageW(PARSE_COMPLETE)
    // を発行し、通常の async load 経路に合流する。
    void StartPreloadAsync(std::pmr::wstring path);

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
    void HandleMdPaneHover(float dip_x, float dip_y, int px, int py, const ::PaneLayout& layout);

    void OnXButtonBack()
    {
        Dispatch(NavigateBackAction{});
    }
    void OnXButtonForward()
    {
        Dispatch(NavigateForwardAction{});
    }

    constexpr HANDLE GetFileWatchEvent() const noexcept
    {
        return doc_service_.GetFileWatchEvent();
    }
    void OnFileWatchEvent();

    void HandleTimer(UINT_PTR timer_id);
    void OnAppLoadFile();
    void OnAppReloadFile();
    void OnAppImageLoaded();
    void OnParseComplete();
    void OnCaptureChanged();
    void OnDestroy();

    void OnSearchTextChanged(std::pmr::wstring text)
    {
        Dispatch(SearchTextChangedAction{ std::move(text) });
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
    void OnToggleCaseSensitive()
    {
        Dispatch(ToggleCaseSensitiveAction{});
    }
    void OnToggleHighlight()
    {
        Dispatch(ToggleHighlightAction{});
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

    // 起動時の preload が App::Init より先に完了した場合、Init 内の AttachOrApplyPreload が
    // 同期で OnParseComplete → FinishLoadMarkdownFile を呼ぶ。復元情報は Init 呼び出し前に
    // セットしておく必要がある。
    constexpr void SetPendingRestoreNode(int node, int offset) noexcept
    {
        state_.view.scroll_restore.SetNodeRestore(node, offset);
    }

    void OnEnterSizeMove()
    {
        Dispatch(EnterSizeMoveAction{});
    }
    void OnExitSizeMove()
    {
        Dispatch(ExitSizeMoveAction{});
    }

    bool IsRenderReady() const noexcept
    {
        return renderer_.GetRenderTarget() != nullptr;
    }
    void Invalidate() noexcept
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void InvalidatePane(const PaneRect& rect) noexcept;
    void InvalidateTitleBar() noexcept;
    constexpr float GetDpiScale() const noexcept
    {
        return state_.window.cached_dpi_scale;
    }

    constexpr float GetTitleBarHeightDip() const noexcept
    {
        return state_.window.titlebar.GetHeight();
    }
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept
    {
        return state_.window.titlebar.HitTest(dip_x, dip_y);
    }
    bool IsOverMdScrollbar(float dip_x, float dip_y);
    bool IsOverMdScrollbar(float dip_x, float dip_y, const ::PaneLayout& layout) const noexcept;
    void OnActivate(bool active)
    {
        Dispatch(ActivateAction{ active });
    }

private:
    void Dispatch(const AppAction& action);

    // reducer を介さない経路から effect を単発発火するヘルパー。
    // SideEffectList を作らずに executor の単発 API へ直送し、毎呼び出しの
    // pmr vector アロケーションを避ける (ホット経路: OnDeferredLayout, OnPaint, OnSizing 等)。
    template <typename T>
    void EmitEffect(T&& e)
    {
        effect_executor_.ExecuteOne(side_effect_detail::WrapIntoDomain(std::forward<T>(e)));
    }

    ResourceManager::Callbacks BuildResourceManagerCallbacks();
    SearchBarController::Callbacks BuildSearchBarCallbacks();

    void EnsureScrollTarget();

    struct DipPoint {
        float x, y;
    };
    DipPoint PixelToDip(int px, int py) const noexcept;

    using HitResult = HitTestService::HitResult;
    HitResult HitTest(int screen_x, int screen_y);
    std::optional<std::pmr::string> GetLinkAtHit(const HitResult& hit) const;
    MdPaneHitContext BuildMdPaneHitContext(int px, int py, const PaneLayout& pane_layout) const noexcept;

    void HandleLinkClick(std::string_view url);

    void SetClipboardText(std::wstring_view text) const;
    void CopySelectionToClipboard() const;
    void CopyCodeBlockToClipboard(int node_index) const;
    void SaveDiagramAsPng(int node_index);
    void CopyDiagramAsSvg(int node_index);

    bool HandleTitleBarClick(float dip_x, float dip_y);
    bool HandleSearchBarClick(float dip_x, float dip_y, const PaneLayout& layout, bool is_double_click);
    void HandleMdPaneClick(float dip_x, float dip_y, int px, int py, const PaneLayout& layout);
    void HandleFilePaneClick(float dip_x, float dip_y, const PaneLayout& layout);
    void HandleTocPaneClick(float dip_x, float dip_y, const PaneLayout& layout);
    static bool IsOverPaneScrollbar(float dip_x, const PaneRect& rect,
                                    float total_content, const PaneScrollInfo& scroll_info) noexcept;

    PaneScrollInfo ComputePaneScrollInfo(const PaneRect& rect, float total_content) const;

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

    void ReloadCurrentFile();
    void DoReloadCurrentFile();
    void DoLoadMarkdownFile();
    void BeginAsyncLoad(std::pmr::wstring path, bool suppress_animation = false);
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
    void InvalidatePaneLayoutCache() noexcept
    {
        state_.pane_layout_valid = false;
    }
    ::PaneZone PaneAtPoint(float dip_x);
    float GetMarkdownPaneWidth();
    void SyncPaneThemeCache();
    void HandleApplyThemeChange(const effect::ApplyThemeChange& e);
    void FinishThemeOrZoomChange();

private:
    HWND hwnd_ = nullptr;

    CursorManager cursors_;

    Renderer renderer_;
    TaskScheduler scheduler_;
    TaskScheduler layout_scheduler_;
    MermaidFileCache file_cache_; // mermaid_renderer_ より先に宣言する（mermaid_renderer_ は破棄時に file_cache_ を参照する）
    MermaidRenderer mermaid_renderer_;
    ImageLoader image_loader_;
    FileWatcher file_watcher_;
    DocumentService doc_service_{ file_watcher_ };
    AppController controller_;
    ConfigService& config_;
    ThemeService theme_service_{ config_ };
    SessionService session_{ config_ };
    FileLoadService file_load_service_{ doc_service_ };

    AppState state_;

    HitTestService hit_test_;
    std::optional<LayoutService> layout_service_;
    ResourceManager resource_manager_;
    Win32Host win32_host_;
    SideEffectExecutor effect_executor_;

    void ShowToast(std::wstring_view message);

    // SVG クリップボードコピーのセッション内キャッシュ (LruCache の挙動は src/util/lru_cache.h 参照)。
    // キーは PNG キャッシュと同じ NodeDiagramHash。LatexMath は SVG コピー対象外。
    static constexpr size_t MAX_SVG_CACHE_ENTRIES = 128;
    LruCache<uint64_t, std::pmr::wstring, MAX_SVG_CACHE_ENTRIES> svg_cache_;
    // SVG レンダリングは非同期で 1 秒程度かかる。同じノードを連打されても
    // 1 件のリクエストに集約するためのフラグ。
    bool svg_copy_in_flight_ = false;
};
