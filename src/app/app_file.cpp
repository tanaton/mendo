#include "app.h"
#include "app_constants.h"
#include "file_loader.h"
#include "file_io.h"
#include "i18n.h"
#include "document_utils.h"
#include "mermaid_util.h"
#include "layout.h"
#include "profiler.h"
#include "string_convert.h"
#include "utility.h"
#include <algorithm>
#include <utility>

void App::LoadHelpDocument()
{
    if (IsHelpPath(state_.document.doc.GetFilePath())) {
        return;
    }

    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    EmitEffect(effect::KillTimer{ app_timer::LOADING_ANIM });
    file_load_service_.StopLoading();
    EmitEffect(effect::StopFileWatch{});
    ResetViewForNewDocument();

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    state_.document.doc = Document::FromMarkdown(std::move(utf8), HELP_PATH);
    state_.document.layout_cache.Reset(state_.document.doc.GetNodes().size());

    state_.file_explorer.SetCurrentFile(L"");

    // 旧ドキュメントで合成された scroll_target は直後の ViewportLayout →
    // ApplyScrollTarget で旧ノード位置から scroll_y を再評価してしまうため、
    // SetScrollY(0) より前に破棄する必要がある。
    state_.view.viewport.ClearScrollTarget();
    state_.view.viewport.SetScrollY(0.0f);
    {
        const auto pane_layout = GetPaneLayout();
        EmitEffect(effect::ViewportLayout{ pane_layout.md_rect.width, pane_layout.md_rect.height });
        EmitEffect(effect::SyncMaxScroll{ pane_layout.md_rect.height });
    }
    Invalidate();
    ScheduleDeferredLayoutIfNeeded();

    UpdateTitleBar();
}

void App::BeginAsyncLoad(std::pmr::wstring path, bool suppress_animation)
{
    // ライブリロード時はアニメーションを表示しない。
    // 大きいファイルを編集中の差分リロードでスピナーが点滅すると視認性が下がるため、
    // 旧コンテンツを表示したまま静かにバックグラウンドでパースし差し替える。
    const bool show_anim = !suppress_animation && DocumentService::NeedsLoadingAnimation(path) && !state_.pending_reload_retry;
    if (show_anim) {
        MENDO_PROFILE("App::BeginAsyncLoad with animation");
        file_load_service_.StartLoading(std::move(path));
        EmitEffect(effect::SetTimer{ app_timer::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS });
        Invalidate();
        UpdateWindow(hwnd_);
    }
    else {
        MENDO_PROFILE("App::BeginAsyncLoad without animation");
        file_load_service_.SetLoadingPath(std::move(path));
    }
    file_load_service_.StartAsyncLoad(scheduler_, hwnd_, app_msg::PARSE_COMPLETE, renderer_.GetTheme());
}

// DeferPrefixShrink はエディタの truncate→rewrite 2段階保存の前半。
// state_.document.doc を更新せず保持して、次のリロードで正確な差分を取り直す。
bool App::ApplyReloadDecisionEarly(const ReloadDecision& decision)
{
    if (decision.op == ReloadOp::NoChange) {
        state_.pending_reload_retry = false;
        EmitEffect(effect::ResumeFileWatch{});
        Invalidate();
        return true;
    }
    if (decision.op == ReloadOp::DeferPrefixShrink) {
        DeferReloadRetry();
        return true;
    }
    state_.pending_reload_retry = false;
    return false;
}

void App::DeferReloadRetry()
{
    // FileWatcher は paused のまま維持する。resume すると待機中の変更通知が
    // FILE_RELOAD_DEBOUNCE を 200ms に上書きし、短縮リトライが効かなくなる。
    state_.pending_reload_retry = true;
    EmitEffect(effect::SetTimer{ app_timer::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_RETRY_MS });
}

bool App::DeferIfPartialWrite(const std::pmr::wstring& path, size_t read_size)
{
    if (!IsFileLargerThan(path.c_str(), read_size)) {
        return false;
    }
    DeferReloadRetry();
    return true;
}

