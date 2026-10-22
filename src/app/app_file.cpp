#include "app.h"
#include "app_constants.h"
#include "file_loader.h"
#include "file_io.h"
#include "i18n.h"
#include "document_utils.h"
#include "layout.h"
#include "layout_computer.h"
#include "profiler.h"
#include "reducer.h"
#include "win32_util.h"
#include "reload_flow.h"
#include <utility>

using layout::RecomputeYPositions;

// 文書の差し替えで対象を失うドラッグを、キャプチャごと終える。
void App::EndDocumentBoundDrag()
{
    if (!IsDocumentBoundDrag(state_.view.drag.kind)) {
        return;
    }
    SideEffectList effects;
    EndLeftDrag(state_, effects);
    Execute(effects);
}

void App::CancelPendingResources()
{
    resource_manager_.CancelMermaidBatch();
    mermaid_renderer_.DropQueued();
    image_loader_.DropQueued();
}

void App::ResetViewForNewDocument()
{
    // 同一文書の再パースと違い、サイドペインのスクロールも先頭へ戻す。
    state_.view.panes.ResetScrollStates();
    // 旧文書のマッチ位置が新 nodes に対して誤用されるのを防ぐ。先に空にしておくので、
    // 入力欄を空にしたときの EN_CHANGE は OnTextChanged で何もせず終わる。
    state_.search.search_bar_ctrl.Reset();
    Execute(effect::SearchUnfocus{ /*clear_text=*/true });
    CancelPendingResources();
}

void App::StartLoadingAnimation()
{
    file_load_service_.BeginLoadingAnimation();
    SetTimer(app_timer::Id::LOADING_ANIM, app_timer::FRAME_INTERVAL_MS);
    Invalidate();
}

void App::StopLoadingAnimation()
{
    // スピナーを消すための再描画。後続が描き直さない経路 (同じファイルを開き直して NoChange) もある。
    if (file_load_service_.IsLoading()) {
        Invalidate();
    }
    KillTimer(app_timer::Id::LOADING_ANIM);
    file_load_service_.StopLoading();
}

void App::LoadHelpDocument()
{
    if (IsHelpPath(state_.document.doc.GetFilePath())) {
        // ヘルプに留まる操作なので、ロード中の別ファイルへ後から切り替わらないようにする。
        CancelOtherFileLoad();
        return;
    }

    const auto rc = LoadRcData(i18n::S().help_resource_id);
    if (rc.empty()) {
        return;
    }

    StopLoadingAnimation();
    file_load_service_.CancelAsyncLoad();
    file_watcher_.StopWatching();

    std::pmr::string utf8(reinterpret_cast<const char*>(rc.data()), rc.size());
    ReplaceDocument(Document::FromMarkdown(std::move(utf8), HELP_PATH));

    FinishLoadMarkdownFile(/*follow_file_pane=*/false);
}

namespace {
// 100MB 級の文書はテキスト・runs・リンク URL を arena ごと捨てても、数十万ノードの走査と
// 表等の解放で UI を十数 ms 止めるため worker で破棄する。
constexpr size_t kDisposeMinNodes = 4096;
// IDWriteTextLayout の解放は 1 個 2〜3µs かかる。
constexpr size_t kDisposeMinLayouts = 256;

// 渡すものは D2D の参照を持たないこと (確保元の既定 pmr リソースはスレッド安全)。
template <class T>
void DisposeOnWorkerIfLarge(TaskScheduler& scheduler, size_t count, size_t min_count, T&& obj)
{
    if (count >= min_count) {
        scheduler.Post([o = std::forward<T>(obj)] {});
    }
}
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
    Document old = std::exchange(state_.document.doc, std::move(next));
    LayoutCache old_cache = std::exchange(state_.document.layout_cache, std::move(estimated));
    renderer_.ForgetDocumentText();
    old_cache.ReleaseDiagrams();
    // 表は 1 ノードにセルの数だけレイアウトを持つので、ノード数によらずワーカーで捨てる。
    scheduler_.Post([o = std::move(old_cache)] {});
    DisposeOnWorkerIfLarge(scheduler_, old.GetNodes().size(), kDisposeMinNodes, std::move(old));
    // ロードとリロードの両方がここを通る。番号に結び付いた状態を旧文書から持ち越さない。
    state_.view.ResetPerNodeTransientState();
    // 待たせていたリロードは旧文書のもの。
    state_.view.reload_after_drag = false;
    EndDocumentBoundDrag();
    Dispatch(ClearTooltipAction{});
}

