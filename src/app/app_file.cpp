#include "app.h"
#include "app_constants.h"
#include "file_loader.h"
#include "i18n.h"
#include "document_utils.h"
#include "mermaid_util.h"
#include "layout.h"
#include "profiler.h"
#include "utility.h"
#include <algorithm>

// ============================================================
// ファイル読み込み
// ============================================================

void App::LoadHelpDocument()
{
    if (IsHelpPath(state_.document.doc.GetFilePath())) {
        return;
    }

    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    KillTimer(hwnd_, app_timer::LOADING_ANIM);
    file_load_service_.StopLoading();
    doc_service_.StopWatching();
    ResetViewForNewDocument();

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    state_.document.doc = Document::FromMarkdown(std::move(utf8), HELP_PATH);
    state_.document.layout_cache.Reset(state_.document.doc.GetNodes().size());

    state_.file_explorer.SetCurrentFile(L"");

    // ビューポート優先レイアウト + 遅延処理
    state_.view.viewport.SetScrollY(0.0f);
    {
        const auto pane_layout = GetPaneLayout();
        layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache,
            pane_layout.md_rect.width, pane_layout.md_rect.height);
        SyncMaxScroll(pane_layout.md_rect.height);
    }
    UpdateScrollBar();
    Invalidate();
    ScheduleDeferredLayoutIfNeeded();

    UpdateTitleBar();
}

void App::BeginAsyncLoad(const std::pmr::wstring& path)
{
    file_load_service_.SetLoadingPath(path);
    if (DocumentService::NeedsLoadingAnimation(path) && !state_.pending_prefix_shrink) {
        file_load_service_.StartLoading(path);
        SetTimer(hwnd_, app_timer::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS, nullptr);
        Invalidate();
        UpdateWindow(hwnd_);
    }
    file_load_service_.StartAsyncLoad(scheduler_, hwnd_, app_msg::PARSE_COMPLETE, renderer_.GetTheme());
}

void App::LoadMarkdownFile(std::wstring_view path)
{
    KillTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE);
    state_.pending_prefix_shrink = false;
    // 仮想パスは NeedsAsyncLoad が true を返し非同期ロードが失敗するため、
    // 先に検出して同期ロードに回す。
    if (IsHelpPath(path)) {
        LoadHelpDocument();
        return;
    }
    const std::pmr::wstring path_str{ path };
    if (!DocumentService::NeedsAsyncLoad(path_str)) {
        file_load_service_.SetLoadingPath(path_str);
        DoLoadMarkdownFile();
    }
    else {
        BeginAsyncLoad(path_str);
    }
}

void App::DoLoadMarkdownFile()
{
    MENDO_PROFILE("DoLoadMarkdownFile");

    // ヘルプ仮想パスの場合は専用ルートへ
    if (IsHelpPath(file_load_service_.GetLoadingPath())) {
        LoadHelpDocument();
        return;
    }

    KillTimer(hwnd_, app_timer::LOADING_ANIM);

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
    KillTimer(hwnd_, app_timer::LOADING_ANIM);
    file_load_service_.StopLoading();

    auto result = file_load_service_.TakeAsyncResult();
    if (!result) {
        MENDO_TRACE("OnParseComplete: no result (cancelled or load failed)");
        // LoadFile失敗時、FileWatcherがpausedのまま残るのを防ぐ
        doc_service_.ResumeWatching();
        HandleLoadFailureFallback();
        return;
    }

    // 差分ベースのスキップ／スクロール復元は同一パスのリロード時のみ有効。
    // 非同期のファイルオープンでも OnParseComplete() が使われるため、
    // 別ファイル読み込み時は差分ロジックをスキップする。
    if (_wcsicmp(result->doc.GetFilePath().c_str(), state_.document.doc.GetFilePath().c_str()) == 0) {
        const std::string_view old_view(state_.document.doc.GetRawUtf8());
        const std::string_view new_view(result->doc.GetRawUtf8());
        const auto decision = AnalyzeReloadDiff(old_view, new_view);
        state_.pending_prefix_shrink = (decision.op == ReloadOp::DeferPrefixShrink);

        MENDO_TRACEF("OnParseComplete: reload node_count=%zu diff_pos=%zu old_size=%zu new_size=%zu op=%d",
            result->doc.GetNodes().size(), decision.diff_pos, old_view.size(), new_view.size(),
            static_cast<int>(decision.op));

        switch (decision.op) {
        case ReloadOp::NoChange:
            doc_service_.ResumeWatching();
            Invalidate();
            return;
        case ReloadOp::DeferPrefixShrink:
            // エディタの truncate→rewrite 2段階保存の前半を検出。state_.document.doc を
            // 更新せず元のコンテンツを保持し、次のリロードで正確な差分を検出できるようにする。
            doc_service_.ResumeWatching();
            return;
        case ReloadOp::PrefixGrowth:
            // 末尾伸張のみ。FinishReload が ResizePreservingPrefix で旧キャッシュの実測高さを保持する。
            resource_manager_.CancelMermaidBatch();
            state_.document.doc = std::move(result->doc);
            FinishReload(true, decision.diff_pos);
            return;
        case ReloadOp::FullReload:
            state_.reload_diff_pos = decision.diff_pos;
            break;
        }
    }
    else {
        // 別ファイルの非同期ロードではリロード用の差分スクロールを使わない
        state_.reload_diff_pos = std::string_view::npos;
    }

    state_.document.doc = std::move(result->doc);
    state_.document.layout_cache = std::move(result->cache);

    FinishLoadMarkdownFile(/* heights_estimated = */ true);
}

