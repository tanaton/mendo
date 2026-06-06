#pragma once
#include "dirty_scheduler.h"
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "text_measurer.h"
#include "theme.h"
#include "viewport_manager.h"
#include <dwrite.h>
#include <memory_resource>

class TaskScheduler;

class Document;

// レガシー呼び出しサイト互換のための using エイリアス。実体は mendo::layout namespace にある。
using mendo::layout::AdvanceNodeY;
using mendo::layout::ComputeColumnWidths;
using mendo::layout::EstimateInvisibleNodeHeight;
using mendo::layout::EstimateNodeHeight;
using mendo::layout::EstimateNodeHeights;
using mendo::layout::GetSpacingAbove;
using mendo::layout::GetSpacingBelow;
using mendo::layout::NodeIndent;
using mendo::layout::NodeTextXOffset;
using mendo::layout::RecomputeYPositions;
using mendo::layout::YPositionResult;

class LayoutEngine {
public:
    bool Init(ITextMeasurer* measurer, const Theme& theme);
    // nullptr で常に RunSerial。Shutdown 時は scheduler の Shutdown より前に
    // SetLayoutScheduler(nullptr) を呼んで参照を切る契約。
    void SetLayoutScheduler(TaskScheduler* scheduler) noexcept
    {
        layout_scheduler_ = scheduler;
    }
    void UpdateTheme(const Theme& theme) noexcept
    {
        theme_ = &theme;
        lifecycle_->UpdateTheme(theme);
    }
    bool RecreateFormats();
    void ComputeLayout(
        std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top = -1.0f, float viewport_bottom = -1.0f);
    void LayoutNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    bool ProcessDirtyBatch(
        std::pmr::vector<Node>& nodes, LayoutCache& cache,
        float viewport_width, int batch_size, int time_budget_us = 0,
        float viewport_top = -1.0f, float viewport_height = -1.0f,
        float buffer_screens = 5.0f);
    bool EnsureVisibleLayout(
        std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top, float viewport_bottom);
    constexpr bool HasDirtyNodes() const noexcept
    {
        return has_dirty_nodes_;
    }
    constexpr float GetTotalHeight() const noexcept
    {
        return total_height_;
    }
    constexpr void SetTotalHeight(float h) noexcept
    {
        total_height_ = h;
    }
    constexpr float GetMarginTop() const noexcept
    {
        return theme_ ? theme_->margin_top : 0.0f;
    }

private:
    // ComputeLayout 末尾の Fenwick 反映: フルパス完走 → bulk load (O(N))、
    // 途中 break 時は処理した分のみ個別 Set。total_height_ は bulk 経路でのみ更新する
    // (incremental 経路は ProcessDirtyBatch / EnsureVisibleLayout が後続で再計算)。
    void ApplyComputeLayoutBlockHeights(
        LayoutCache& cache, const std::pmr::vector<float>& block_heights,
        bool broke_early, float final_y) noexcept;

    // 同一の ITextMeasurer 派生から得た 2 つの IF view。lifecycle 系 (Init/RecreateFormats/UpdateTheme)
    // は UI スレッドからのみ呼び、backend (MeasureNode/MeasureTable) は const 経由で
    // layout_scheduler_ 上の worker から並列呼び出しされる。
    IMeasureLifecycle* lifecycle_ = nullptr;
    IMeasureBackend* backend_ = nullptr;
    const Theme* theme_ = nullptr;
    mendo::layout::DirtyScheduler scheduler_{};
    TaskScheduler* layout_scheduler_ = nullptr;

    std::pmr::vector<float> block_heights_buf_;
    float total_height_ = 0.0f;
    float last_viewport_width_ = 0.0f;
    bool has_dirty_nodes_ = false;
};

// LayoutEngine + ViewportManager の組み合わせを薄くラップし、
// スクロール target 管理付きのレイアウト操作を提供する。
class LayoutService {
public:
    LayoutService(LayoutEngine& engine, ViewportManager& viewport) noexcept
        : engine_(engine), viewport_(viewport)
    {
    }

    void ViewportLayout(Document& doc, LayoutCache& cache, float width, float height);

    // 増分レイアウトのビューポート絞り込み。height > 0 で可視範囲 + 周辺バッファのみ
    // 処理し、height <= 0 (デフォルト) なら全 dirty を順次処理する。
    struct ViewportLimit {
        float height = 0.0f;
        float buffer_screens = 5.0f;
    };
    bool ProcessDirtyBatch(
        Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us = 0,
        ViewportLimit viewport = {});
    bool EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height);
    void RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme) noexcept;

    constexpr bool HasDirtyNodes() const noexcept
    {
        return engine_.HasDirtyNodes();
    }
    constexpr void SetTotalHeight(float h) noexcept
    {
        engine_.SetTotalHeight(h);
    }
    // スクロール上限/スクロールバー計算に使う高さ。末尾 node の spacing_below を含まない。
    // engine_.GetTotalHeight() は末尾余白込みの総描画高さで、これをスクロール上限に使うと sb[last]
    // 分オーバースクロールする (layout_cache.h の ComputeTotalContentHeight 注記参照)。
    // node_count を doc から導出することで App/executor 双方が同一経路を共有する (実装は layout.cpp)。
    float GetScrollableContentHeight(const Document& doc, const LayoutCache& cache) const noexcept;

private:
    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
