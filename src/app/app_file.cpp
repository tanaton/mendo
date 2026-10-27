#include "app.h"
#include "app_constants.h"
#include "file_io.h"
#include "i18n.h"
#include "document_utils.h"
#include "layout.h"
#include "layout_computer.h"
#include "profiler.h"
#include "win32_util.h"
#include "reload_flow.h"
#include <utility>

using layout::RecomputeYPositions;

void App::CancelPendingResources()
{
    resource_manager_.CancelMermaidBatch();
    mermaid_renderer_.DropQueued();
    image_loader_.DropQueued();
}

void App::RequestDroppedResourcesAgain()
{
    if (!std::exchange(resources_dropped_for_load_, false)) {
        return;
    }
    resource_manager_.ScheduleBitmapManage();
    Invalidate();
}

void App::StartLoadingAnimation()
{
    spinner_shown_ = true;
    spinner_angle_ = 0.0f;
    SetTimer(app_timer::Id::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS);
    Invalidate();
}

void App::ScheduleLoadingAnimation()
{
    // 回っている最中に次の読み込みが始まったら、そのまま回し続ける。
    if (!spinner_shown_) {
        SetTimer(app_timer::Id::LOADING_ANIM, app_timer::LOADING_SPINNER_DELAY_MS);
    }
}

void App::StopLoadingAnimation()
{
    // スピナーを消すための再描画。後続が描き直さない経路 (同じファイルを開き直して NoChange) もある。
    if (spinner_shown_) {
        Invalidate();
    }
    // 出す前のスピナーの遅延もここで止める。
    KillTimer(app_timer::Id::LOADING_ANIM);
    spinner_shown_ = false;
}

void App::ShowHelpResource()
{
    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    StopLoadingAnimation();
    load_coordinator_.Cancel();
    file_watcher_.StopWatching();

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    Document help = Document::FromMarkdown(std::move(utf8), HELP_PATH);
    LayoutCache estimated = layout::MakeEstimatedLayoutCache(help.GetNodes(), theme_service_.GetTheme(), MdContentWidth());
    OpenDocument(std::move(help), std::move(estimated), /*follow_file_pane=*/false);
}

namespace {
// IDWriteTextLayout の解放は 1 個 2〜3µs かかる。
constexpr size_t kDisposeMinLayouts = 256;
} // namespace

void App::DisposeReleasedLayouts()
{
    auto released = state_.document.layout_cache.TakeReleasedLayouts();
    DisposeOnWorkerIfLarge(scheduler_, released.size(), kDisposeMinLayouts, std::move(released));
}

void App::ReplaceDocument(Document next, LayoutCache estimated)
{
    // 以降、doc のノードと layout_cache のエントリは常に同数 (描画・ヒットテスト等はこれを前提にする)。
    assert(estimated.size() == next.GetNodes().size());
    resources_dropped_for_load_ = false;
    Document old = std::exchange(state_.document.doc, std::move(next));
    LayoutCache old_cache = std::exchange(state_.document.layout_cache, std::move(estimated));
    renderer_.ForgetDocumentText();
    old_cache.ReleaseDiagrams();
    // 表は 1 ノードにセルの数だけレイアウトを持つので、ノード数によらずワーカーで捨てる。
    scheduler_.Post([o = std::move(old_cache)] {});
    DisposeOnWorkerIfLarge(scheduler_, old.GetNodes().size(), kDisposeMinNodes, std::move(old));
}

void App::BeginAsyncLoad(std::pmr::wstring path, bool show_animation, ReloadBase reload_base)
{
    MENDO_PROFILE("App::BeginAsyncLoad");
    if (show_animation) {
        ScheduleLoadingAnimation();
    }
    // 進行中の読み込み (preload を含む) は置き換える。
    load_coordinator_.Start(scheduler_, std::move(path), theme_service_.GetTheme(), MdContentWidth(), std::move(reload_base));
}

