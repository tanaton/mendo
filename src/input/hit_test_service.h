#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "ui_types.h"
#include "theme.h"
#include "ui_constants.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <unordered_map>

// テーブルの行/列ヒット判定。座標はテーブル原点 (エントリ上端 / base_x) からのローカル系。
// 罫線の帰属は描画 (GenTable) に合わせる: 行 r は上罫線を含む [row_cum_y[r], row_cum_y[r+1])、
// 列 c は左罫線を含む [col_cum_x[c] - border, col_cum_x[c+1] - border)。
struct TableRowHit {
    int row = -1;
    double row_top = 0.0;
};

inline TableRowHit FindTableRow(const TableLayoutData& tl, double local_y) noexcept
{
    const int idx = tl.RowIndexAt(local_y);
    if (idx < 0) {
        return {};
    }
    return { idx, tl.row_cum_y[static_cast<size_t>(idx)] };
}

struct TableColHit {
    int col;
    float cell_left;
};

// テーブル左右の外側は端の列にクランプする。
inline TableColHit FindTableCol(const TableLayoutData& tl, float local_x) noexcept
{
    const size_t col_count = tl.col_count;
    const auto it = std::ranges::upper_bound(tl.col_cum_x, local_x + TABLE_BORDER_WIDTH);
    const auto pos = static_cast<size_t>(std::ranges::distance(tl.col_cum_x.begin(), it));
    const size_t idx = std::clamp(pos, size_t{ 1 }, col_count) - 1;
    return { static_cast<int>(idx), tl.col_cum_x[idx] };
}

// 本文ペインの点。X は本文ペインの左端から、Y は文書の先頭から (DIP)。Y は float では刻みが粗くなる大きさになりうるので
// double で持ち、当てるノードの上端との差にしてから float の局所座標にする。
struct DocPoint {
    float x = 0.0f;
    double y = 0.0;
};

struct MdPaneHitContext {
    std::span<const Node> nodes;
    const LayoutCache& cache;
    const Theme& theme;
    double scroll_y;
    // 当てる点。
    DocPoint point;
    // ボタン位置とブロック横スクロールの clamp に使う。
    float content_width = 0.0f;
    // ブロック単位の横スクロール状態。null なら全 0 扱い。
    const std::unordered_map<int, float>* block_scroll_x = nullptr;

    // 点の Y を、ノード node の上端からの Y にする。差は double で取る。表の中は float の刻みが粗くなる大きさになるので
    // Exact のまま使い、ほかは上端を 0 にした系の矩形と float で比べる。
    double LocalYExact(size_t node) const noexcept
    {
        return point.y - cache.Top(node);
    }
    float LocalY(size_t node) const noexcept
    {
        return static_cast<float>(LocalYExact(node));
    }
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
    // 点の下が、描いている画像の箱か。読み込み中の幅 0 の箱と、読めない画像 (アイコンと alt) は含めない。
    // 描いた layout の inline object で決めるので、カーソルと描画が食い違わない。
    bool on_image = false;
};

HitResult HitTest(const MdPaneHitContext& ctx) noexcept;
// HitTest と同じノードを返す。文字位置 (DirectWrite のヒットテストと UTF-16 変換) は求めない。
int HitNode(const MdPaneHitContext& ctx) noexcept;

HitResult HitTestTable(
    const Node& node, const NodeLayoutEntry& entry,
    int node_index,
    const Theme& theme,
    float dip_x, double table_y, float h_scroll_x) noexcept;

NavDirection NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) noexcept;

// 可視ノードのオーバーレイボタン (コードのコピー / 図の保存・コピー・拡大表示) のうちマウス位置にあるもの。
OverlayButtonHit OverlayButtonHitTest(const MdPaneHitContext& ctx) noexcept;
// マウスが node_index の図の画像の上にあり、その図に保存・コピー・拡大表示のボタンを出せるか。
bool DiagramImageHitTest(const MdPaneHitContext& ctx, int node_index) noexcept;
// node_index のブロック横スクロールバー (ヒット余白込み) 上か。
bool BlockHScrollbarHitTest(const MdPaneHitContext& ctx, int node_index, float visible_width) noexcept;

} // namespace hit_test