// reducer を経由せず App 層で直接実行する。ファイル I/O + 同期/非同期ロード分岐 +
// パース + レイアウト初期化を含む大きな命令的ワークフローで、reducer 化すると
// effect variant と executor が肥大化するため意図的に service 経路として分離している。
void App::LoadMarkdownFile(std::wstring_view path)
{
    MENDO_PROFILE("App::LoadMarkdownFile");
    EmitEffect(effect::KillTimer{ app_timer::FILE_RELOAD_DEBOUNCE });
    state_.pending_reload_retry = false;
    // 仮想パスは NeedsAsyncLoad が true を返し非同期ロードが失敗するため、
    // 先に検出して同期ロードに回す。
    if (IsHelpPath(path)) {
        LoadHelpDocument();
        return;
    }
    std::pmr::wstring path_str{ path };
    if (!DocumentService::NeedsAsyncLoad(path_str)) {
        file_load_service_.SetLoadingPath(std::move(path_str));
        DoLoadMarkdownFile();
    }
    else {
        BeginAsyncLoad(std::move(path_str));
    }
}

void App::StartPreloadAsync(std::pmr::wstring path)
{
    file_load_service_.StartPreloadAsync(std::move(path));
}

void App::DoLoadMarkdownFile()
{
    MENDO_PROFILE("DoLoadMarkdownFile");

    if (IsHelpPath(file_load_service_.GetLoadingPath())) {
        LoadHelpDocument();
        return;
    }

    EmitEffect(effect::KillTimer{ app_timer::LOADING_ANIM });

    {
        MENDO_PROFILE("ExecuteLoad(FileIO+Parse)");
        auto load_result = file_load_service_.ExecuteLoad(state_.document.doc, state_.document.layout_cache);
        if (!load_result) {
            ShowToast(FileLoadErrorMessage(load_result.error(), i18n::S()));
            HandleLoadFailureFallback();
            return;
        }
    }

    FinishLoadMarkdownFile();
}

void App::HandleLoadFailureFallback()
{
    if (state_.document.doc.IsEmpty()) {
        LoadHelpDocument();
    }
    else {
        Invalidate();
    }
}

void App::OnParseComplete()
{
    EmitEffect(effect::KillTimer{ app_timer::LOADING_ANIM });
    file_load_service_.StopLoading();

    auto result = file_load_service_.TakeAsyncResult();
    if (!result) {
        MENDO_TRACE("OnParseComplete: no result (cancelled or load failed)");
        // 失敗パスでも paused 状態の FileWatcher を必ず再開させる。
        EmitEffect(effect::ResumeFileWatch{});
        if (auto err = file_load_service_.TakeAsyncError()) {
            ShowToast(FileLoadErrorMessage(*err, i18n::S()));
        }
        HandleLoadFailureFallback();
        return;
    }

    // 差分ベースのスキップ／スクロール復元は同一パスのリロード時のみ有効。
    // 非同期のファイルオープンでも OnParseComplete() が使われるため、
    // 別ファイル読み込み時は差分ロジックをスキップする。
    if (path_util::iequal(result->doc.GetFilePath(), state_.document.doc.GetFilePath())) {
        if (DeferIfPartialWrite(result->doc.GetFilePath(), result->doc.GetLoadedByteSize())) {
            return;
        }

        const std::string_view old_view(state_.document.doc.GetRawText());
        const std::string_view new_view(result->doc.GetRawText());
        const auto decision = AnalyzeReloadDiff(old_view, new_view);

        MENDO_TRACEF("OnParseComplete: reload node_count=%zu diff_pos=%zu old_size=%zu new_size=%zu op=%d",
                     result->doc.GetNodes().size(), decision.diff_pos, old_view.size(), new_view.size(),
                     std::to_underlying(decision.op));

        if (ApplyReloadDecisionEarly(decision)) {
            return;
        }
        if (decision.op == ReloadOp::PrefixGrowth) {
            resource_manager_.CancelMermaidBatch();
            state_.document.doc = std::move(result->doc);
            FinishReload(decision.diff_pos);
            return;
        }
        state_.reload_diff_pos = decision.diff_pos;
    }
    else {
        state_.reload_diff_pos = std::wstring_view::npos;
    }

    const bool heights_estimated = result->heights_estimated;
    state_.document.doc = std::move(result->doc);
    state_.document.layout_cache = std::move(result->cache);

    FinishLoadMarkdownFile(heights_estimated);
}