// DeferPrefixShrink はエディタの保存途中 (truncate→rewrite の前半) を読んだ状態。
// state_.document.doc を更新せず保持して、次のリロードで正確な差分を取り直す。
App::ReloadFlow App::ApplyReloadDecisionEarly(const ReloadDecision& decision, bool has_parsed_doc)
{
    switch (decision.op) {
    case ReloadOp::NoChange:
        prefix_shrink_retry_.Reset();
        file_watcher_.ResumeWatching();
        return ReloadFlow::Handled;
    case ReloadOp::DeferPrefixShrink:
        // 上限後は ReloadCurrentFile がパース込みで読み直すので、文書が無い結果は defer のまま流す。
        if (prefix_shrink_retry_.TryDefer() || !has_parsed_doc) {
            DeferReloadRetry();
            return ReloadFlow::Handled;
        }
        break;
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
    SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_RETRY_MS);
}

// reducer を経由せず App 層で直接実行する。ファイル I/O + パース + レイアウト初期化を含む
// 大きな命令的ワークフローで、reducer 化すると effect variant と App::Execute が肥大化するため
// 意図的に service 経路として分離している。
// 読み込みとパースは大きさによらず worker で行い、UI スレッドではファイルに触れない (ネットワーク上のファイルで止まらない)。
void App::LoadMarkdownFile(std::wstring_view path)
{
    MENDO_PROFILE("App::LoadMarkdownFile");
    KillTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE);
    // 止めたリロードや取り消される進行中のリロードは、このロードが取り消し・失敗で終わったときに読み直す。
    file_watcher_.RequeueIfPaused();
    prefix_shrink_retry_.Reset();
    // 仮想パスはファイルとして読めないので、埋め込みリソースから開く。
    if (IsHelpPath(path)) {
        ShowHelpResource();
        return;
    }
    // 旧文書の画像・図の待ちはもう要らない。差し替えずに終わったら RequestDroppedResourcesAgain が要求し直す。
    // 開いている文書を開き直すとリロードとして扱われ、図と画像はコードやパスがキーなので待ちをそのまま使える。
    if (!path_util::iequal(path, state_.document.doc.GetFilePath())) {
        CancelPendingResources();
        resources_dropped_for_load_ = true;
    }
    BeginAsyncLoad(std::pmr::wstring{ path }, /*show_animation=*/true);
}

// 同じファイルの再読み込みは FileWatcher を paused にしたまま進んでいるので止めない。
void App::CancelOtherFileLoad()
{
    if (!load_coordinator_.IsActive() ||
        path_util::iequal(load_coordinator_.LoadingPath(), state_.document.doc.GetFilePath())) {
        return;
    }
    StopLoadingAnimation();
    load_coordinator_.Cancel();
    file_watcher_.ResumeWatching();
    RequestDroppedResourcesAgain();
}

// Init (RestoreTheme) 前に呼ばれるため、推定用のテーマは設定から直接作る。
// 推定に使う寸法はライト/ダーク共通でズームだけに依存するので、ダークモード未読込でも結果は同じ。
void App::StartPreloadAsync(std::pmr::wstring path)
{
    InitSharedScheduler();
    load_coordinator_.Start(scheduler_, std::move(path), theme_service_.CreateTheme(theme_service_.LoadZoomLevel()));
}

void App::HandleLoadFailureFallback(std::optional<FileLoadError> error)
{
    if (error) {
        ShowToast(FileLoadErrorMessage(*error, i18n::S()));
    }
    // ファイル一覧から開いたファイルが消えていたなら、一覧を今の中身に直す。
    if (error == FileLoadError::NotFound) {
        RefreshFilePane();
    }
    file_watcher_.ResumeWatching();
    Dispatch(LoadFailedAction{});
    if (state_.document.doc.IsEmpty()) {
        ShowHelpResource();
    }
}

