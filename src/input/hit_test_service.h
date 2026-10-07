#pragma once
#include "block_h_scroll_context.h"
#include "doc_dwrite_bridge.h"
#include "document_types.h"
#include "layout_cache.h"
#include "ui_types.h"
#include "theme.h"
#include "ui_constants.h"
#include <algorithm>
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
// 累積配列が未確定 (evict 直後など) の線形フォールバックも同じ帰属を返すこと。
struct TableRowHit {
    int row = -1;
    float row_top = 0.0f;
};

inline TableRowHit FindTableRow(const TableLayoutData& tl, size_t row_count, float fallback_row_h, float local_y) noexcept
{
    if (row_count == 0 || local_y < 0.0f) {
        return {};
    }
    if (tl.HasRowGeometry(row_count)) {
        const int idx = tl.RowIndexAt(local_y);
        if (idx < 0) {
            return {};
        }
        return { idx, tl.row_cum_y[static_cast<size_t>(idx)] };
    }
    float ry = 0.0f;
    for (size_t r = 0; r < row_count; r++) {
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : fallback_row_h;
        const float row_bottom = ry + row_h + TABLE_BORDER_WIDTH;
        if (local_y < row_bottom) {
            return { static_cast<int>(r), ry };
        }
        ry = row_bottom;
    }
    return {};
}

struct TableColHit {
    int col = 0;
    float cell_left = TABLE_BORDER_WIDTH;
};

// テーブル左右の外側は端の列にクランプする。
inline TableColHit FindTableCol(const TableLayoutData& tl, float local_x) noexcept
{
    const size_t col_count = tl.col_widths.size();
    if (col_count == 0) {
        return {};
    }
    if (tl.col_cum_x.size() == col_count + 1) {
        const auto it = std::ranges::upper_bound(tl.col_cum_x, local_x + TABLE_BORDER_WIDTH);
        const auto pos = static_cast<size_t>(std::ranges::distance(tl.col_cum_x.begin(), it));
        const size_t idx = std::clamp(pos, size_t{ 1 }, col_count) - 1;
        return { static_cast<int>(idx), tl.col_cum_x[idx] };
    }
    float cx = TABLE_BORDER_WIDTH;
    for (size_t c = 0; c + 1 < col_count; c++) {
        const float col_right = cx + tl.col_widths[c] + TABLE_CELL_PADDING * 2.0f;
        if (local_x < col_right) {
            return { static_cast<int>(c), cx };
        }
        cx = col_right + TABLE_BORDER_WIDTH;
    }
    return { static_cast<int>(col_count - 1), cx };
}

struct MdPaneHitContext {
    const std::pmr::vector<Node>& nodes;
    const LayoutCache& cache;
    const Theme& theme;
    float scroll_y;
    float md_pane_left;
    float dpi_scale;
    int screen_x;
    int screen_y;
    // ボタンヒットテスト用（HitTestでは未使用）
    float content_width = 0.0f;
    float md_pane_height = 0.0f;
    // ブロック単位の横スクロール状態。null なら全 0 扱い。
    const std::pmr::unordered_map<int, float>* block_scroll_x = nullptr;
};

inline float LookupBlockScrollX(const MdPaneHitContext& ctx, int node) noexcept
{
    return LookupBlockScrollX(ctx.block_scroll_x, node);
}

class HitTestService {
public:
    struct HitResult {
        int node_index = -1;
        uint32_t text_pos = 0;
    };

    HitResult HitTest(const MdPaneHitContext& ctx) const noexcept;

    HitResult HitTestTable(
        const Node& node, const NodeLayoutEntry& entry,
        float entry_text_top,
        int node_index,
        const Theme& theme,
        float dip_x, float dip_y, float h_scroll_x = 0.0f) const noexcept;

    NavButtonHover NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const noexcept;

    // 可視ノード走査・座標変換・キャッシュ照合を共有して Copy / Save / DiagramCopy を一度に判定する。
    struct CodeBlockButtonHit {
        int copy_node = -1;
        int save_node = -1;
        int diagram_copy_node = -1;
    };
    CodeBlockButtonHit CodeBlockButtonsHitTest(const MdPaneHitContext& ctx) const noexcept;

private:
    // 同一座標の連続ヒットテストを高速化する結果キャッシュ。
    // effects_generation が変わると自動で無効化される。
    template <typename T>
    struct HitCache {
        int screen_x = std::numeric_limits<int>::min(), screen_y = std::numeric_limits<int>::min();
        float scroll_y = 0.0f;
        uint32_t effects_gen = std::numeric_limits<uint32_t>::max();
        // 結果がブロック横スクロールに依存する場合のスナップショット (-1 = 非依存)。
        // 該当ノードの scroll_x が変わったらミス扱いにする。effects_generation を
        // 横スクロールで進める方式だと、Renderer の effects 再適用まで巻き添えになる。
        int h_scroll_node = -1;
        float h_scroll_x = 0.0f;
        T result{};

        constexpr bool Matches(const MdPaneHitContext& ctx, uint32_t gen) const noexcept
        {
            if (ctx.screen_x != screen_x || ctx.screen_y != screen_y || ctx.scroll_y != scroll_y || gen != effects_gen) {
                return false;
            }
            return h_scroll_node < 0 || LookupBlockScrollX(ctx, h_scroll_node) == h_scroll_x;
        }
        constexpr void Store(const MdPaneHitContext& ctx, uint32_t gen, const T& r, int scroll_node = -1, float scroll_x = 0.0f) noexcept
        {
            screen_x = ctx.screen_x;
            screen_y = ctx.screen_y;
            scroll_y = ctx.scroll_y;
            effects_gen = gen;
            h_scroll_node = scroll_node;
            h_scroll_x = scroll_x;
            result = r;
        }
    };

    mutable HitCache<HitResult> last_md_hit_{};
    mutable HitCache<CodeBlockButtonHit> button_cache_{};

    // HitTestPoint の UTF-16→UTF-8 逆変換を同一ノード/セル間で再利用する
    // (ドラッグ選択時の連続呼び出しで decode を抑える)。
    // HitTest 冒頭で ResetIfBufferChanged を呼び、ドキュメント切替時の string_view
    // dangling を防ぐ。
    mutable mendo::WideViewCache md_wv_cache_;
    mutable mendo::WideViewCache cell_wv_cache_;
};
