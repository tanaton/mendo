#include "reducer_internal.h"
#include "document_utils.h"
#include "file_io.h"
#include <cmath>

namespace {

void ApplyNavResult(AppState& state, SideEffectList& effects, NavEntry&& entry)
{
    if (!entry.file_path.empty() && !path_util::iequal(entry.file_path, state.document.doc.GetFilePath())) {
        state.view.scroll_restore.SetNodeRestore(entry.node, static_cast<int>(std::lround(entry.offset)));
        PushEffect(effects, effect::LoadFile{ std::move(entry.file_path) });
    }
    else {
        ApplyScrollTargetAndEmit(state, effects, entry.node, entry.offset, /*toc_auto_scroll=*/true);
    }
}

} // namespace

void ReduceNavigateBack(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.view.nav_history.GoBack(CurrentNavEntry(state), out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

void ReduceNavigateForward(AppState& state, SideEffectList& effects)
{
    NavEntry out;
    if (state.view.nav_history.GoForward(CurrentNavEntry(state), out)) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

void ReduceFilePaneDirectoryClicked(AppState& state, SideEffectList& effects, const FilePaneDirectoryClickedAction& a)
{
    state.file_explorer.SetDirectory(a.full_path);
    if (!state.document.doc.GetFilePath().empty()) {
        state.file_explorer.SetCurrentFile(state.document.doc.GetFilePath());
    }
    state.view.panes.SidePaneScroll(PaneTarget::File) = {};
    PushEffect(effects, effect::InvalidatePaneCache{ PaneZone::FilePane });
    PushEffect(effects, effect::InvalidateWindow{});
}

void ReduceFilePaneFileClicked(AppState& state, SideEffectList& effects, const FilePaneFileClickedAction& a)
{
    PushCurrentNavEntry(state);
    PushEffect(effects, effect::LoadFile{ a.full_path });
}

namespace {
void ScrollToResolvedAnchor(AppState& state, SideEffectList& effects, int idx, bool toc_auto_scroll)
{
    if (idx < 0) {
        return;
    }
    const auto target = MakeHeadingTopTarget(
        idx,
        state.theme->heading_spacing_above,
        state.pane_layout_cache.Get().md_rect.y);
    ApplyScrollTargetAndEmit(state, effects, target.node, target.offset, toc_auto_scroll);
}

void ScrollToAnchor(AppState& state, SideEffectList& effects, std::string_view anchor_id)
{
    ScrollToResolvedAnchor(state, effects, state.document.doc.FindAnchorIndex(anchor_id), /*toc_auto_scroll=*/true);
}

// anchor_id() 由来など、既に正規化済み入力向け（ToLowerAsciiCopy の確保を回避する）。
void ScrollToNormalizedAnchor(AppState& state, SideEffectList& effects, std::string_view anchor_id, bool toc_auto_scroll)
{
    ScrollToResolvedAnchor(state, effects, state.document.doc.FindNormalizedAnchorIndex(anchor_id), toc_auto_scroll);
}
} // namespace

void ReduceTocItemClicked(AppState& state, SideEffectList& effects, const TocItemClickedAction& a)
{
    PushCurrentNavEntry(state);
    // TOC は anchor_id() を直接渡すため、改めての正規化は不要。
    // 目次クリック由来では目次ペインの自動スクロールを抑制する (issue#259)。
    ScrollToNormalizedAnchor(state, effects, a.anchor_id, /*toc_auto_scroll=*/false);
}

void ReduceNavigateAnchor(AppState& state, SideEffectList& effects, const NavigateAnchorAction& a)
{
    // nav 履歴の push は呼び出し側 (App::HandleLinkClick) で行われる。
    ScrollToAnchor(state, effects, a.anchor_id);
}

void ReduceRestoreScrollAfterLoad(AppState& state, SideEffectList& /*effects*/, const RestoreScrollAfterLoadAction& a)
{
    // ノードインデックスはセッション内の識別子で、再パース後は別ノードを指しうる。
    state.view.ResetPerNodeTransientState();

    state.view.viewport.ClearScrollTarget();
    if (a.has_reload_diff) {
        state.view.viewport.SetScrollY(a.reload_diff_scroll_y);
        state.reload_diff_pos = std::string_view::npos;
    }
    else if (state.view.scroll_restore.HasNodeRestore()) {
        state.view.viewport.SetScrollTarget(
            state.view.scroll_restore.pending_restore_node,
            static_cast<float>(state.view.scroll_restore.pending_restore_offset));
        state.view.viewport.ApplyScrollTarget(state.document.layout_cache);
        state.view.scroll_restore.ClearNodeRestore();
    }
    else {
        state.view.viewport.SetScrollY(0.0f);
    }
}

void ReduceDropFiles(AppState& state, SideEffectList& effects, const DropFilesAction& a)
{
    if (!state.document.doc.GetFilePath().empty()) {
        PushCurrentNavEntry(state);
    }
    PushEffect(effects, effect::LoadFile{ a.path });
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    if (!state.document.doc.GetFilePath().empty() && !IsHelpPath(state.document.doc.GetFilePath())) {
        PushCurrentNavEntry(state);
    }
    PushEffect(effects, effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
}