void App::FinishLoadMarkdownFile(bool heights_estimated)
{
    ResetViewForNewDocument();
    state_.search.search_bar_ctrl.Reset();
    PostMessage(hwnd_, app_msg::SEARCH_UNFOCUS, app_param::SEARCH_UNFOCUS_FILE_SWITCH, 0);
    state_.active_toc_index = -1;

    const std::pmr::wstring dir = state_.document.doc.GetDirectory();
    if (!dir.empty()) {
        state_.file_explorer.SetDirectory(dir);
        state_.file_explorer.SetCurrentFile(state_.document.doc.GetFilePath());
    }

    // ビューポート優先レイアウト: 可視範囲のみ計測し、残りは遅延処理に委ねる。
    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    // スクロール位置の復元
    float scroll_y = 0.0f;

    const bool has_reload_diff = (state_.reload_diff_pos != std::string_view::npos);

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

    // cache.Reset()直後は全ノードの高さが0のため、スクロール復元前に
    // ノード高さを推定し、Mermaidキャッシュの実測値で補正する
    if (has_reload_diff || state_.view.scroll_restore.HasNodeRestore()) {
        if (!heights_estimated) {
            MENDO_PROFILE("EstimateNodeHeights");
            EstimateNodeHeights(state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme());
        }
        ApplyMermaidCacheHeights(md_width);
    }

    state_.view.viewport.ClearScrollTarget();

    if (has_reload_diff) {
        scroll_y = CalcScrollForDiff(state_.reload_diff_pos, md_height);
        state_.reload_diff_pos = std::string_view::npos;
        state_.view.viewport.SetScrollY(scroll_y);
    }
    else if (state_.view.scroll_restore.HasNodeRestore()) {
        state_.view.viewport.SetScrollTarget(
            state_.view.scroll_restore.pending_restore_node,
            static_cast<float>(state_.view.scroll_restore.pending_restore_offset));
        state_.view.viewport.ApplyScrollTarget(state_.document.layout_cache);
        scroll_y = state_.view.viewport.GetScrollY();
        state_.view.scroll_restore.ClearNodeRestore();
    }
    else {
        state_.view.viewport.SetScrollY(0.0f);
    }

    MENDO_TRACEF("FinishLoad: scroll_y=%.1f (0=top of file)", scroll_y);

    {
        MENDO_PROFILE("ViewportLayout(Initial)");
        layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache, md_width, md_height);
    }

    FinalizeLayout(md_height);

    UpdateTitleBar();

    doc_service_.StartWatching(state_.document.doc.GetFilePath(), [this]() {
        KillTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE);
        SetTimer(hwnd_, app_timer::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_DEBOUNCE_MS, nullptr);
    });
}

// ============================================================
// リロード
// ============================================================

void App::ReloadCurrentFile()
{
    const auto& path = state_.document.doc.GetFilePath();
    if (path.empty() || IsHelpPath(path)) {
        return;
    }
    // ローディングアニメーション表示中は重複リロードを抑制
    if (file_load_service_.IsLoading()) {
        return;
    }

    if (DocumentService::NeedsAsyncLoad(path)) {
        MENDO_TRACE("ReloadCurrentFile: async path");
        BeginAsyncLoad(path);
    }
    else {
        MENDO_TRACE("ReloadCurrentFile: sync path (DoReloadCurrentFile)");
        DoReloadCurrentFile();
    }
}

