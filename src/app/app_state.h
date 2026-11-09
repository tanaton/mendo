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
#include "titlebar.h"
#include "tooltip.h"
#include "ui_types.h"
#include "pane_layout.h"
#include "render_params.h"
#include "theme.h"
#include <string>
#include <unordered_map>
#include <variant>

struct DocumentState {
    Document doc;
    LayoutCache layout_cache;
};

// 進行中の左ボタンドラッグ。種類ごとに、そのドラッグで使う値だけを持つ。
namespace left_drag {

struct None {};
// 選択の起点と、押した位置 (px。小さいクリックの判定に使う)。
struct TextSelect {
    int node = -1;
    uint32_t pos = 0;
    int click_x = 0;
    int click_y = 0;

    constexpr TextSelection SelectionTo(int to_node, uint32_t to_pos) const noexcept
    {
        return TextSelection::MakeOrdered(node, pos, to_node, to_pos);
    }
};
// pane の右端。
struct Splitter {
    PaneTarget pane = PaneTarget::File;
};
// grip_offset はつまみを掴んだ位置 (つまみ上端から)。
struct PaneScrollbar {
    PaneTarget pane = PaneTarget::File;
    float grip_offset = 0.0f;
};
struct MdScrollbar {
    float grip_offset = 0.0f;
};
// 対象ノードと、開始時の x・横スクロール量。
struct BlockHScroll {
    int node = -1;
    float start_x = 0.0f;
    float start_scroll = 0.0f;
};
// 入力欄の選択の起点 (キャレット位置)。
struct SearchInput {
    int anchor = 0;
};

} // namespace left_drag

using LeftDragState = std::variant<left_drag::None, left_drag::TextSelect, left_drag::Splitter, left_drag::PaneScrollbar,
                                   left_drag::MdScrollbar, left_drag::BlockHScroll, left_drag::SearchInput>;

// ノード番号や検索語に結び付くドラッグ。文書を差し替えると対象を失う。
inline bool IsDocumentBoundDrag(const LeftDragState& drag) noexcept
{
    return std::holds_alternative<left_drag::TextSelect>(drag) || std::holds_alternative<left_drag::BlockHScroll>(drag) ||
           std::holds_alternative<left_drag::SearchInput>(drag);
}

// 本文ペイン (本文・検索バー・スクロールバー) の上のドラッグ。
inline bool IsMdPaneDrag(const LeftDragState& drag) noexcept
{
    return IsDocumentBoundDrag(drag) || std::holds_alternative<left_drag::MdScrollbar>(drag);
}

struct ViewState {
    ViewportManager viewport;
    PaneController panes;
    // 次に開き終えた文書でスクロールする先。復元する位置か、飛ぶ見出し (UTF-8。別の文書へのリンクの # 以降と --anchor)。
    // 見出しの位置は開き終えるまで分からないので、名前のまま持つ。
    std::variant<std::monostate, ScrollTarget, std::string> pending_scroll;
    NavHistory nav_history;

    // ブロック単位の横スクロール。キーは LayoutCache のノードインデックス。
    // ファイルロード/リロードでクリア (永続化しない)。スクロールバーはホバー中とドラッグ中の
    // 対象のみ描画するため、map サイズはユーザー操作分に限定される。
    std::unordered_map<int, float> block_scroll_x;
    HoverHighlights hover;
    // SyncActiveTocIndex が描画のたびに決め直す。
    int active_toc_index = -1;
    // 読み込みのスピナーを本文ペインの代わりに出している。その間は本文・ナビボタン・検索バー・スクロールバーを描かないので、
    // ポインタもそれらに当てない。
    bool loading_shown = false;
    // 目次クリック直後の同期 1 回だけ、目次ペインを追従スクロールさせない (issue#259)。
    // クリックした項目がペイン内で動いて見失うのを防ぐ。
    bool keep_toc_pane_once = false;
    LeftDragState drag;

    bool HasPendingScroll() const noexcept
    {
        return !std::holds_alternative<std::monostate>(pending_scroll);
    }

