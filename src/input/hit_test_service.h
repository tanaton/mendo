#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "nav_button.h"
#include "pane.h"
#include "theme.h"
#include <dwrite.h>
#include <optional>
#include <string>
#include <memory_resource>

// MDペインのヒットテストに必要なコンテキスト情報。
// ドキュメントデータ、ビューポート状態、マウス位置をまとめる。
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
};

class HitTestService {
public:
    struct HitResult {
        int node_index = -1;
        uint32_t text_pos = 0;
    };

    // Md ペイン内のヒットテスト
    HitResult HitTest(const MdPaneHitContext& ctx) const noexcept;

    // テーブルセル内のヒットテスト
    HitResult HitTestTable(const Node& node, const NodeLayoutEntry& entry,
        int node_index,
        const Theme& theme,
        float dip_x, float dip_y) const noexcept;

    // ナビゲーションボタンのヒットテスト
    NavButtonHover NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const noexcept;

    // コードブロックのコピーボタンのヒットテスト。
    // ヒットしたコードブロックのノードインデックスを返す（-1=なし）。
    int CopyButtonHitTest(const MdPaneHitContext& ctx) const noexcept;

    // Mermaidダイアグラムの保存ボタンのヒットテスト。
    // ヒットしたダイアグラムのノードインデックスを返す（-1=なし）。
    int SaveButtonHitTest(const MdPaneHitContext& ctx) const noexcept;

private:
    // 同一座標の連続ヒットテストを高速化する結果キャッシュ。
    // effects_generation が変わると自動で無効化される。
    template <typename T>
    struct HitCache {
        int screen_x = INT_MIN, screen_y = INT_MIN;
        float scroll_y = 0.0f;
        uint32_t effects_gen = UINT32_MAX;
        T result{};

        bool Matches(const MdPaneHitContext& ctx, uint32_t gen) const noexcept
        {
            return ctx.screen_x == screen_x && ctx.screen_y == screen_y
                && ctx.scroll_y == scroll_y && gen == effects_gen;
        }
        void Store(const MdPaneHitContext& ctx, uint32_t gen, const T& r) noexcept
        {
            screen_x = ctx.screen_x;
            screen_y = ctx.screen_y;
            scroll_y = ctx.scroll_y;
            effects_gen = gen;
            result = r;
        }
    };
    mutable HitCache<HitResult> last_md_hit_{};
    mutable HitCache<int> last_copy_hit_{ .result = -1 };
    mutable HitCache<int> last_save_hit_{ .result = -1 };
};