void App::DoReloadCurrentFile()
{
    MENDO_PROFILE("DoReloadCurrentFile");

    KillTimer(hwnd_, app_timer::LOADING_ANIM);
    file_load_service_.StopLoading();
    state_.active_toc_index = -1;

    if (state_.document.doc.GetFilePath().empty()) {
        return;
    }

    CancelPendingResources();

    // ファイルを読み込み、旧コンテンツとの差分位置をコピーなしで計算
    auto load_result = [this]() {
        MENDO_PROFILE("Reload::LoadFile");
        return FileLoader::LoadFile(state_.document.doc.GetFilePath());
    }();
    if (!load_result) {
        doc_service_.ResumeWatching();
        return;
    }
    std::pmr::string new_utf8 = std::move(*load_result);
    const std::string_view old_view(state_.document.doc.GetRawUtf8());
    const std::string_view new_view(new_utf8);
    const auto decision = AnalyzeReloadDiff(old_view, new_view);
    state_.pending_prefix_shrink = (decision.op == ReloadOp::DeferPrefixShrink);

    MENDO_TRACEF("DoReload: diff_pos=%zu old_size=%zu new_size=%zu op=%d",
        decision.diff_pos, old_view.size(), new_view.size(),
        static_cast<int>(decision.op));

    switch (decision.op) {
    case ReloadOp::NoChange:
        // 差分がなければリロード不要。エディタの保存操作が複数の通知を
        // 発生させた場合に、レイアウトキャッシュの不要なリセットを防ぐ。
        doc_service_.ResumeWatching();
        return;
    case ReloadOp::DeferPrefixShrink:
        // エディタの truncate→rewrite 2段階保存の前半（ファイル縮小）を検出。
        // state_.document.doc を更新せず元のコンテンツを保持し、次のリロードで正確な差分を検出する。
        doc_service_.ResumeWatching();
        return;
    case ReloadOp::PrefixGrowth:
    case ReloadOp::FullReload: {
        MENDO_PROFILE("Reload::ReplaceFromMarkdown");
        state_.document.doc.ReplaceFromMarkdown(std::move(new_utf8));
        FinishReload(decision.op == ReloadOp::PrefixGrowth, decision.diff_pos);
        return;
    }
    }
}

// DoReloadCurrentFile / OnParseComplete 共通のリロード後処理。
// ドキュメントは更新済みの状態で呼ばれる。
void App::FinishReload(bool is_prefix_only, size_t diff_pos)
{
    const float old_scroll = state_.view.viewport.GetScrollY();

    if (is_prefix_only) {
        state_.document.layout_cache.ResizePreservingPrefix(state_.document.doc.GetNodes().size());
    }
    else {
        state_.document.layout_cache.Reset(state_.document.doc.GetNodes().size(), false);
        EstimateNodeHeights(state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme());
    }

    renderer_.InvalidateTocPaneCache();

    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    if (!is_prefix_only) {
        // Mermaidブロックの推定高さをファイルキャッシュの実測値で上書きし、
        // スクロール位置のずれを防ぐ
        ApplyMermaidCacheHeights(md_width);
    }

    const float desired_scroll = is_prefix_only
        ? old_scroll
        : CalcScrollForDiff(diff_pos, md_height);

    MENDO_TRACEF("FinishReload: desired_scroll=%.1f old_scroll=%.1f is_prefix_only=%d",
        desired_scroll, old_scroll, is_prefix_only ? 1 : 0);

    // スクロール位置を設定してからViewportLayoutを呼ぶことで、
    // 変更箇所周辺の可視ノードが優先的に計測される。
    // リロード時は直前の navigation target を破棄し、ピクセル位置で固定する。
    state_.view.viewport.ClearScrollTarget();
    state_.view.viewport.SetScrollY(desired_scroll);

    {
        MENDO_PROFILE("Reload::ViewportLayout");
        layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache, md_width, md_height);
    }

    FinalizeLayout(md_height);

    if (state_.search.search_state.IsVisible() && !state_.search.search_state.GetQuery().empty()) {
        state_.search.search_bar_ctrl.RunSearchAndLocate(state_.document.doc.GetNodes());
    }

    doc_service_.ResumeWatching();
}

// ============================================================
// ファイル読み込みヘルパー
// ============================================================

float App::CalcScrollForDiff(size_t diff_pos, float viewport_height) const
{
    MENDO_TRACEF("CalcScrollForDiff: diff_pos=%zu node_count=%zu", diff_pos, state_.document.doc.GetNodes().size());
    return CalcScrollYForDiff(
        state_.document.doc.GetNodes(), state_.document.layout_cache,
        std::string_view(state_.document.doc.GetRawUtf8()),
        diff_pos, viewport_height, state_.view.viewport.GetScrollY());
}

void App::ApplyMermaidCacheHeights(float md_width)
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
    if (any_applied) {
        RecomputeYPositions(state_.document.doc.GetNodesMut(), state_.document.layout_cache, renderer_.GetTheme());
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
