#include "app.h"
#include "app_constants.h"
#include "app_state_queries.h"
#include "document_service.h"
#include "file_loader.h"
#include "file_io.h"
#include "i18n.h"
#include "document_utils.h"
#include "layout.h"
#include "layout_queries.h"
#include "profiler.h"
#include "rc_resource.h"
#include "reload_flow.h"
#include <utility>

using mendo::layout::RecomputeYPositions;

// 一時状態のリセットで左ドラッグ状態だけが消えると、LButtonUp がどの終了経路にも入らず
// SetCapture が残る (タイトルバーのドラッグや MouseLeave 検出が効かなくなる)。
void App::ReleaseCaptureIfDragDropped(bool had_left_drag)
{
    if (had_left_drag && !IsLeftDragActive(state_) && state_.interaction.gesture.GetPhase() == GesturePhase::Idle) {
        EmitEffect(effect::ReleaseCapture{});
    }
}

void App::CancelPendingResources()
{
    resource_manager_.CancelMermaidBatch();
    image_loader_.CancelPending();
    resource_manager_.ClearResolvedPaths();
}

void App::ResetViewForNewDocument()
{
    const bool had_left_drag = IsLeftDragActive(state_);
    state_.view.ResetForNewDocument();
    // 旧文書のマッチ位置が新 nodes に対して誤用されるのを防ぐ。
    state_.search.search_bar_ctrl.Reset();
    ReleaseCaptureIfDragDropped(had_left_drag);
    EmitEffect(effect::SearchUnfocus{ /*clear_text=*/true });
    CancelPendingResources();
    renderer_.ShrinkBuffers();
}

void App::StopLoadingAnimation()
{
    // スピナーを消すための再描画。後続が描き直さない経路 (同じファイルを開き直して NoChange) もある。
    if (file_load_service_.IsLoading()) {
        Invalidate();
    }
    EmitEffect(effect::KillTimer{ app_timer::Id::LOADING_ANIM });
    file_load_service_.StopLoading();
}

void App::LoadHelpDocument()
{
    if (IsHelpPath(state_.document.doc.GetFilePath())) {
        return;
    }

    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    StopLoadingAnimation();
    file_load_service_.CancelAsyncLoad();
    EmitEffect(effect::StopFileWatch{});

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    ReplaceDocument(Document::FromMarkdown(std::move(utf8), HELP_PATH));

    FinishLoadMarkdownFile(/*follow_file_pane=*/false);
}

namespace {
// 100MB 級の文書はテキスト・runs・リンク URL を arena ごと捨てても、数十万ノードの走査と
// 表等の解放で UI を十数 ms 止めるため worker で破棄する。
// 渡すものは COM / D2D の参照を持たないこと (確保元の既定 pmr リソースはスレッド安全)。
template <class T>
void DisposeOnWorkerIfLarge(TaskScheduler& scheduler, size_t node_count, T&& obj)
{
    constexpr size_t kBackgroundDisposeMinNodes = 4096;
    if (node_count >= kBackgroundDisposeMinNodes) {
        scheduler.Post([o = std::forward<T>(obj)] {});
    }
}
} // namespace

void App::ReplaceDocument(Document next, LayoutCache estimated)
{
    // 以降、doc のノードと layout_cache のエントリは常に同数 (描画・ヒットテスト等はこれを前提にする)。
    assert(estimated.size() == next.GetNodes().size());
    Document old = std::exchange(state_.document.doc, std::move(next));
    // 旧 LayoutCache は D2D 由来の参照を持つので worker に渡さず UI スレッドで破棄する。
    state_.document.layout_cache = std::move(estimated);
    DisposeOnWorkerIfLarge(scheduler_, old.GetNodes().size(), std::move(old));
    // ツールチップの同一判定は node / TOC の index で行うため、旧文書の対象を持ち越さない。
    InvalidateHitPositions();
}

