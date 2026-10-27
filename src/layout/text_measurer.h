#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "theme.h"
#include <optional>

class TaskScheduler;

struct TableRestoreResult {
    bool restored = false;
    // 見積もった行高さで evict されていた行を実測し直して高さが変わったときの新しい高さ。
    std::optional<float> height;
};

// IDWriteFactory はスレッドセーフで IDWriteTextLayout は per-call 生成のため、
// 同一インスタンスを複数ワーカーから const 経由で叩いて良い。
// lifecycle 系 (RecreateFormats / SetTheme / SetScheduler) は non-const で UI スレッドからのみ呼ぶ。
// 並列計測には const 参照で渡すので、計測中に lifecycle 系は呼べない。
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;

    virtual bool RecreateFormats() = 0;

    // 計測に使うテーマ。ズームや色の切り替えは同じ Theme を書き換えるので、渡すのは 1 回でよい。
    void SetTheme(const Theme& theme) noexcept
    {
        theme_ = &theme;
    }
    const Theme& GetTheme() const noexcept
    {
        return *theme_;
    }
    // 並列計測 (ノード単位 / 巨大テーブルのセル単位) に使う scheduler。nullptr なら呼び出しスレッドで直列に計測する。
    // scheduler の Shutdown より前に nullptr へ戻して参照を切る契約。
    void SetScheduler(TaskScheduler* scheduler) noexcept
    {
        scheduler_ = scheduler;
    }
    TaskScheduler* Scheduler() const noexcept
    {
        return scheduler_;
    }

    // 測った高さを返す。測れなければ nullopt で、エントリは dirty のまま再試行を待つ (例外と同じく上限で諦める)。
    // entry にはレイアウトなどの計測の産物だけを書き、高さと dirty の確定は MeasureEntry (CommitMeasured) に任せる。
    // 並列計測では worker ごとに別ノード・別 entry を渡す。
    // viewport (エントリ上端からのローカル座標) はテーブルで行単位の部分復元判定に用いる。
    // ビットマップを描くノード (IsBitmapDrawNode) は渡さない。高さは MeasureEntry が BitmapNodeHeight で決める。
    virtual std::optional<float> MeasureNode(
        const Node& node, NodeLayoutEntry& entry, float max_width,
        YRange viewport) const = 0;
    // 幅不変のまま行単位 evict されたテーブルの、viewport 内の evict 行だけセルを再生成する。
    // layout_dirty を経由しないため、可視中に毎フレーム全行を再計測することがない。
    // 高さが変わったときの確定は呼び出し側 (CommitMeasured) に任せる。
    virtual TableRestoreResult RestoreEvictedTableRows(const Node& /*node*/, NodeLayoutEntry& /*entry*/, float /*max_width*/,
                                                       YRange /*viewport*/) const
    {
        return {};
    }

protected:
    const Theme* theme_ = nullptr;
    TaskScheduler* scheduler_ = nullptr;
};

// 測れたら組んだ幅 (NodeLayoutWidth) と実測値を cached_width/height に残し、同じ幅へ戻った際に
// EstimateInvisibleNodeHeight が推定値で上書きしないようにする。失敗回数も 0 に戻し、以後の一時的な失敗を初回として扱う。
// 測れなければ何も確定せず、推定の高さを実測値として残さない。
// viewport は文書座標で受け取り、entry_top 基準のローカル座標に直してバックエンドへ渡す。
// bitmap は画像/図ノードの DiagramEntry (無ければ nullptr)。戻り値は測れたか。
inline bool MeasureEntry(
    const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry, float node_width,
    YRange viewport, float entry_top, const DiagramEntry* bitmap, const Theme& theme)
{
    const std::optional<float> height = IsBitmapDrawNode(node)
                                            ? layout::BitmapNodeHeight(bitmap, node_width, entry.height, theme)
                                            : measurer.MeasureNode(node, entry, node_width, viewport.ToLocal(entry_top));
    if (height) {
        entry.CommitMeasured(layout::NodeLayoutWidth(node, node_width), *height);
    }
    return height.has_value();
}

// RestoreEvictedTableRows で可視の evict 行を戻し、行の実測で高さが変わったら確定する。viewport はローカル座標。
inline TableRestoreResult RestoreEntryTableRows(
    const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry, float node_width, YRange viewport)
{
    const TableRestoreResult result = measurer.RestoreEvictedTableRows(node, entry, node_width, viewport);
    if (result.height) {
        entry.CommitMeasured(layout::NodeLayoutWidth(node, node_width), *result.height);
    }
    return result;
}