    bool IsDragging() const noexcept
    {
        return !std::holds_alternative<left_drag::None>(drag);
    }
    bool IsSelectingText() const noexcept
    {
        return std::holds_alternative<left_drag::TextSelect>(drag);
    }
    // 横スクロールをドラッグ中のブロック。ドラッグしていなければ -1。
    int HDragNode() const noexcept
    {
        const auto* h = std::get_if<left_drag::BlockHScroll>(&drag);
        return h ? h->node : -1;
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
    // EDIT の選択。selection_start は基点で、caret_pos はその前のことも後ろのこともある。-1 はテキスト末尾。
    int caret_pos = -1;
    int selection_start = -1;
    std::wstring ime_composition;
    // 入力欄の検索クエリ。表示とヒットテストにはこのまま使い、検索には UTF-8 にして渡す。
    std::wstring query_wide;
    // 区切った検索が終わったら、見つけたマッチへスクロールする。
    bool scroll_after_search = false;
};

// 移動・サイズ変更のモーダルループ (WM_ENTERSIZEMOVE から WM_EXITSIZEMOVE まで)。
enum class SizeMove : uint8_t {
    None,
    // 移動だけ。終了時のレイアウト処理を省く。
    Moving,
    // 大きさか DPI が変わった。終了時に描画面とレイアウトを合わせ直す。
    Resized,
};

struct WindowState {
    TitleBar titlebar;
    SizeMove size_move = SizeMove::None;
    bool window_active = true;
    float cached_dpi_scale = 1.0f;
    // 直近の WM_SIZE のクライアント領域 (物理 px)。ペイン配置の元になり、同じ大きさの WM_SIZE を無視するのにも使う。
    uint32_t client_width = 0;
    uint32_t client_height = 0;
    // client_width / client_height を処理したときの DPI スケール。DPI が変わると同じピクセル寸法でも DIP 寸法が変わる。
    float client_dpi_scale = 0.0f;

    constexpr bool IsSizing() const noexcept
    {
        return size_move != SizeMove::None;
    }
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

    // ThemeService が所有する Theme への非所有参照。App の構築時に設定し、null にはならない。
    // ダークモードとズームの切り替えは内部値を in-place で書き換えるためポインタは
    // 安定。reducer 系が Y 位置計算に必要な spacing_above 等を参照するために使う。
    const Theme* theme = nullptr;

    FileExplorer file_explorer;
};

// マウスキャプチャを持つ条件。Reduce が前後を比べて取得と解放を決めるので、個々の処理では指示しない。
inline bool CaptureRequired(const AppState& state) noexcept
{
    return state.view.IsDragging() || state.interaction.gesture.GetPhase() != GesturePhase::Idle;
}

inline ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept
{
    return state.view.viewport.CurrentTarget(state.document.layout_cache);
}

inline NavEntry CurrentNavEntry(const AppState& state)
{
    const ScrollTarget t = SnapshotVisibleTarget(state);
    return NavEntry{ state.document.doc.GetFilePath(), t.node, t.offset };
}

// reducer と App (Win32 層) の双方が AppState から導出するペイン/ブロック情報。
// 非 inline 関数の定義は reducer.cpp。

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

// Reveal ボタンを押せるか。無効時は描画のみ行い、ヒットしない。
inline bool SidePaneRevealEnabled(const AppState& state, PaneTarget target) noexcept
{
    return target == PaneTarget::File && state.document.doc.HasBackingFile();
}

// window の寸法・ペイン・テーマから組む。数十回の浮動小数演算なので持たずに毎回求める。
PaneLayout ComputePaneLayout(const AppState& state) noexcept;
// Mermaid のキャッシュキーに入るため、全経路でこの値を使う (ずれるとキャッシュを外す)。
inline float MdContentWidth(const AppState& state) noexcept
{
    return state.theme->ContentWidth(ComputePaneLayout(state).md_rect.width);
}
// md_width は本文ペインの幅。配置を求め済みならその幅を渡す。
BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index, float md_width) noexcept;
inline BlockHScrollGeometry ResolveBlockHScrollGeometry(const AppState& state, int node_index) noexcept
{
    return ResolveBlockHScrollGeometry(state, node_index, ComputePaneLayout(state).md_rect.width);
}
// 総コンテンツ高はアクションのペイロードで運ばず、都度導出する
// (運ぶと、遅延レイアウト進行中にドラッグ開始と移動で別計測の値が混ざり、つまみの位置が跳ねる)。
inline PaneScrollInfo MdScrollInfo(const AppState& state, const PaneLayout& layout) noexcept
{
    return MdPaneScrollbar(state.document.layout_cache, layout.md_rect, state.theme->margin_bottom).info;
}
inline float MdMaxScroll(const AppState& state) noexcept
{
    return MdScrollInfo(state, ComputePaneLayout(state)).max_scroll;
}
inline SearchBarLayout SearchBarLayoutFor(const AppState& state, const PaneRect& md_rect) noexcept
{
    return ComputeSearchBarLayout(md_rect, !state.search.query_wide.empty());
}