void App::ReplaceDocument(Document next)
{
    LayoutCache estimated = mendo::layout::MakeEstimatedLayoutCache(next.GetNodes(), renderer_.GetTheme());
    ReplaceDocument(std::move(next), std::move(estimated));
}

void App::BeginAsyncLoad(std::pmr::wstring path, bool show_animation, std::shared_ptr<const std::pmr::string> reload_base)
{
    if (show_animation) {
        MENDO_PROFILE("App::BeginAsyncLoad with animation");
        file_load_service_.StartLoading(std::move(path));
        EmitEffect(effect::SetTimer{ app_timer::Id::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS });
        Invalidate();
        UpdateWindow(hwnd_);
    }
    else {
        MENDO_PROFILE("App::BeginAsyncLoad without animation");
        file_load_service_.SetLoadingPath(std::move(path));
    }
    file_load_service_.StartAsyncLoad(scheduler_, hwnd_, app_msg::PARSE_COMPLETE, renderer_.GetTheme(), std::move(reload_base));
}

// Defer* はエディタの保存途中 (truncate→rewrite の前半や書き込み中) を読んだ状態。
// state_.document.doc を更新せず保持して、次のリロードで正確な差分を取り直す。
App::ReloadFlow App::ApplyReloadDecisionEarly(const ReloadDecision& decision, bool has_parsed_doc)
{
    switch (decision.op) {
    case ReloadOp::NoChange:
        prefix_shrink_retry_.Reset();
        EmitEffect(effect::ResumeFileWatch{});
        return ReloadFlow::Handled;
    case ReloadOp::DeferPrefixShrink:
        // 上限後は ReloadCurrentFile がパース込みで読み直すので、文書が無い結果は defer のまま流す。
        if (prefix_shrink_retry_.TryDefer() || !has_parsed_doc) {
            DeferReloadRetry();
            return ReloadFlow::Handled;
        }
        break;
    case ReloadOp::DeferPartialWrite:
        DeferReloadRetry();
        return ReloadFlow::Handled;
    case ReloadOp::Replace:
        break;
    }
    prefix_shrink_retry_.Reset();
    return ReloadFlow::ContinueWithReload;
}

void App::DeferReloadRetry()
{
    // FileWatcher は paused のまま維持する。resume すると待機中の変更通知が
    // FILE_RELOAD_DEBOUNCE を 200ms に上書きし、短縮リトライが効かなくなる。
    EmitEffect(effect::SetTimer{ app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_RETRY_MS });
}

// reducer を経由せず App 層で直接実行する。ファイル I/O + 同期/非同期ロード分岐 +
// パース + レイアウト初期化を含む大きな命令的ワークフローで、reducer 化すると
// effect variant と executor が肥大化するため意図的に service 経路として分離している。
void App::LoadMarkdownFile(std::wstring_view path)
{
    MENDO_PROFILE("App::LoadMarkdownFile");
    EmitEffect(effect::KillTimer{ app_timer::Id::FILE_RELOAD_DEBOUNCE });
    prefix_shrink_retry_.Reset();
    // 仮想パスは IsAsyncLoadCandidate が true を返し非同期ロードが失敗するため、
    // 先に検出して同期ロードに回す。
    if (IsHelpPath(path)) {
        LoadHelpDocument();
        return;
    }
    std::pmr::wstring path_str{ path };
    const auto size = QueryFileSize(path_str.c_str());
    if (DocumentService::IsAsyncLoadCandidate(size)) {
        BeginAsyncLoad(std::move(path_str), DocumentService::ShouldShowLoadingAnimation(size));
        return;
    }
    file_load_service_.CancelAsyncLoad();
    file_load_service_.SetLoadingPath(std::move(path_str));
    DoLoadMarkdownFile();
}

// 同じファイルの再読み込みは FileWatcher を paused にしたまま進んでいるので止めない。
void App::CancelOtherFileLoad()
{
    if (!file_load_service_.IsAsyncLoading() ||
        path_util::iequal(file_load_service_.GetLoadingPath(), state_.document.doc.GetFilePath())) {
        return;
    }
    StopLoadingAnimation();
    file_load_service_.CancelAsyncLoad();
}

