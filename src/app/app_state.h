#pragma once
#include "app_events.h"
#include "block_h_scroll.h"
#include "document.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "pane_controller.h"
#include "search_state.h"
#include "search_bar_controller.h"
#include "nav.h"
#include "file_explorer.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_types.h"
#include "pane_layout.h"
#include "theme.h"
#include <string>
#include <memory_resource>
#include <unordered_map>

struct DocumentState {
    Document doc;
    LayoutCache layout_cache;
};

// 進行中の左ボタンドラッグ。マウスキャプチャはこれが None でない間だけ持つ (右ボタンのジェスチャーを除く)。
struct LeftDragState {
    LeftDrag kind = LeftDrag::None;
    // Splitter / PaneScrollbar: 対象のサイドペイン。
    PaneTarget pane = PaneTarget::File;
    // スクロールバー: つまみを掴んだ位置 (つまみ上端から)。
    float grip_offset = 0.0f;
    // BlockHScroll: 対象ノードと開始時の x・横スクロール量。
    int block_node = -1;
    float start_x = 0.0f;
    float start_scroll = 0.0f;
    // SearchInput: 選択の起点 (キャレット位置)。
    int search_anchor = 0;
};

struct ViewState {
    ViewportManager viewport;
    PaneController panes;
    // 次に開き終えた文書で復元するスクロール位置。
    ScrollTarget pending_restore;
    NavHistory nav_history;

    // ブロック単位の横スクロール。キーは LayoutCache のノードインデックス。
    // ファイルロード/リロードでクリア (永続化しない)。スクロールバーはホバー中とドラッグ中の
    // 対象のみ描画するため、map サイズはユーザー操作分に限定される。
    std::pmr::unordered_map<int, float> block_scroll_x;
    HoverHighlights hover;
    int active_toc_index = -1;
    LeftDragState drag;
    // テキスト選択のドラッグ中に届いたリロード。差し替えると選択の途中でドラッグが終わるので、終わるまで待つ。
    bool reload_after_drag = false;

    // 横スクロールをドラッグ中のブロック。ドラッグしていなければ -1。
    constexpr int HDragNode() const noexcept
    {
        return drag.kind == LeftDrag::BlockHScroll ? drag.block_node : -1;
    }

    float GetBlockScrollX(int node_index, const BlockHScrollGeometry& geom) const noexcept
    {
        return LookupBlockScrollX(&block_scroll_x, node_index, geom);
    }

    // ノード/見出し index に結び付く一時状態。文書を差し替えると別の要素を指すのでクリアする。
    // ドラッグはキャプチャの解放を伴うので呼び出し側が EndLeftDrag で終える。
    // ホバーは描画後に引き直される (App::RefreshHoverAtCursor)。
    void ResetPerNodeTransientState()
    {
        viewport.ClearSelection();
        block_scroll_x.clear();
        hover = {};
        active_toc_index = -1;
    }
};

struct InteractionState {
    MouseGesture gesture;
    SwipeDetector swipe_detector;
    // 表示中か表示待ちのツールチップの対象。ウィンドウ (Tooltip) は App が持つ。
    TooltipTarget tooltip;
    // 空なら非表示。フェードは毎フレームの全画面再描画を伴うので、表示と消去の 2 回だけ描き直す。
    std::pmr::wstring toast_message;
    int zoom_wheel_accum = 0;
};

struct SearchGroup {
    SearchState search_state;
    SearchBarController search_bar_ctrl;
};

struct WindowState {
    TitleBar titlebar;
    bool is_sizing = false;
    // 移動だけのサイズ変更ループでは終了時のレイアウト処理を省くため。
    bool size_changed_in_sizing = false;
    bool window_active = true;
    float cached_dpi_scale = 1.0f;
    // 直近の WM_SIZE のクライアント領域 (物理 px)。ペイン配置の元になり、同じ大きさの WM_SIZE を無視するのにも使う。
    uint32_t client_width = 0;
    uint32_t client_height = 0;
    // DPI が変わった。次の WM_SIZE は同じピクセルサイズでも処理する。
    bool dpi_changed = false;
};

struct AppState {
    DocumentState document;
    ViewState view;
    InteractionState interaction;
    SearchGroup search;
    WindowState window;

    // Renderer が所有する Theme への非所有参照。App::Init で設定され、
    // Theme 変更時 (renderer_.SetTheme 経由) は内部値が in-place 更新されるためポインタは
    // 安定。reducer 系が Y 位置計算に必要な spacing_above 等を参照するために使う。
    const Theme* theme = nullptr;

    FileExplorer file_explorer;

    std::pmr::wstring cached_title_text = L"mendo";
    PaneLayoutCache pane_layout_cache;
};

inline ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept
{
    return state.view.viewport.CurrentTarget(state.document.layout_cache);
}
// load_path: これから開くファイル。現在と別のファイルなら、ロード失敗 (LoadFailedAction) で取り消す仮の履歴にする。
void PushCurrentNavEntry(AppState& state, std::wstring_view load_path = {});

// reducer と App (Win32 層) の双方が AppState から導出するペイン/ブロック情報。
// 非 inline 関数の定義は reducer.cpp。

struct SidePaneContext {
    PaneScrollInfo info;
    ScrollState& scroll;
};
SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane);

inline size_t SidePaneItemCount(const AppState& state, PaneTarget pane)
{
    return pane == PaneTarget::File ? state.file_explorer.GetEntries().size() : state.document.doc.GetToc().size();
}

// サイドペイン (rect) で dip_y にある項目の番号。無ければ -1。
inline int SidePaneItemAt(const AppState& state, PaneTarget pane, const PaneRect& rect, float dip_y)
{
    const float item_h = state.theme->pane_item_height;
    const float local_y = SidePaneLocalY(dip_y, PaneContentTop(rect, state.theme->pane_header_height), state.view.panes.SidePaneScroll(pane).scroll_y);
    return HitTestUniformList(local_y, item_h, SidePaneItemCount(state, pane));
}

// サイドペインヘッダーのボタン構成。Refresh/Reveal はファイルペインのみ。
struct SidePaneHeaderButtons {
    bool has_file_buttons = false;
    // 無効時は描画のみ行い、ヒットしない。
    bool reveal_enabled = false;
};

inline SidePaneHeaderButtons SidePaneHeaderButtonsFor(const AppState& state, PaneTarget target) noexcept
{
    const bool is_file = target == PaneTarget::File;
    return { .has_file_buttons = is_file, .reveal_enabled = is_file && state.document.doc.HasBackingFile() };
}

// 無効なら window の寸法・ペイン・テーマから組み直す。Reduce は入口で必ず呼ぶ。
const PaneLayout& EnsurePaneLayout(AppState& state) noexcept;
BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept;
float MdScrollableContentHeight(const AppState& state) noexcept;
// pane_layout_cache が確定していること。
float MdMaxScroll(const AppState& state) noexcept;
