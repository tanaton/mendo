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

void NavigateHistory(AppState& state, SideEffectList& effects, bool forward)
{
    auto& history = state.view.nav_history;
    const NavEntry current = CurrentNavEntry(state);
    NavEntry out;
    const bool moved = forward ? history.GoForward(current, out) : history.GoBack(current, out);
    if (moved) {
        ApplyNavResult(state, effects, std::move(out));
    }
}

// 現在位置を履歴に積んでから別ファイルを開く。
void OpenFileWithHistory(AppState& state, SideEffectList& effects, const std::pmr::wstring& path)
{
    PushCurrentNavEntry(state);
    PushEffect(effects, effect::LoadFile{ path });
}

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

void ReduceNavigateBack(AppState& state, SideEffectList& effects)
{
    NavigateHistory(state, effects, /*forward=*/false);
}

void ReduceNavigateForward(AppState& state, SideEffectList& effects)
{
    NavigateHistory(state, effects, /*forward=*/true);
}

void ReduceFilePaneDirectoryClicked(AppState& state, SideEffectList& effects, const FilePaneDirectoryClickedAction& a)
{
    state.file_explorer.SetDirectory(a.full_path);
    state.view.panes.SidePaneScroll(PaneTarget::File) = {};
    EmitSidePaneScrollChanged(effects, PaneTarget::File);
}

void ReduceFilePaneFileClicked(AppState& state, SideEffectList& effects, const FilePaneFileClickedAction& a)
{
    OpenFileWithHistory(state, effects, a.full_path);
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

void ReduceRestoreScrollAfterLoad(AppState& state, const RestoreScrollAfterLoadAction& a)
{
    auto& viewport = state.view.viewport;
    auto& restore = state.view.scroll_restore;
    if (a.reload_diff_scroll_y) {
        viewport.SetScrollY(*a.reload_diff_scroll_y);
    }
    else if (restore.HasNodeRestore()) {
        viewport.SetScrollTarget(restore.pending_restore_node, static_cast<float>(restore.pending_restore_offset));
        viewport.ApplyScrollTarget(state.document.layout_cache);
        restore.ClearNodeRestore();
    }
    else {
        viewport.SetScrollY(0.0f);
    }
}

// 復元情報は失敗したロード先のノード番号なので、残すと次に開く別ファイル
// (起動時の失敗ならフォールバックのヘルプ) が無関係な位置へスクロールする。
void ReduceLoadFailed(AppState& state)
{
    state.view.scroll_restore.ClearNodeRestore();
}

void ReduceDropFiles(AppState& state, SideEffectList& effects, const DropFilesAction& a)
{
    OpenFileWithHistory(state, effects, a.path);
}

void ReduceShowHelp(AppState& state, SideEffectList& effects)
{
    if (!IsHelpPath(state.document.doc.GetFilePath())) {
        PushCurrentNavEntry(state);
    }
    PushEffect(effects, effect::LoadFile{ std::pmr::wstring(HELP_PATH) });
}