// Init (RestoreThemeAndZoom) 前に呼ばれるため、推定用のテーマは設定から直接作る。
// 推定に使う寸法はライト/ダーク共通でズームだけに依存するので、ダークモード未読込でも結果は同じ。
void App::StartPreloadAsync(std::pmr::wstring path)
{
    file_load_service_.StartPreloadAsync(std::move(path), theme_service_.CreateTheme(theme_service_.LoadZoomIndex()));
}

void App::DoLoadMarkdownFile()
{
    MENDO_PROFILE("DoLoadMarkdownFile");

    EmitEffect(effect::KillTimer{ app_timer::Id::LOADING_ANIM });

    const bool follow_file_pane = FilePaneFollowsLoad(state_.document.doc.GetFilePath(), file_load_service_.GetLoadingPath());
    {
        MENDO_PROFILE("ExecuteLoad(FileIO+Parse)");
        auto load_result = file_load_service_.ExecuteLoad();
        if (!load_result) {
            ShowToast(FileLoadErrorMessage(load_result.error(), i18n::S()));
            HandleLoadFailureFallback();
            return;
        }
        ReplaceDocument(std::move(*load_result));
    }

    FinishLoadMarkdownFile(follow_file_pane);
}

void App::HandleLoadFailureFallback()
{
    Dispatch(LoadFailedAction{});
    if (state_.document.doc.IsEmpty()) {
        LoadHelpDocument();
    }
    else {
        Invalidate();
    }
}

void App::OnParseComplete()
{
    MENDO_PROFILE("OnParseComplete");

    auto outcome = file_load_service_.TakeAsync();
    const auto plan = PlanParseComplete(
        outcome, file_load_service_.IsAsyncLoading(), state_.document.doc,
        [](const std::pmr::wstring& path, size_t read_size) { return IsFileLargerThan(path.c_str(), read_size); });

    if (plan.step == ParseCompleteStep::IgnoreStale) {
        MENDO_TRACE("OnParseComplete: stale message ignored (new load in progress)");
        return;
    }

    StopLoadingAnimation();

    // ApplyReload / ReplaceWithResult のときだけ結果がある。
    AsyncLoadResult* const result = (outcome && *outcome) ? &**outcome : nullptr;
    if (plan.reload) {
        MENDO_TRACEF("OnParseComplete: reload worker_diff={} node_count={} diff_pos={} new_size={} op={}",
                     result->reload.has_value(), result->doc.GetNodes().size(), plan.reload->diff_pos,
                     result->doc.GetRawText().size(), std::to_underlying(plan.reload->op));
    }

    switch (plan.step) {
    case ParseCompleteStep::IgnoreStale:
        return;
    case ParseCompleteStep::Fail:
        MENDO_TRACE("OnParseComplete: no result (cancelled or load failed)");
        EmitEffect(effect::ResumeFileWatch{});
        if (plan.error) {
            ShowToast(FileLoadErrorMessage(*plan.error, i18n::S()));
        }
        HandleLoadFailureFallback();
        return;
    case ParseCompleteStep::RetryDocumentChanged:
    case ParseCompleteStep::RetryPartialWrite:
        // worker が作った推定済み LayoutCache は D2D 由来の参照を持たないので、文書ごと worker で捨てられる。
        if (result) {
            DisposeOnWorkerIfLarge(scheduler_, result->doc.GetNodes().size(), std::move(outcome));
        }
        DeferReloadRetry();
        return;
    case ParseCompleteStep::ApplyReload:
        if (ApplyReloadDecisionEarly(*plan.reload, !result->reload.has_value()) == ReloadFlow::Handled) {
            return;
        }
        ReplaceDocument(std::move(result->doc), std::move(result->cache));
        FinishReload(plan.reload->diff_pos);
        return;
    case ParseCompleteStep::ReplaceWithResult:
        ReplaceDocument(std::move(result->doc), std::move(result->cache));
        FinishLoadMarkdownFile(plan.follow_file_pane);
        return;
    }
}

