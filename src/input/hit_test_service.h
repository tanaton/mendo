#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "ui_types.h"
#include "theme.h"
#include "ui_constants.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <ranges>
#include <unordered_map>

// 描画とヒットテスト (NavButtonHitTest) が共有するナビボタン矩形。テストもこの API で座標を得る。
inline D2D1_RECT_F NavBackButtonRect(const PaneRect& md_rect) noexcept
{
    const float x = md_rect.x + md_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2.0f - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    const float y = md_rect.y + md_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;
    return D2D1::RectF(x, y, x + NAV_BTN_SIZE, y + NAV_BTN_SIZE);
}

inline D2D1_RECT_F NavForwardButtonRect(const PaneRect& md_rect) noexcept
{
    const D2D1_RECT_F back = NavBackButtonRect(md_rect);
    const float x = back.right + NAV_BTN_GAP;
    return D2D1::RectF(x, back.top, x + NAV_BTN_SIZE, back.bottom);
}

// テーブルの行/列ヒット判定。座標はテーブル原点 (エントリ上端 / base_x) からのローカル系。
// 罫線の帰属は描画 (GenTable) に合わせる: 行 r は上罫線を含む [cum_y[r], cum_y[r+1])、
// 列 c は左罫線を含む [cum_x[c] - border, cum_x[c+1] - border)。
struct TableRowHit {
    int row = -1;
    float row_top = 0.0f;
};

// 行幾何が未確定ならヒットなし。
inline TableRowHit FindTableRow(const TableLayoutData& tl, size_t row_count, float local_y) noexcept
{
    if (row_count == 0 || local_y < 0.0f || !tl.HasRowGeometry(row_count)) {
        return {};
    }
    const int idx = tl.RowIndexAt(local_y);
    if (idx < 0) {
        return {};
    }
    return { idx, tl.row_cum_y[static_cast<size_t>(idx)] };
}

struct TableColHit {
    int col = 0;
    float cell_left = TABLE_BORDER_WIDTH;
};

// テーブル左右の外側は端の列にクランプする。列幾何が無効 (InvalidateGeometry 後) なら先頭列。
inline TableColHit FindTableCol(const TableLayoutData& tl, float local_x) noexcept
{
    if (!tl.HasColGeometry()) {
        return {};
    }
    const size_t col_count = tl.col_widths.size();
    const auto it = std::ranges::upper_bound(tl.col_cum_x, local_x + TABLE_BORDER_WIDTH);
    const auto pos = static_cast<size_t>(std::ranges::distance(tl.col_cum_x.begin(), it));
    const size_t idx = std::clamp(pos, size_t{ 1 }, col_count) - 1;
    return { static_cast<int>(idx), tl.col_cum_x[idx] };
}

struct MdPaneHitContext {
    const std::pmr::vector<Node>& nodes;
    const LayoutCache& cache;
    const Theme& theme;
    float scroll_y;
    PaneRect md_rect;
    float dpi_scale;
    int screen_x;
    int screen_y;
    // ボタン位置とブロック横スクロールの clamp に使う。
    float content_width = 0.0f;
    // ブロック単位の横スクロール状態。null なら全 0 扱い。
    const std::pmr::unordered_map<int, float>* block_scroll_x = nullptr;
};

// MD ペインのヒットテスト。状態を持たない純粋関数群。
namespace hit_test {

struct HitResult {
    static constexpr uint32_t kNoChar = std::numeric_limits<uint32_t>::max();

    int node_index = -1;
    // キャレット位置 (選択の端)。文字の右半分を指すとその文字の後ろになる。
    uint32_t text_pos = 0;
    // 点の下にある文字の位置 (リンクの判定)。文字の上でなければ kNoChar。
    uint32_t char_pos = kNoChar;
};

HitResult HitTest(const MdPaneHitContext& ctx) noexcept;
// HitTest と同じノードを返す。文字位置 (DirectWrite のヒットテストと UTF-16 変換) は求めない。
int HitNode(const MdPaneHitContext& ctx) noexcept;

HitResult HitTestTable(
    const Node& node, const NodeLayoutEntry& entry,
    float entry_text_top,
    int node_index,
    const Theme& theme,
    float dip_x, float dip_y, float h_scroll_x = 0.0f) noexcept;

NavButtonHover NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) noexcept;

// 可視ノードのオーバーレイボタン (コードのコピー / 図の保存・コピー) のうちマウス位置にあるもの。
OverlayButtonHit OverlayButtonHitTest(const MdPaneHitContext& ctx) noexcept;
// node_index のブロック横スクロールバー (ヒット余白込み) 上か。
bool BlockHScrollbarHitTest(const MdPaneHitContext& ctx, int node_index, float visible_width) noexcept;

} // namespace hit_test
