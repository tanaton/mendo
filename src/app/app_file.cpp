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
    state_.view.viewport.ClearSelection();
    CancelPendingResources();
    renderer_.ShrinkBuffers();
    doc_service_.StopWatching();

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    state_.document.doc = Document::FromMarkdown(std::move(utf8), HELP_PATH);
    state_.document.layout_cache.Reset(state_.document.doc.GetNodes().size());

    state_.file_explorer.SetCurrentFile(L"");
    renderer_.InvalidateFilePaneCache();

    state_.view.panes.ResetScrollStates();
    renderer_.InvalidateTocPaneCache();

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
            Invalidate();
            return;
        }
    }

    FinishLoadMarkdownFile();
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
        Invalidate();
        return;
    }

    // 差分ベースのスキップ／スクロール復元は同一パスのリロード時のみ有効。
    // 非同期のファイルオープンでも OnParseComplete() が使われるため、
    // 別ファイル読み込み時は差分ロジックをスキップする。
    if (_wcsicmp(result->doc.GetFilePath().c_str(), state_.document.doc.GetFilePath().c_str()) == 0) {
        const std::string_view old_view(state_.document.doc.GetRawUtf8());
        const std::string_view new_view(result->doc.GetRawUtf8());
        const size_t diff_pos = FindFirstDifference(old_view, new_view);

        MENDO_TRACEF("OnParseComplete: reload node_count=%zu diff_pos=%zu old_size=%zu new_size=%zu",
            result->doc.GetNodes().size(), diff_pos, old_view.size(), new_view.size());

        if (diff_pos == std::string_view::npos) {
            // 差分なし → リロード不要、監視再開
            state_.pending_prefix_shrink = false;
            doc_service_.ResumeWatching();
            Invalidate();
            return;
        }

        // diff_pos が短い方の末尾と一致 = 片方がもう片方のprefixで、
        // ファイルの伸縮に過ぎない場合はスクロール位置を維持する
        const bool is_prefix_only = IsPrefixOnlyDiff(diff_pos, old_view.size(), new_view.size());

        // エディタの truncate→rewrite 2段階保存の前半（ファイル縮小）を検出。
        // state_.document.doc を更新せず元のコンテンツを保持することで、次のリロードで
        // 「元コンテンツ vs 最終コンテンツ」の正確な差分を検出できるようにする。
        if (ShouldDeferForTruncateRewrite(is_prefix_only, old_view.size(), new_view.size())) {
            return;
        }

        if (is_prefix_only) {
            // prefix-only はファイル末尾の伸縮のみ。FinishReload が
            // ResizePreservingPrefix で旧キャッシュの実測高さを保持する。
            resource_manager_.CancelMermaidBatch();
            state_.document.doc = std::move(result->doc);
            FinishReload(true, diff_pos, state_.reload_old_scroll);
            return;
        }

        state_.reload_diff_pos = diff_pos;
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
    state_.view.viewport.ClearSelection();
    state_.search.search_bar_ctrl.Reset();
    PostMessage(hwnd_, app_msg::SEARCH_UNFOCUS, app_param::SEARCH_UNFOCUS_FILE_SWITCH, 0);
    state_.active_toc_index = -1;
    CancelPendingResources();
    renderer_.ShrinkBuffers();

    const std::pmr::wstring dir = state_.document.doc.GetDirectory();
    if (!dir.empty()) {
        state_.file_explorer.SetDirectory(dir);
        state_.file_explorer.SetCurrentFile(state_.document.doc.GetFilePath());
    }

    state_.view.panes.ResetScrollStates();
    renderer_.InvalidateFilePaneCache();
    renderer_.InvalidateTocPaneCache();

    // ビューポート優先レイアウト: 可視範囲のみ計測し、残りは遅延処理に委ねる。
    const auto pane_layout = GetPaneLayout();
    const float md_width = pane_layout.md_rect.width;
    const float md_height = pane_layout.md_rect.height;

    // スクロール位置の復元
    float scroll_y = 0.0f;

    const bool has_reload_diff = (state_.reload_diff_pos != std::string_view::npos);

    MENDO_TRACEF("FinishLoad: has_reload_diff=%d HasNodeRestore=%d HasNavScroll=%d nav_scroll_y=%.1f heights_estimated=%d",
        has_reload_diff ? 1 : 0,
        state_.view.scroll_restore.HasNodeRestore() ? 1 : 0,
        state_.view.scroll_restore.HasNavScroll() ? 1 : 0,
        state_.view.scroll_restore.HasNavScroll() ? state_.view.scroll_restore.pending_nav_scroll_y : -1.0f,
        heights_estimated ? 1 : 0);

    // cache.Reset()直後は全ノードの高さが0のため、スクロール復元前に
    // ノード高さを推定し、Mermaidキャッシュの実測値で補正する
    if (has_reload_diff
        || state_.view.scroll_restore.HasNodeRestore()
        || (state_.view.scroll_restore.HasNavScroll() && state_.view.scroll_restore.pending_nav_scroll_y > 0.0f)) {
        if (!heights_estimated) {
            MENDO_PROFILE("EstimateNodeHeights");
            EstimateNodeHeights(state_.document.doc.GetNodes(), state_.document.layout_cache, renderer_.GetTheme());
        }
        ApplyMermaidCacheHeights(md_width);
    }

    if (has_reload_diff) {
        scroll_y = CalcScrollForDiff(state_.reload_diff_pos, md_height, state_.reload_old_scroll);
        state_.reload_diff_pos = std::string_view::npos;
    }
    else if (state_.view.scroll_restore.HasNodeRestore()) {
        const int node = std::min(state_.view.scroll_restore.pending_restore_node,
            static_cast<int>(state_.document.layout_cache.size()) - 1);
        if (node >= 0) {
            scroll_y = std::max(0.0f,
                state_.document.layout_cache[node].y_position + static_cast<float>(state_.view.scroll_restore.pending_restore_offset));
        }
        state_.view.scroll_restore.ClearNodeRestore();
    }
    else if (state_.view.scroll_restore.HasNavScroll()) {
        scroll_y = state_.view.scroll_restore.ConsumeNavScroll();
        // 遅延レイアウトのドリフト補正用（セッション復元と同じ仕組み）
        state_.view.scroll_restore.pending_restore_scroll_y = scroll_y;
    }
    else {
        // 新規ファイルオープン: 前回ナビゲーションの残留値をクリア
        state_.view.scroll_restore.pending_restore_scroll_y = -1;
    }

    MENDO_TRACEF("FinishLoad: scroll_y=%.1f (0=top of file)", scroll_y);

    state_.view.viewport.SetScrollY(scroll_y);

    // 推定→計測の高さ差をアンカー補償
    const bool need_anchor = (scroll_y > 0.0f);
    const auto anchor = need_anchor ? SaveAnchor() : AnchorState{};

    {
        MENDO_PROFILE("ViewportLayout(Initial)");
        layout_service_->ViewportLayout(state_.document.doc, state_.document.layout_cache, md_width, md_height);
    }

    if (need_anchor) {
        state_.view.viewport.AnchorCompensateScroll(anchor.idx, anchor.y_before, state_.document.layout_cache);
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
        state_.reload_old_scroll = state_.view.viewport.GetScrollY();
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

    const float old_scroll = state_.view.viewport.GetScrollY();

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
    const size_t diff_pos = FindFirstDifference(old_view, new_view);

    MENDO_TRACEF("DoReload: diff_pos=%zu old_size=%zu new_size=%zu old_scroll=%.1f",
        diff_pos, old_view.size(), new_view.size(), old_scroll);

    // 差分がなければリロード不要。エディタの保存操作が複数の通知を
    // 発生させた場合に、レイアウトキャッシュの不要なリセットを防ぐ。
    if (diff_pos == std::string_view::npos) {
        state_.pending_prefix_shrink = false;
        doc_service_.ResumeWatching();
        return;
    }

    // 変更箇所のスクロール位置を決定
    // diff_pos が短い方の末尾と一致 = 片方がもう片方のprefixで、
    // ファイルの伸縮に過ぎない場合はスクロール位置を維持する
    const bool is_prefix_only = IsPrefixOnlyDiff(diff_pos, old_view.size(), new_view.size());

    // エディタの truncate→rewrite 2段階保存の前半（ファイル縮小）を検出。
    // state_.document.doc を更新せず元のコンテンツを保持し、次のリロードで正確な差分を検出する。
    if (ShouldDeferForTruncateRewrite(is_prefix_only, old_view.size(), new_view.size())) {
        return;
    }

    // ドキュメントを新コンテンツで更新
    {
        MENDO_PROFILE("Reload::ReplaceFromMarkdown");
        state_.document.doc.ReplaceFromMarkdown(std::move(new_utf8));
    }

    FinishReload(is_prefix_only, diff_pos, old_scroll);
}

// DoReloadCurrentFile / OnParseComplete 共通のリロード後処理。
// ドキュメントは更新済みの状態で呼ばれる。
// is_prefix_only: ファイル末尾の伸縮のみか
// diff_pos: 差分開始位置 (prefix-only の場合は使用しない)
// old_scroll: リロード前のスクロール位置
void App::FinishReload(bool is_prefix_only, size_t diff_pos, float old_scroll)
{
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
        : CalcScrollForDiff(diff_pos, md_height, old_scroll);

    MENDO_TRACEF("FinishReload: desired_scroll=%.1f old_scroll=%.1f is_prefix_only=%d",
        desired_scroll, old_scroll, is_prefix_only ? 1 : 0);

    // スクロール位置を設定してからViewportLayoutを呼ぶことで、
    // 変更箇所周辺の可視ノードが優先的に計測される
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

bool App::ShouldDeferForTruncateRewrite(bool is_prefix_only, size_t old_size, size_t new_size)
{
    if (is_prefix_only && new_size < old_size) {
        // prefix-only shrink が連続する場合もすべて defer する。
        // エディタによっては truncate が複数回発生してから rewrite されることがある。
        state_.pending_prefix_shrink = true;
        doc_service_.ResumeWatching();
        return true;
    }
    state_.pending_prefix_shrink = false;
    return false;
}

float App::CalcScrollForDiff(size_t diff_pos, float viewport_height, float fallback_scroll) const
{
    MENDO_TRACEF("CalcScrollForDiff: diff_pos=%zu node_count=%zu", diff_pos, state_.document.doc.GetNodes().size());
    return CalcScrollYForDiff(
        state_.document.doc.GetNodes(), state_.document.layout_cache,
        std::string_view(state_.document.doc.GetRawUtf8()),
        diff_pos, viewport_height, fallback_scroll);
}

void App::ApplyMermaidCacheHeights(float md_width)
{
    const float content_width = renderer_.GetTheme().ContentWidth(md_width);
    const bool dark_mode = theme_service_.IsDarkMode();
    const auto& nodes = state_.document.doc.GetNodes();
    bool any_applied = false;
    for (size_t i : state_.document.doc.GetMermaidNodeIndices()) {
        const auto hash = mermaid_util::HashCode(nodes[i].text_utf8, content_width, dark_mode);
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