void App::OnParseComplete()
{
    MENDO_PROFILE("OnParseComplete");

    auto outcome = load_coordinator_.Take();
    const auto plan = PlanParseComplete(outcome, state_.document.doc);

    if (plan.step == ParseCompleteStep::IgnoreStale) {
        MENDO_TRACE("OnParseComplete: stale message ignored (new load in progress)");
        return;
    }

    StopLoadingAnimation();

    // ApplyReload / ReplaceWithResult のときだけ結果がある。
    AsyncLoadResult* const result = (outcome && *outcome) ? &**outcome : nullptr;
    if (plan.reload) {
        MENDO_TRACEF(
            "OnParseComplete: reload worker_diff={} node_count={} diff_pos={} new_size={} op={}",
            result->reload.has_value(), result->doc.GetNodes().size(), plan.reload->diff_pos,
            result->doc.GetRawText().size(), std::to_underlying(plan.reload->op));
    }

    if (plan.step != ParseCompleteStep::ReplaceWithResult) {
        RequestDroppedResourcesAgain();
    }

    switch (plan.step) {
    case ParseCompleteStep::IgnoreStale:
        return;
    case ParseCompleteStep::Fail:
        MENDO_TRACE("OnParseComplete: no result (cancelled or load failed)");
        HandleLoadFailureFallback(plan.error);
        return;
    case ParseCompleteStep::Retry:
        // worker が作った推定済み LayoutCache は D2D 由来の参照を持たないので、文書ごと worker で捨てられる。
        if (result) {
            DisposeOnWorkerIfLarge(scheduler_, result->doc.GetNodes().size(), kDisposeMinNodes, std::move(outcome));
        }
        DeferReloadRetry();
        return;
    case ParseCompleteStep::ApplyReload:
        if (ApplyReloadDecisionEarly(*plan.reload, !result->reload.has_value()) == ReloadFlow::Handled) {
            return;
        }
        // 読み始めた後にテキスト選択のドラッグが始まった。読み直しはドラッグの終わりに回す。
        if (DeferReloadWhileSelecting()) {
            DisposeOnWorkerIfLarge(scheduler_, result->doc.GetNodes().size(), kDisposeMinNodes, std::move(outcome));
            return;
        }
        ReloadDocument(std::move(result->doc), std::move(result->cache), result->reload ? result->reload->ends : std::nullopt, plan.reload->diff_pos);
        return;
    case ParseCompleteStep::ReplaceWithResult:
        OpenDocument(std::move(result->doc), std::move(result->cache), plan.follow_file_pane);
        return;
    }
    std::unreachable();
}

void App::OpenDocument(Document next, LayoutCache estimated, bool follow_file_pane)
{
    MENDO_PROFILE("OpenDocument");

    ReplaceDocument(std::move(next), std::move(estimated));
    CancelPendingResources();

    // スクロール復元先の Y がずれないよう、推定高さを Mermaid/画像キャッシュの実測値で補正してから reducer に渡す。
    if (state_.view.pending_restore.IsValid()) {
        MENDO_TRACEF("FinishLoad: node={} offset={}", state_.view.pending_restore.node, state_.view.pending_restore.offset);
        if (ApplyCachedHeights({ 0, state_.document.doc.GetNodes().size() })) {
            RecomputeYPositions(state_.document.doc.GetNodes(), state_.document.layout_cache, theme_service_.GetTheme());
        }
    }
    Dispatch(DocumentOpenedAction{ follow_file_pane });

    RelayoutViewport(RelayoutResources::ImagesAndDiagrams);

    MENDO_TRACEF("FinishLoad: scroll_y={:.1f} max_scroll={:.1f}", state_.view.viewport.GetScrollY(), MdMaxScroll());

    UpdateTitleBar();
    if (state_.document.doc.HasBackingFile()) {
        StartFileWatch(state_.document.doc.GetFilePath());
    }
    // 開いただけの文書は読むだけかもしれないので、変更の通知で読み直すまでは短く閉じる。
    mermaid_renderer_.SetEditingDiagrams(false);
}

