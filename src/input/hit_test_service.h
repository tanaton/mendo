#pragma once
#include "types.h"
#include "layout_cache.h"
#include "pane.h"
#include "theme.h"
#include <dwrite.h>
#include <optional>
#include <string>
#include <memory_resource>

class HitTestService {
public:
    struct HitResult {
        int node_index = -1;
        uint32_t text_pos = 0;
    };

    // Md ペイン内のヒットテスト
    HitResult HitTest(const std::pmr::vector<Node>& nodes,
        const LayoutCache& cache,
        const Theme& theme,
        float scroll_y,
        float md_pane_left,
        float dpi_scale,
        int screen_x, int screen_y) const noexcept;

    // テーブルセル内のヒットテスト
    HitResult HitTestTable(const Node& node, const NodeLayoutEntry& entry,
        int node_index,
        const Theme& theme,
        float dip_x, float dip_y) const noexcept;

    // ナビゲーションボタンのヒットテスト
    enum class NavButtonHover { None, Back, Forward };
    NavButtonHover NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const noexcept;

    // コードブロックのコピーボタンのヒットテスト。
    // ヒットしたコードブロックのノードインデックスを返す（-1=なし）。
    int CopyButtonHitTest(const std::pmr::vector<Node>& nodes,
        const LayoutCache& cache,
        const Theme& theme,
        float scroll_y,
        float md_pane_left,
        float content_width,
        float md_pane_height,
        float dpi_scale,
        int screen_x, int screen_y) const noexcept;

    // Mermaidダイアグラムの保存ボタンのヒットテスト。
    // ヒットしたダイアグラムのノードインデックスを返す（-1=なし）。
    int SaveButtonHitTest(const std::pmr::vector<Node>& nodes,
        const LayoutCache& cache,
        const Theme& theme,
        float scroll_y,
        float md_pane_left,
        float content_width,
        float md_pane_height,
        float dpi_scale,
        int screen_x, int screen_y) const noexcept;
};