void App::FinishLoadMarkdownFile(bool follow_file_pane)
{
    MENDO_PROFILE("FinishLoadMarkdownFile");

    ResetViewForNewDocument();

    const std::pmr::wstring& dir = state_.document.doc.GetDirectory();
    if (follow_file_pane && !dir.empty()) {
        state_.file_explorer.SetDirectory(dir);
    }
    state_.file_explorer.SetCurrentFile(state_.document.doc.GetFilePath());

    const PaneRect md = GetPaneLayout().md_rect;

    // スクロール復元先の Y がずれないよう、推定高さを Mermaid/画像キャッシュの実測値で補正する。
    if (state_.view.scroll_restore.HasNodeRestore()) {
        MENDO_TRACEF("FinishLoad: node={} offset={}",
                     state_.view.scroll_restore.pending_restore_node,
                     state_.view.scroll_restore.pending_restore_offset);
        ApplyCachedHeightsAndRecompute();
    }
    Dispatch(RestoreScrollAfterLoadAction{});

    {
        MENDO_PROFILE("ViewportLayout(Initial)");
        ViewportLayout(md.width, md.height);
    }

    FinalizeLayout(md.height);

    MENDO_TRACEF("FinishLoad: scroll_y={:.1f} max_scroll={:.1f}",
                 state_.view.viewport.GetScrollY(),
                 state_.view.viewport.GetMaxScroll());

    UpdateTitleBar();
    EmitEffect(effect::SyncTocActive{});
    if (state_.document.doc.HasBackingFile()) {
        EmitEffect(effect::StartFileWatch{ state_.document.doc.GetFilePath() });
    }
}

void App::ReloadCurrentFile()
{
    MENDO_PROFILE("ReloadCurrentFile");

    if (!state_.document.doc.HasBackingFile()) {
        return;
    }
    // テキスト選択ドラッグ中にリロードすると ClearSelection が is_dragging_ も落として
    // ドラッグ確定 (TextSelectionEndedAction) が発火しなくなる。ドラッグ終了後に
    // リトライさせる (DeferReloadRetry がタイマを再セットし FileWatcher は paused 維持)。
    if (state_.view.viewport.IsDragging()) {
        DeferReloadRetry();
        return;
    }
    // FileWatcher のバーストで重複スケジュールされないよう、進行中のロードが
    // あれば即 return する。show_animation=false 経路では IsLoading() が立たない
    // ため IsAsyncLoading() も併せて見る。
    if (file_load_service_.IsLoading() || file_load_service_.IsAsyncLoading()) {
        return;
    }
    // ここまでに届いた通知の変更はこれから読む内容に含まれる。残すと Resume で同じ保存をもう一度読み直す。
    file_watcher_.DiscardPendingChange();

    const auto& path = state_.document.doc.GetFilePath();
    if (DocumentService::IsAsyncLoadCandidate(path)) {
        MENDO_TRACE("ReloadCurrentFile: async path");
        // 編集中の差分リロードでスピナーが点滅すると見づらいため、旧コンテンツを表示したまま差し替える。
        // prefix shrink の defer が上限に達したら、縮んだ内容を受け入れるため worker の事前判定を省いてパースさせる。
        BeginAsyncLoad(path, /*show_animation=*/false,
                       prefix_shrink_retry_.Exhausted() ? nullptr : state_.document.doc.GetRawText().Share());
        return;
    }
    MENDO_TRACE("ReloadCurrentFile: sync path (DoReloadCurrentFile)");
    DoReloadCurrentFile();
}