void App::FinishLoadMarkdownFile(bool heights_estimated)
{
    ResetViewForNewDocument();
    state_.search.search_bar_ctrl.Reset();
    EmitEffect(effect::PostWindowMessage{ app_msg::SEARCH_UNFOCUS, app_param::SEARCH_UNFOCUS_FILE_SWITCH, 0 });
    state_.active_toc_index = -1;

    const std::pmr::wstring dir = state_.document.doc.GetDirectory();
    if (!dir.empty()) {
        state_.file_explorer.SetDirectory(dir);
        state_.file_explorer.SetCurrentFile(state_.document.doc.GetFilePath());
    }

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    float scroll_y = 0.0f;

    const bool has_reload_diff = (state_.reload_diff_pos != std::wstring_view::npos);

    if (state_.view.scroll_restore.HasNodeRestore()) {
        MENDO_TRACEF("FinishLoad: has_reload_diff=%d node=%d offset=%d heights_estimated=%d",
                     has_reload_diff ? 1 : 0,
                     state_.view.scroll_restore.pending_restore_node,
                     state_.view.scroll_restore.pending_restore_offset,
                     heights_estimated ? 1 : 0);
    }
    else {
        MENDO_TRACEF("FinishLoad: has_reload_diff=%d (no node restore) heights_estimated=%d",
                     has_reload_diff ? 1 : 0,
                     heights_estimated ? 1 : 0);
    }

    // cache.Reset() 直後は全ノードの高さが 0 のため、スクロール復元前に
    // ノード高さを推定し、Mermaid/画像キャッシュの実測値で補正する。
    if (has_reload_diff || state_.view.scroll_restore.HasNodeRestore()) {
        if (!heights_estimated) {
            EstimateNodeHeights(state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme());
        }
        ApplyCachedHeightsAndRecompute(md_width);
    }

    // CalcScrollForDiff は md_height (layout 値) に依存するため reducer に渡せず、
    // App 側で先に解決してから Dispatch する。
    float reload_diff_scroll_y = 0.0f;
    if (has_reload_diff) {
        reload_diff_scroll_y = CalcScrollForDiff(state_.reload_diff_pos, md_height);
    }
    Dispatch(RestoreScrollAfterLoadAction{ has_reload_diff, reload_diff_scroll_y });
    scroll_y = state_.view.viewport.GetScrollY();

    MENDO_TRACEF("FinishLoad: after-restore scroll_y=%.1f reload_diff_scroll_y=%.1f",
                 scroll_y, reload_diff_scroll_y);

    {
        MENDO_PROFILE("ViewportLayout(Initial)");
        EmitEffect(effect::ViewportLayout{ md_width, md_height });
    }

    FinalizeLayout(md_height);

    MENDO_TRACEF("FinishLoad: after-finalize scroll_y=%.1f max_scroll=%.1f",
                 state_.view.viewport.GetScrollY(),
                 state_.view.viewport.GetMaxScroll());

    UpdateTitleBar();

    EmitEffect(effect::SyncTocActive{});

    EmitEffect(effect::StartFileWatch{ state_.document.doc.GetFilePath() });
}

void App::ReloadCurrentFile()
{
    const auto& path = state_.document.doc.GetFilePath();
    if (path.empty() || IsHelpPath(path)) {
        return;
    }
    // FileWatcher のバーストで重複スケジュールされないよう、進行中のロードが
    // あれば即 return する。suppress_animation 経路では IsLoading() が立たない
    // ため IsAsyncLoading() も併せて見る。
    if (file_load_service_.IsLoading() || file_load_service_.IsAsyncLoading()) {
        return;
    }

    if (DocumentService::NeedsAsyncLoad(path)) {
        MENDO_TRACE("ReloadCurrentFile: async path");
        BeginAsyncLoad(path, /* suppress_animation = */ true);
    }
    else {
        MENDO_TRACE("ReloadCurrentFile: sync path (DoReloadCurrentFile)");
        DoReloadCurrentFile();
    }
}

void App::DoReloadCurrentFile()
{
    MENDO_PROFILE("DoReloadCurrentFile");

    EmitEffect(effect::KillTimer{ app_timer::LOADING_ANIM });
    file_load_service_.StopLoading();
    state_.active_toc_index = -1;

    if (state_.document.doc.GetFilePath().empty()) {
        return;
    }

    CancelPendingResources();

    auto load_result = FileLoader::LoadFile(state_.document.doc.GetFilePath());
    if (!load_result) {
        EmitEffect(effect::ResumeFileWatch{});
        return;
    }

    if (DeferIfPartialWrite(state_.document.doc.GetFilePath(), load_result->byte_size)) {
        return;
    }

    const size_t byte_size = load_result->byte_size;
    std::pmr::string new_text = std::move(load_result->text);

    const std::string_view old_view(state_.document.doc.GetRawText());
    const std::string_view new_view(new_text);
    const auto decision = AnalyzeReloadDiff(old_view, new_view);

    MENDO_TRACEF("DoReload: diff_pos=%zu old_size=%zu new_size=%zu op=%d",
                 decision.diff_pos, old_view.size(), new_view.size(),
                 std::to_underlying(decision.op));

    if (ApplyReloadDecisionEarly(decision)) {
        return;
    }
    state_.document.doc.ReplaceFromMarkdown(std::move(new_text), byte_size);
    FinishReload(decision.diff_pos);
}

