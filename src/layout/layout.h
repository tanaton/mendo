#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "text_measurer.h"
#include "theme.h"
#include <memory_resource>

class Document;
class TaskScheduler;
class ViewportManager;

// レガシー呼び出しサイト互換のための using エイリアス。実体は mendo::layout namespace にある。
using mendo::layout::AdvanceNodeY;
using mendo::layout::ComputeColumnWidths;
using mendo::layout::EstimateInvisibleNodeHeight;
using mendo::layout::EstimateNodeHeights;
using mendo::layout::GetSpacingAbove;
using mendo::layout::GetSpacingBelow;
using mendo::layout::NodeBoxPadY;
using mendo::layout::NodeIndent;
using mendo::layout::NodeTextXOffset;
using mendo::layout::RecomputeYPositions;

class LayoutEngine {
public:
    // 小刻みな WM_SIZE で全ノード再レイアウトが頻発するのを防ぐ幅変化の閾値。
    static constexpr float kWidthChangeThreshold = 2.0f;

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
        measurer_->UpdateTheme(theme);
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

private:
    // lifecycle 系 (Init/RecreateFormats/UpdateTheme) は UI スレッドからのみ呼び、
    // MeasureNode は const 経由で layout_scheduler_ 上の worker から並列呼び出しされる。
    ITextMeasurer* measurer_ = nullptr;
    const Theme* theme_ = nullptr;
    TaskScheduler* layout_scheduler_ = nullptr;

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
    void RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme,
                               mendo::layout::HeightChangeRange changed) noexcept;

    constexpr bool HasDirtyNodes() const noexcept
    {
        return engine_.HasDirtyNodes();
    }

private:
    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