void App::DoReloadCurrentFile()
{
    MENDO_PROFILE("DoReloadCurrentFile");

    auto load_result = FileLoader::LoadFile(state_.document.doc.GetFilePath());
    if (!load_result) {
        EmitEffect(effect::ResumeFileWatch{});
        return;
    }

    // 読んでいる間にも書き込みが続いていれば、書き終わるまで待ってから読み直す。
    if (IsFileLargerThan(state_.document.doc.GetFilePath().c_str(), load_result->byte_size)) {
        DeferReloadRetry();
        return;
    }

    const size_t byte_size = load_result->byte_size;
    std::pmr::string new_text = std::move(load_result->text);

    const std::string_view old_view(state_.document.doc.GetRawText());
    const auto decision = AnalyzeReloadDiff(old_view, new_text);

    MENDO_TRACEF("DoReload: diff_pos={} old_size={} new_size={} op={}",
                 decision.diff_pos, old_view.size(), new_text.size(),
                 std::to_underlying(decision.op));

    if (ApplyReloadDecisionEarly(decision, /*has_parsed_doc=*/true) == ReloadFlow::Handled) {
        return;
    }
    ReplaceDocument(Document::FromMarkdown(std::move(new_text), byte_size, state_.document.doc.GetFilePath()));
    FinishReload(decision.diff_pos);
}

// DoReloadCurrentFile / OnParseComplete 共通のリロード後処理。
// ドキュメントは更新済みの状態で呼ばれる。
void App::FinishReload(size_t diff_pos)
{
    MENDO_PROFILE("FinishReload");

    // 旧ノード番号で発行した要求を捨てる。編集で画像が置かれた可能性があるので失敗記録も消す。
    CancelPendingResources();
    image_loader_.ResetFailedPaths();

    // ノード index がずれると per-node-index の一時状態が別ノードを指すためクリアする
    // (別文書への切替は ViewState::ResetForNewDocument が担う)。
    const bool had_left_drag = IsLeftDragActive(state_);
    state_.view.ResetPerNodeTransientState();
    ReleaseCaptureIfDragDropped(had_left_drag);

    const PaneRect md = GetPaneLayout().md_rect;

    // Mermaid/LaTeX 図と通常画像の推定高さを実測値 (file_cache_ / image_loader メモリキャッシュ) で
    // 上書きし、CalcScrollYForDiff の Y 計算がずれないようにする。
    ApplyCachedHeightsAndRecompute();

    const float desired_scroll = CalcScrollYForDiff(
        state_.document.doc.GetNodes(), state_.document.layout_cache,
        std::string_view{ state_.document.doc.GetRawText() },
        diff_pos, md.height, state_.view.viewport.GetScrollY());

    MENDO_TRACEF("FinishReload: desired_scroll={:.1f} diff_pos={}",
                 desired_scroll, diff_pos);

    // スクロール位置を先に設定してから ViewportLayout を呼ぶことで、変更箇所周辺の
    // 可視ノードが優先的に計測される。リロード時は直前の navigation target を破棄し、
    // ピクセル位置で固定する。
    state_.view.viewport.SetScrollY(desired_scroll);

    {
        MENDO_PROFILE("Reload::ViewportLayout");
        ViewportLayout(md.width, md.height);
    }

    FinalizeLayout(md.height);

    MENDO_TRACEF("FinishReload: after-finalize scroll_y={:.1f} max_scroll={:.1f}",
                 state_.view.viewport.GetScrollY(),
                 state_.view.viewport.GetMaxScroll());

    if (state_.search.search_state.IsVisible() && !state_.search.search_state.GetQuery().empty()) {
        state_.search.search_bar_ctrl.RunSearchAndLocate(state_.document.doc.GetNodes());
    }

    EmitEffect(effect::SyncTocActive{});

    EmitEffect(effect::ResumeFileWatch{});
}

void App::ApplyCachedHeightsAndRecompute()
{
    const bool mermaid_applied = resource_manager_.ApplyMermaidCacheHeightsForReload();
    const bool image_applied = resource_manager_.ApplyCachedImagesForReload() > 0;
    if (mermaid_applied || image_applied) {
        RecomputeYPositions(
            state_.document.doc.GetNodes(),
            state_.document.layout_cache,
            renderer_.GetTheme());
    }
}
