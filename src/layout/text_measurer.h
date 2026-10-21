#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include <limits>

// 計測バックエンドに渡すときはエントリ上端からのローカル座標 (ToLocal 済み)。
struct MeasureViewportRange {
    static constexpr float kInf = std::numeric_limits<float>::infinity();
    float top = -kInf;
    float bottom = kInf;
    constexpr MeasureViewportRange ToLocal(float entry_top) const noexcept
    {
        return { top - entry_top, bottom - entry_top };
    }
};

struct TableRestoreResult {
    bool restored = false;
    // 旧幅の行高さのまま evict されていた行を実測し直して高さが変わった
    bool height_changed = false;
};

// IDWriteFactory はスレッドセーフで IDWriteTextLayout は per-call 生成のため、
// 同一インスタンスを複数ワーカーから const 経由で叩いて良い。
// lifecycle 系 (RecreateFormats / UpdateTheme) は non-const で UI スレッドからのみ呼ぶ。
// 並列計測には const 参照で渡すので、計測中に lifecycle 系は呼べない。
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;

    virtual bool RecreateFormats() = 0;
    virtual void UpdateTheme(const Theme& theme) noexcept = 0;

    // 並列計測では worker ごとに別ノード・別 entry を渡す。
    // viewport はテーブルで行単位の部分復元判定に用いる。
    // ビットマップを描くノード (IsBitmapDrawNode) は渡さない。高さは MeasureEntry が BitmapNodeHeight で決める。
    virtual void MeasureNode(
        const Node& node, NodeLayoutEntry& entry, float max_width,
        MeasureViewportRange viewport = {}) const = 0;
    // 幅不変のまま行単位 evict されたテーブルの、viewport 内の evict 行だけセルを再生成する。
    // layout_dirty を経由しないため、可視中に毎フレーム全行を再計測することがない。
    virtual TableRestoreResult RestoreEvictedTableRows(const Node& /*node*/, NodeLayoutEntry& /*entry*/, float /*max_width*/,
                                                       MeasureViewportRange /*viewport*/) const
    {
        return {};
    }
};

// 組んだ幅 (NodeLayoutWidth) と実測値を cached_width/height に残し、同じ幅へ戻った際に
// EstimateInvisibleNodeHeight が推定値で上書きしないようにする。
// 計測に成功したら失敗回数を 0 に戻し、以後の一時的な失敗を初回として扱う。
// viewport は文書座標で受け取り、entry_top 基準のローカル座標に直してバックエンドへ渡す。
// bitmap は画像/図ノードの DiagramEntry (無ければ nullptr)。
inline void MeasureEntry(
    const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry, float node_width,
    MeasureViewportRange viewport, float entry_top, const DiagramEntry* bitmap, const Theme& theme)
{
    if (IsBitmapDrawNode(node)) {
        entry.height = layout::BitmapNodeHeight(bitmap, node_width, entry.height, theme);
        entry.layout_dirty = false;
    }
    else {
        measurer.MeasureNode(node, entry, node_width, viewport.ToLocal(entry_top));
    }
    entry.cached_width = layout::NodeLayoutWidth(node, node_width);
    entry.cached_height = entry.height;
    entry.measure_failures = 0;
}