void App::ReloadCurrentFile()
{
    MENDO_PROFILE("ReloadCurrentFile");

    if (!state_.document.doc.HasBackingFile()) {
        return;
    }
    if (DeferReloadWhileSelecting()) {
        return;
    }
    // FileWatcher のバーストで重複スケジュールされないよう、進行中のロードがあれば即 return する。
    if (load_coordinator_.IsActive()) {
        // 別ファイルのロード中なら、それが取り消し・失敗で終わったときの Resume で読み直す。
        file_watcher_.RequeueIfPaused();
        return;
    }
    // ここまでに届いた通知の変更はこれから読む内容に含まれる。残すと Resume で同じ保存をもう一度読み直す。
    file_watcher_.DiscardPendingChange();

    // 編集中の差分リロードでスピナーが点滅すると見づらいため、旧コンテンツを表示したまま差し替える。
    // prefix shrink の defer が上限に達したら、縮んだ内容を受け入れるため worker の事前判定を省いてパースさせる。
    const auto& doc = state_.document.doc;
    BeginAsyncLoad(doc.GetFilePath(), /*show_animation=*/false,
                   prefix_shrink_retry_.Exhausted() ? ReloadBase{} : ReloadBase{ doc.ShareData() });
}

// FileWatcher は paused のまま残し、ドラッグの終わり (reducer の ClearDrag) で読み直させる。
bool App::DeferReloadWhileSelecting()
{
    if (state_.view.drag.kind != LeftDrag::TextSelect) {
        return false;
    }
    state_.view.reload_after_drag = true;
    return true;
}

void App::ReloadDocument(Document next, LayoutCache estimated, std::optional<UnchangedEnds> ends, size_t diff_pos)
{
    MENDO_PROFILE("ReloadDocument");

    // 保存のたびに可視範囲を作り直さず、実測の高さも推定値に戻さない (スクロール位置の計算と表示の跳ねを防ぐ)。
    const auto carry = layout::CarryOverUnchanged(state_.document.doc.GetNodes(), state_.document.layout_cache, next.GetNodes(), estimated,
                                                  ends ? *ends : FindUnchangedEnds(state_.document.doc.GetNodes(), next.GetNodes()));
    ReplaceDocument(std::move(next), std::move(estimated));

    // 図と画像はコードやパスがキーなので、描画・デコード待ちも結果もそのまま使える。
    // 編集で画像が置かれた可能性があるので失敗記録は消す。
    resource_manager_.CancelMermaidBatch();
    image_loader_.ResetFailedPaths();

    const PaneRect md = GetPaneLayout().md_rect;

    // Mermaid/LaTeX 図と通常画像の推定高さを実測値 (file_cache_ / image_loader メモリキャッシュ) で
    // 上書きし、CalcScrollYForDiff の Y 計算がずれないようにする。引き継いだノードは実測の高さを持っているので、
    // 当てるのは変わった範囲だけでよい。引き継いだ高さと合わせて Y を 1 回で組み直す。
    const bool applied = ApplyCachedHeights({ carry.ends.head, carry.ends.NewTailBegin() });
    if (applied || carry.carried > 0) {
        RecomputeYPositions(state_.document.doc.GetNodes(), state_.document.layout_cache, theme_service_.GetTheme());
    }

    const float desired_scroll = layout::CalcScrollYForDiff(
        state_.document.doc.GetNodes(), state_.document.layout_cache,
        std::string_view{ state_.document.doc.GetRawText() },
        diff_pos, md.height, state_.view.viewport.GetScrollY());

    MENDO_TRACEF("ReloadDocument: desired_scroll={:.1f} diff_pos={}", desired_scroll, diff_pos);

    // スクロール位置を先に設定してから RelayoutViewport を呼ぶことで、変更箇所周辺の可視ノードが優先的に計測される。
    Dispatch(DocumentReloadedAction{ carry.ends, desired_scroll });

    RelayoutViewport(RelayoutResources::ImagesAndDiagrams);

    MENDO_TRACEF("ReloadDocument: after-relayout scroll_y={:.1f} max_scroll={:.1f}", state_.view.viewport.GetScrollY(), MdMaxScroll());

    Dispatch(ReloadSettledAction{});

    mermaid_renderer_.SetEditingDiagrams(!state_.document.doc.GetDiagramNodeIndices().empty());
    file_watcher_.ResumeWatching();
}

bool App::ApplyCachedHeights(VisibleRange nodes)
{
    const bool mermaid_applied = resource_manager_.ApplyMermaidCacheHeightsForReload(nodes);
    const bool image_applied = resource_manager_.ApplyCachedImagesForReload(nodes);
    return mermaid_applied || image_applied;
}
