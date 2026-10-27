#pragma once
#include "app_events.h"
#include "block_h_scroll.h"
#include "document.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "search_state.h"
#include "nav.h"
#include "file_explorer.h"
#include "mouse_gesture.h"
#include "swipe_detector.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_types.h"
#include "pane_layout.h"
#include "render_params.h"
#include "theme.h"
#include <string>
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
    // TextSelect: 選択の起点と、押した位置 (px。小さいクリックの判定に使う)。
    int select_node = -1;
    uint32_t select_pos = 0;
    int click_x = 0;
    int click_y = 0;

    constexpr TextSelection SelectionTo(int node, uint32_t pos) const noexcept
    {
        return TextSelection::MakeOrdered(select_node, select_pos, node, pos);
    }
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
    std::unordered_map<int, float> block_scroll_x;
    HoverHighlights hover;
    // SyncActiveTocIndex が描画のたびに決め直す。
    int active_toc_index = -1;
    // 目次クリック直後の同期 1 回だけ、目次ペインを追従スクロールさせない (issue#259)。
    // クリックした項目がペイン内で動いて見失うのを防ぐ。
    bool keep_toc_pane_once = false;
    LeftDragState drag;
    // テキスト選択のドラッグ中に届いたリロード。差し替えると選択の途中でドラッグが終わるので、終わるまで待つ。
    bool reload_after_drag = false;

    // 横スクロールをドラッグ中のブロック。ドラッグしていなければ -1。
    constexpr int HDragNode() const noexcept
    {
        return drag.kind == LeftDrag::BlockHScroll ? drag.block_node : -1;
    }

    float GetBlockScrollX(const BlockHScrollGeometry& geom) const noexcept
    {
        return LookupBlockScrollX(&block_scroll_x, geom);
    }
};

struct InteractionState {
    MouseGesture gesture;
    SwipeDetector swipe_detector;
    // 表示中か表示待ちのツールチップの対象。文言とウィンドウ (Tooltip) は App が持つ。
    TooltipTarget tooltip;
    // Ctrl+ホイールの delta の端数。ピンチや高分解能ホイールは WHEEL_DELTA 未満を連発するため、
    // 1 ノッチ分たまるごとに 1 段だけズームする。
    int zoom_wheel_accum = 0;
};

inline GestureRenderState ResolveGestureOverlay(const MouseGesture& gesture, const SwipeDetector& swipe) noexcept
{
    GestureRenderState gs;
    gs.trail = gesture.IsGestureActive() ? &gesture.GetTrailPoints() : nullptr;
    // マウスジェスチャーを優先する。None ならオーバーレイは出さない。
    gs.direction = gesture.IsOverlayVisible() ? gesture.GetDirection() : swipe.GetOverlayDirection();
    return gs;
}

// 検索バー。検索そのものは SearchState、入力欄の表示に要るもの (キャレット・選択・IME 変換中の文字列) はここに持つ。
// 入力は非表示の EDIT (Win32Window) で受け、変わるたびにアクションで写してくる。
struct SearchGroup {
    SearchState search_state;
    // EDIT の選択 [selection_start, caret_pos)。-1 はテキスト末尾。
    int caret_pos = -1;
    int selection_start = -1;
    std::wstring ime_composition;
    // 入力欄に表示している検索クエリ。SearchState::GetQuery() の UTF-16 版。
    std::wstring query_wide;
    // 区切った検索が終わったら、見つけたマッチへスクロールする。
    bool scroll_after_search = false;
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

    constexpr float WidthDip() const noexcept
    {
        return PixelToDip(static_cast<float>(client_width), cached_dpi_scale);
    }
    constexpr float HeightDip() const noexcept
    {
        return PixelToDip(static_cast<float>(client_height), cached_dpi_scale);
    }
};

struct AppState {
    DocumentState document;
    ViewState view;
    InteractionState interaction;
    SearchGroup search;
    WindowState window;

    // ThemeService が所有する Theme への非所有参照。App::Init で設定され、
    // ダークモードとズームの切り替えは内部値を in-place で書き換えるためポインタは
    // 安定。reducer 系が Y 位置計算に必要な spacing_above 等を参照するために使う。
    const Theme* theme = nullptr;

    FileExplorer file_explorer;

    PaneLayoutCache pane_layout_cache;
};

inline ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept
{
    return state.view.viewport.CurrentTarget(state.document.layout_cache);
}

// reducer と App (Win32 層) の双方が AppState から導出するペイン/ブロック情報。
// 非 inline 関数の定義は reducer.cpp。

struct SidePaneContext {
    PaneScrollInfo info;
    float item_height;
    ScrollState& scroll;
};
SidePaneContext GetSidePaneContext(AppState& state, PaneTarget pane);
// 目次のアクティブ見出しを今の scroll_y と見出しの Y から決め直し、変わったら目次ペインを追従させる。
// 描画のたびに呼ぶので、スクロール・計測・画像や図の高さの確定など Y が動くどの経路でも同期が漏れない。
void SyncActiveTocIndex(AppState& state);

inline size_t SidePaneItemCount(const AppState& state, PaneTarget pane)
{
    return pane == PaneTarget::File ? state.file_explorer.GetEntries().size() : state.document.doc.GetToc().size();
}

// 一覧の 1 項目の高さ。ペインごとの倍率を掛ける。
inline float SidePaneItemHeight(const AppState& state, PaneTarget pane) noexcept
{
    return state.theme->pane_item_height * state.view.panes.SidePaneZoom(pane).Value();
}

inline PaneScrollInfo SidePaneScrollInfo(const AppState& state, PaneTarget pane, const PaneLayout& layout)
{
    return ComputeScrollInfo(layout.Get(pane), state.theme->pane_header_height, SidePaneContentHeight(SidePaneItemCount(state, pane), SidePaneItemHeight(state, pane)));
}

// サイドペイン (rect) で dip_y にある項目の番号。無ければ -1。
inline int SidePaneItemAt(const AppState& state, PaneTarget pane, const PaneRect& rect, float dip_y)
{
    const float item_h = SidePaneItemHeight(state, pane);
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
inline PaneScrollInfo MdScrollInfo(const AppState& state, const PaneLayout& layout) noexcept
{
    return ComputeScrollInfo(layout.md_rect, 0.0f, MdScrollableContentHeight(state));
}
inline SearchBarLayout SearchBarLayoutFor(const AppState& state, const PaneRect& md_rect) noexcept
{
    return ComputeSearchBarLayout(md_rect, !state.search.search_state.GetQuery().empty());
}
// pane_layout_cache が確定していること。
float MdMaxScroll(const AppState& state) noexcept;
