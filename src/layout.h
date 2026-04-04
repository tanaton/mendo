#pragma once
#include "types.h"
#include "layout_cache.h"
#include "theme.h"
#include "text_measurer.h"
#include <dwrite.h>
#include <memory_resource>

// テーブルの自然幅（実測値）と利用可能な幅から列幅を計算する。
// 最終的な列幅のベクターを返す。
[[nodiscard]] std::pmr::vector<float> ComputeColumnWidths(const std::pmr::vector<float>& natural_widths,
    float available_width, size_t col_count);

// テーブル行から線形化テキストを構築する（セルはタブ区切り、行は改行区切り）。
// テキスト選択機能のサポートに使用する。
[[nodiscard]] std::pmr::wstring BuildLinearizedTableText(const std::pmr::vector<TableRow>& rows);

// from_index 以降の全ノードの Y 位置とスペーシングを再計算する。
// {total_height, has_dirty_nodes} を返す。
struct YPositionResult {
    float total_height = 0.0f;
    bool has_dirty_nodes = false;
};
// safe_exit_after: この位置以降でY位置が一致すれば早期終了する。
// SIZE_MAX（デフォルト）の場合は早期終了しない。
YPositionResult RecomputeYPositions(std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme,
    size_t from_index = 0, bool has_earlier_dirty = false, size_t safe_exit_after = SIZE_MAX) noexcept;

// DirectWriteを使わず、ノードの種類からおおよその高さを割り当ててY座標を推定する。
// セッション復元時のスクロール位置計算用（O(n)の算術演算のみ、layout_dirtyは変更しない）。
void EstimateNodeHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, const Theme& theme) noexcept;

class LayoutEngine {
public:
    bool Init(ITextMeasurer* measurer, const Theme& theme);
    void UpdateTheme(const Theme& theme) noexcept { theme_ = &theme; measurer_->UpdateTheme(theme); }
    // すべてのテキストフォーマットオブジェクトを再作成する（ズームやテーマ変更後など）。
    bool RecreateFormats();
    void ComputeLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top = -1.0f, float viewport_bottom = -1.0f);
    void LayoutNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    bool ProcessDirtyBatch(std::pmr::vector<Node>& nodes, LayoutCache& cache,
        float viewport_width, int batch_size, int time_budget_us = 0);
    bool EnsureVisibleLayout(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top, float viewport_bottom);
    constexpr bool HasDirtyNodes() const noexcept { return has_dirty_nodes_; }
    constexpr float GetTotalHeight() const noexcept { return total_height_; }
    constexpr void SetTotalHeight(float h) noexcept { total_height_ = h; }

private:
    ITextMeasurer* measurer_ = nullptr;
    const Theme* theme_ = nullptr;

    float total_height_ = 0.0f;
    float last_viewport_width_ = 0.0f;
    bool has_dirty_nodes_ = false;
};