void App::ReloadDocument(Document next, LayoutCache estimated)
{
    // 保存のたびに可視範囲を作り直さず、実測の高さも推定値に戻さない (スクロール位置の計算と表示の跳ねを防ぐ)。
    const auto carry = layout::CarryOverUnchanged(state_.document.doc.GetNodes(), state_.document.layout_cache, next.GetNodes(), estimated);
    if (carry.carried > 0) {
        RecomputeYPositions(next.GetNodes(), estimated, renderer_.GetTheme());
    }
    // FinishReload の再検索が終わるまで、変わらなかったノードのハイライトと件数を残す。
    state_.search.search_state.KeepMatchesOfUnchangedNodes(carry.ends);
    ReplaceDocument(std::move(next), std::move(estimated));
}

void App::ReplaceDocument(Document next)
{
    LayoutCache estimated = layout::MakeEstimatedLayoutCache(next.GetNodes(), renderer_.GetTheme(), MdContentWidth());
    ReplaceDocument(std::move(next), std::move(estimated));
}

void App::BeginAsyncLoad(std::pmr::wstring path, bool show_animation, std::shared_ptr<const std::pmr::string> reload_base)
{
    MENDO_PROFILE("App::BeginAsyncLoad");
    file_load_service_.SetLoadingPath(std::move(path));
    if (show_animation) {
        StartLoadingAnimation();
        UpdateWindow(hwnd_);
    }
    file_load_service_.StartAsyncLoad(scheduler_, hwnd_, app_msg::PARSE_COMPLETE, renderer_.GetTheme(), MdContentWidth(), std::move(reload_base));
}

// Defer* はエディタの保存途中 (truncate→rewrite の前半や書き込み中) を読んだ状態。
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
    SetTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE, app_timer::FILE_RELOAD_RETRY_MS);
}

// reducer を経由せず App 層で直接実行する。ファイル I/O + 同期/非同期ロード分岐 +
// パース + レイアウト初期化を含む大きな命令的ワークフローで、reducer 化すると
// effect variant と App::Execute が肥大化するため意図的に service 経路として分離している。
void App::LoadMarkdownFile(std::wstring_view path)
{
    MENDO_PROFILE("App::LoadMarkdownFile");
    KillTimer(app_timer::Id::FILE_RELOAD_DEBOUNCE);
    // 止めたリロードや取り消される進行中のリロードは、このロードが取り消し・失敗で終わったときに読み直す。
    file_watcher_.RequeueIfPaused();
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
        // 共有 scheduler は FIFO なので、旧文書の画像デコード待ちが残っているとパースがその後ろに並ぶ。
        // 読み込みに失敗して旧文書に戻ったら HandleLoadFailureFallback が要求し直す。
        CancelPendingResources();
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
    file_watcher_.ResumeWatching();
}

// Init (RestoreTheme) 前に呼ばれるため、推定用のテーマは設定から直接作る。
// 推定に使う寸法はライト/ダーク共通でズームだけに依存するので、ダークモード未読込でも結果は同じ。
void App::StartPreloadAsync(std::pmr::wstring path)
{
    InitSharedScheduler();
    file_load_service_.StartPreloadAsync(scheduler_, std::move(path), theme_service_.CreateTheme(theme_service_.LoadZoomIndex()));
}

void App::DoLoadMarkdownFile()
{
    MENDO_PROFILE("DoLoadMarkdownFile");

    StopLoadingAnimation();

    const auto& path = file_load_service_.GetLoadingPath();
    const bool follow_file_pane = FilePaneFollowsLoad(state_.document.doc.GetFilePath(), path);
    {
        MENDO_PROFILE("LoadFile(FileIO+Parse)");
        auto load_result = DocumentService::LoadFile(path);
        if (!load_result) {
            HandleLoadFailureFallback(load_result.error());
            return;
        }
        ReplaceDocument(std::move(*load_result));
    }

    FinishLoadMarkdownFile(follow_file_pane);
}

