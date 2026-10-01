#include "reducer_internal.h"
#include "document_utils.h"
#include "file_io.h"
#include <algorithm>
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
    state.view.panes.SidePaneScroll(PaneTarget::File) = {};
    EmitSidePaneScrollChanged(effects, PaneTarget::File);
}

void ReduceFilePaneFileClicked(AppState& state, SideEffectList& effects, const FilePaneFileClickedAction& a)
{
    PushCurrentNavEntry(state);
    PushEffect(effects, effect::LoadFile{ a.full_path });
}

void ReduceFilePaneRevealCurrentFile(AppState& state, SideEffectList& effects)
{
    // ヘルプ表示中は移動先のフォルダがない。
    if (!state.document.doc.HasBackingFile()) {
        return;
    }
    state.file_explorer.SetDirectory(state.document.doc.GetDirectory());

    const auto& entries = state.file_explorer.GetEntries();
    const auto it = std::ranges::find_if(entries, &FileEntry::is_current);
    const auto ctx = GetSidePaneContext(state, PaneTarget::File);
    ctx.scroll.scroll_y = it != entries.end()
                              ? CenterPaneScrollOnItem(static_cast<size_t>(std::distance(entries.begin(), it)), state.theme->pane_item_height, ctx.info)
                              : 0.0f;
    EmitSidePaneScrollChanged(effects, PaneTarget::File);
}

namespace {
void ScrollToHeading(AppState& state, SideEffectList& effects, int node_index, bool toc_auto_scroll)
{
    if (node_index < 0) {
        return;
    }
    const auto target = MakeHeadingTopTarget(
        node_index,
        state.theme->heading_spacing_above,
        state.pane_layout_cache.Get().md_rect.y);
    ApplyScrollTargetAndEmit(state, effects, target.node, target.offset, toc_auto_scroll);
}
} // namespace

void ReduceTocItemClicked(AppState& state, SideEffectList& effects, const TocItemClickedAction& a)
{
    PushCurrentNavEntry(state);
    // 目次クリック由来では目次ペインの自動スクロールを抑制する (issue#259)。
    ScrollToHeading(state, effects, a.node_index, /*toc_auto_scroll=*/false);
}

void ReduceNavigateAnchor(AppState& state, SideEffectList& effects, const NavigateAnchorAction& a)
{
    PushCurrentNavEntry(state);
    ScrollToHeading(state, effects, state.document.doc.FindAnchorIndex(a.anchor_id), /*toc_auto_scroll=*/true);
}

void ReduceRestoreScrollAfterLoad(AppState& state, SideEffectList& /*effects*/, const RestoreScrollAfterLoadAction& a)
{
    if (a.reload_diff_scroll_y) {
        state.view.viewport.SetScrollY(*a.reload_diff_scroll_y);
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
    PushCurrentNavEntry(state);
    PushEffect(effects, effect::LoadFile{ a.path });
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    if (!IsHelpPath(state.document.doc.GetFilePath())) {
        PushCurrentNavEntry(state);
    }
    PushEffect(effects, effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
}