// DoReloadCurrentFile / OnParseComplete 共通のリロード後処理。
// ドキュメントは更新済みの状態で呼ばれる。
void App::FinishReload(size_t diff_pos)
{
    state_.document.layout_cache.Reset(state_.document.doc.GetNodes().size(), false);
    EstimateNodeHeights(state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme());

    renderer_.InvalidateTocPaneCache();

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    // Mermaid/LaTeX 図と通常画像の推定高さを実測値 (file_cache_ / image_loader メモリキャッシュ) で
    // 上書きし、CalcScrollForDiff の Y 計算がずれないようにする。
    ApplyCachedHeightsAndRecompute(md_width);

    const float desired_scroll = CalcScrollForDiff(diff_pos, md_height);

    MENDO_TRACEF("FinishReload: desired_scroll=%.1f diff_pos=%zu",
                 desired_scroll, diff_pos);

    // スクロール位置を先に設定してから ViewportLayout を呼ぶことで、変更箇所周辺の
    // 可視ノードが優先的に計測される。リロード時は直前の navigation target を破棄し、
    // ピクセル位置で固定する。
    state_.view.viewport.ClearScrollTarget();
    state_.view.viewport.SetScrollY(desired_scroll);

    {
        MENDO_PROFILE("Reload::ViewportLayout");
        EmitEffect(effect::ViewportLayout{ md_width, md_height });
    }

    FinalizeLayout(md_height);

    MENDO_TRACEF("FinishReload: after-finalize scroll_y=%.1f max_scroll=%.1f",
                 state_.view.viewport.GetScrollY(),
                 state_.view.viewport.GetMaxScroll());

    if (state_.search.search_state.IsVisible()) {
        // PMR プール再利用で (nodes.data(), size) が一致すると古いキャッシュを
        // 誤用するため、クエリ有無に関わらず明示破棄する。
        state_.search.search_state.InvalidateLowercaseCache();
        if (!state_.search.search_state.GetQuery().empty()) {
            state_.search.search_bar_ctrl.RunSearchAndLocate(state_.document.doc.GetNodes());
        }
    }

    EmitEffect(effect::SyncTocActive{});

    EmitEffect(effect::ResumeFileWatch{});
}

float App::CalcScrollForDiff(size_t diff_pos, float viewport_height) const
{
    MENDO_TRACEF("CalcScrollForDiff: diff_pos=%zu node_count=%zu", diff_pos, state_.document.doc.GetNodes().size());
    return CalcScrollYForDiff(
        state_.document.doc.GetNodes(), state_.document.layout_cache,
        std::string_view{ state_.document.doc.GetRawText() },
        diff_pos, viewport_height, state_.view.viewport.GetScrollY());
}

bool App::ApplyMermaidCacheHeights(float md_width)
{
    const float content_width = renderer_.GetTheme().ContentWidth(md_width);
    const bool dark_mode = theme_service_.IsDarkMode();
    const auto& nodes = state_.document.doc.GetNodes();
    bool any_applied = false;
    for (size_t i : state_.document.doc.GetDiagramNodeIndices()) {
        const auto hash = mermaid_util::NodeDiagramHash(nodes[i], content_width, dark_mode);
        MermaidFileCache::CacheEntry fentry;
        if (file_cache_.LookupDimensions(hash, fentry)) {
            state_.document.layout_cache[i].height = fentry.css_height;
            any_applied = true;
        }
    }
    return any_applied;
}

void App::ApplyCachedHeightsAndRecompute(float md_width)
{
    const bool mermaid_applied = ApplyMermaidCacheHeights(md_width);
    const bool image_applied = resource_manager_.ApplyCachedImagesForReload() > 0;
    if (mermaid_applied || image_applied) {
        RecomputeYPositions(
            state_.document.doc.GetNodesMut(),
            state_.document.layout_cache,
            renderer_.GetTheme());
    }
}

void App::UpdateTitleBar()
{
    const int zoom_percent = static_cast<int>(ZOOM_STEPS[state_.view.viewport.GetZoomIndex()] * 100.0f + 0.5f);
    auto title = BuildTitleString(state_.document.doc.GetFilePath(), zoom_percent);
    SetWindowTextW(hwnd_, title.c_str());
    state_.cached_title_text = std::move(title);
    InvalidateTitleBar();
}