void App::HandleLoadFailureFallback(std::optional<FileLoadError> error)
{
    if (error) {
        ShowToast(FileLoadErrorMessage(*error, i18n::S()));
    }
    file_watcher_.ResumeWatching();
    Dispatch(LoadFailedAction{});
    if (state_.document.doc.IsEmpty()) {
        LoadHelpDocument();
    }
    else {
        // 読み込み開始時に捨てた画像・図の待ちを、表示を続ける旧文書のぶんだけ要求し直す。
        resource_manager_.ScheduleBitmapManage();
        Invalidate();
    }
}

void App::OnParseComplete()
{
    MENDO_PROFILE("OnParseComplete");

    auto outcome = file_load_service_.TakeAsync();
    const auto plan = PlanParseComplete(
        outcome, file_load_service_.IsAsyncLoading(), state_.document.doc,
        [](const std::pmr::wstring& path, size_t read_size) static { return IsFileLargerThan(path.c_str(), read_size); });

    if (plan.step == ParseCompleteStep::IgnoreStale) {
        MENDO_TRACE("OnParseComplete: stale message ignored (new load in progress)");
        return;
    }

    StopLoadingAnimation();

    // ApplyReload / ReplaceWithResult のときだけ結果がある。
    AsyncLoadResult* const result = (outcome && *outcome) ? &**outcome : nullptr;
    // worker が作った推定済み LayoutCache は D2D 由来の参照を持たないので、文書ごと worker で捨てられる。
    const auto dispose_outcome = [&] {
        DisposeOnWorkerIfLarge(scheduler_, result->doc.GetNodes().size(), kDisposeMinNodes, std::move(outcome));
    };
    if (plan.reload) {
        MENDO_TRACEF(
            "OnParseComplete: reload worker_diff={} node_count={} diff_pos={} new_size={} op={}",
            result->reload.has_value(), result->doc.GetNodes().size(), plan.reload->diff_pos,
            result->doc.GetRawText().size(), std::to_underlying(plan.reload->op));
    }

    switch (plan.step) {
    case ParseCompleteStep::IgnoreStale:
        return;
    case ParseCompleteStep::Fail:
        MENDO_TRACE("OnParseComplete: no result (cancelled or load failed)");
        HandleLoadFailureFallback(plan.error);
        return;
    case ParseCompleteStep::RetryDocumentChanged:
    case ParseCompleteStep::RetryPartialWrite:
        if (result) {
            dispose_outcome();
        }
        DeferReloadRetry();
        return;
    case ParseCompleteStep::ApplyReload:
        if (ApplyReloadDecisionEarly(*plan.reload, !result->reload.has_value()) == ReloadFlow::Handled) {
            return;
        }
        // 読み始めた後にテキスト選択のドラッグが始まった。読み直しはドラッグの終わりに回す。
        if (DeferReloadWhileSelecting()) {
            dispose_outcome();
            return;
        }
        ReloadDocument(std::move(result->doc), std::move(result->cache));
        FinishReload(plan.reload->diff_pos);
        return;
    case ParseCompleteStep::ReplaceWithResult:
        ReplaceDocument(std::move(result->doc), std::move(result->cache));
        FinishLoadMarkdownFile(plan.follow_file_pane);
        return;
    }
    std::unreachable();
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

    // スクロール復元先の Y がずれないよう、推定高さを Mermaid/画像キャッシュの実測値で補正する。
    if (state_.view.pending_restore.IsValid()) {
        MENDO_TRACEF("FinishLoad: node={} offset={}", state_.view.pending_restore.node, state_.view.pending_restore.offset);
        ApplyCachedHeightsAndRecompute();
    }
    Dispatch(RestoreScrollAfterLoadAction{});

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
    // FileWatcher のバーストで重複スケジュールされないよう、進行中のロードが
    // あれば即 return する。show_animation=false 経路では IsLoading() が立たない
    // ため IsAsyncLoading() も併せて見る。
    if (file_load_service_.IsLoading() || file_load_service_.IsAsyncLoading()) {
        // 別ファイルのロード中なら、それが取り消し・失敗で終わったときの Resume で読み直す。
        file_watcher_.RequeueIfPaused();
        return;
    }
    // ここまでに届いた通知の変更はこれから読む内容に含まれる。残すと Resume で同じ保存をもう一度読み直す。
    file_watcher_.DiscardPendingChange();

    const auto& path = state_.document.doc.GetFilePath();
    if (DocumentService::IsAsyncLoadCandidate(path)) {
        MENDO_TRACE("ReloadCurrentFile: async path");
        // 編集中の差分リロードでスピナーが点滅すると見づらいため、旧コンテンツを表示したまま差し替える。
        // prefix shrink の defer が上限に達したら、縮んだ内容を受け入れるため worker の事前判定を省いてパースさせる。
        BeginAsyncLoad(
            path, /*show_animation=*/false,
            prefix_shrink_retry_.Exhausted() ? nullptr : state_.document.doc.GetRawText().Share());
        return;
    }
    MENDO_TRACE("ReloadCurrentFile: sync path (DoReloadCurrentFile)");
    DoReloadCurrentFile();
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

void App::DoReloadCurrentFile()
{
    MENDO_PROFILE("DoReloadCurrentFile");

    auto load_result = FileLoader::LoadFile(state_.document.doc.GetFilePath());
    if (!load_result) {
        file_watcher_.ResumeWatching();
        return;
    }

    const size_t byte_size = load_result->byte_size;
    std::pmr::string new_text = std::move(load_result->text);

    const std::string_view old_view(state_.document.doc.GetRawText());
    const auto decision = DecideReload(IsFileLargerThan(state_.document.doc.GetFilePath().c_str(), byte_size), old_view, new_text);

    MENDO_TRACEF(
        "DoReload: diff_pos={} old_size={} new_size={} op={}",
        decision.diff_pos, old_view.size(), new_text.size(),
        std::to_underlying(decision.op));

    if (ApplyReloadDecisionEarly(decision, /*has_parsed_doc=*/true) == ReloadFlow::Handled) {
        return;
    }
    Document next = Document::FromMarkdown(std::move(new_text), byte_size, state_.document.doc.GetFilePath());
    LayoutCache estimated = layout::MakeEstimatedLayoutCache(next.GetNodes(), renderer_.GetTheme(), MdContentWidth());
    ReloadDocument(std::move(next), std::move(estimated));
    FinishReload(decision.diff_pos);
}

// DoReloadCurrentFile / OnParseComplete 共通のリロード後処理。
// ドキュメントは更新済みの状態で呼ばれる。
void App::FinishReload(size_t diff_pos)
{
    MENDO_PROFILE("FinishReload");

    // 図と画像はコードやパスがキーなので、描画・デコード待ちも結果もそのまま使える。
    // 編集で画像が置かれた可能性があるので失敗記録は消す。
    resource_manager_.CancelMermaidBatch();
    image_loader_.ResetFailedPaths();

    const PaneRect md = GetPaneLayout().md_rect;

    // Mermaid/LaTeX 図と通常画像の推定高さを実測値 (file_cache_ / image_loader メモリキャッシュ) で
    // 上書きし、CalcScrollYForDiff の Y 計算がずれないようにする。
    ApplyCachedHeightsAndRecompute();

    const float desired_scroll = CalcScrollYForDiff(
        state_.document.doc.GetNodes(), state_.document.layout_cache,
        std::string_view{ state_.document.doc.GetRawText() },
        diff_pos, md.height, state_.view.viewport.GetScrollY());

    MENDO_TRACEF("FinishReload: desired_scroll={:.1f} diff_pos={}", desired_scroll, diff_pos);

    // スクロール位置を先に設定してから RelayoutViewport を呼ぶことで、変更箇所周辺の
    // 可視ノードが優先的に計測される。リロード時は直前の navigation target を破棄し、
    // ピクセル位置で固定する。
    state_.view.viewport.SetScrollY(desired_scroll);

    RelayoutViewport(RelayoutResources::ImagesAndDiagrams);

    MENDO_TRACEF("FinishReload: after-relayout scroll_y={:.1f} max_scroll={:.1f}", state_.view.viewport.GetScrollY(), MdMaxScroll());

    if (state_.search.search_state.IsVisible() && !state_.search.search_state.GetQuery().empty()) {
        SideEffectList effects;
        state_.search.search_bar_ctrl.RunSearchAndLocate(effects, state_.document.doc.GetNodes());
        Execute(effects);
    }

    mermaid_renderer_.SetEditingDiagrams(!state_.document.doc.GetDiagramNodeIndices().empty());
    file_watcher_.ResumeWatching();
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
