#pragma once
#include "layout.h"
#include "viewport_manager.h"
#include "document.h"
#include "layout_cache.h"

class LayoutService {
public:
    LayoutService(LayoutEngine& engine, ViewportManager& viewport) noexcept
        : engine_(engine), viewport_(viewport)
    {
    }

    // ビューポート優先レイアウト（リサイズ時）
    void ViewportLayout(Document& doc, LayoutCache& cache, float width, float height);

    // ダーティバッチ処理（遅延レイアウト）
    // viewport_height > 0 の場合、ビューポート付近のダーティノードのみ処理する。
    bool ProcessDirtyBatch(Document& doc, LayoutCache& cache, float width, int batch_size, int time_budget_us = 0,
        float viewport_height = -1.0f, float buffer_screens = 5.0f);

    // 可視領域のレイアウト保証（OnPaint 時）
    bool EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height);

    // ダイアグラム反映後の Y 位置再計算
    void RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme) noexcept;

    // ダーティノードが残っているか
    constexpr bool HasDirtyNodes() const noexcept { return engine_.HasDirtyNodes(); }

    // 合計高さ
    constexpr float GetTotalHeight() const noexcept { return engine_.GetTotalHeight(); }
    constexpr void SetTotalHeight(float h) noexcept { engine_.SetTotalHeight(h); }

private:
    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
