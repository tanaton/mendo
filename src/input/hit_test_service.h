#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "ui_types.h"
#include "theme.h"
#include "ui_constants.h"
#include <concepts>
#include <limits>
#include <memory_resource>

// ボタン矩形の純粋表現。テスト側が実装の式を再構築せずに済むよう、
// 各ボタンの矩形を返す API は本ヘッダから提供する。
struct ButtonRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    constexpr bool Contains(float px, float py) const noexcept
    {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

// 戻るボタンの矩形。実装側の NavButtonHitTest と同一の式を使う。
inline constexpr ButtonRect NavBackButtonRect(const PaneRect& md_rect) noexcept
{
    const float x = md_rect.x + md_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2.0f - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    const float y = md_rect.y + md_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;
    return { x, y, NAV_BTN_SIZE, NAV_BTN_SIZE };
}

// 進むボタンの矩形。Back の右に NAV_BTN_GAP の隙間を空けて並ぶ。
inline constexpr ButtonRect NavForwardButtonRect(const PaneRect& md_rect) noexcept
{
    const ButtonRect back = NavBackButtonRect(md_rect);
    return { back.x + NAV_BTN_SIZE + NAV_BTN_GAP, back.y, NAV_BTN_SIZE, NAV_BTN_SIZE };
}

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

// 物理ピクセルをMDペインローカルのDIP座標に変換する。
struct PaneDip {
    float x;
    float y;
};
inline constexpr PaneDip ScreenToPaneDip(const MdPaneHitContext& ctx) noexcept
{
    return {
        ctx.screen_x / ctx.dpi_scale - ctx.md_pane_left,
        ctx.screen_y / ctx.dpi_scale + ctx.scroll_y,
    };
}

class HitTestService {
public:
    struct HitResult {
        int node_index = -1;
        uint32_t text_pos = 0;
    };

    // Md ペイン内のヒットテスト
    HitResult HitTest(const MdPaneHitContext& ctx) const noexcept;

    // テーブルセル内のヒットテスト
    // entry_text_top はノードのテキスト上端 Y (旧 entry.text_top)。
    HitResult HitTestTable(const Node& node, const NodeLayoutEntry& entry,
                           float entry_text_top,
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

    // 可視ノード走査・座標変換・キャッシュ照合を共有して Copy / Save / SvgCopy を一度に判定する。
    struct CodeBlockButtonHit {
        int copy_node = -1;
        int save_node = -1;
        int svg_copy_node = -1;
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
        T result{};

        constexpr bool Matches(const MdPaneHitContext& ctx, uint32_t gen) const noexcept
        {
            return ctx.screen_x == screen_x && ctx.screen_y == screen_y && ctx.scroll_y == scroll_y && gen == effects_gen;
        }
        constexpr void Store(const MdPaneHitContext& ctx, uint32_t gen, const T& r) noexcept
        {
            screen_x = ctx.screen_x;
            screen_y = ctx.screen_y;
            scroll_y = ctx.scroll_y;
            effects_gen = gen;
            result = r;
        }
    };

    // CodeBlock ノードのオーバーレイボタン（Copy/Save）共通ヒットテスト。
    // キャッシュ照合・座標変換・可視範囲走査を一元化し、matches が true を返したノードの index を返す。
    // matches は (i, node, entry, entry_text_top, dip_x, dip_y) を受け取る。
    template <typename Predicate>
        requires std::predicate<Predicate&, int, const Node&, const NodeLayoutEntry&, float, float, float>
    int HitTestCodeBlockButton(
        const MdPaneHitContext& ctx,
        HitCache<int>& cache,
        Predicate&& matches) const noexcept
    {
        if (ctx.nodes.empty()) {
            return -1;
        }
        const uint32_t gen = ctx.cache.GetEffectsGeneration();
        if (cache.Matches(ctx, gen)) {
            return cache.result;
        }

        const auto [dip_x, dip_y] = ScreenToPaneDip(ctx);

        const float viewport_top = ctx.scroll_y;
        const float viewport_bottom = ctx.scroll_y + ctx.md_pane_height;
        const int first = FindFirstVisibleNodeIndex(ctx.cache, ctx.nodes.size(), viewport_top);
        const int count = static_cast<int>(ctx.nodes.size());
        for (int i = first; i < count; i++) {
            const float entry_text_top = ctx.cache[i].text_top;
            // 早期 break: CopyButton は padding 分だけテキスト上端の上に出るため padding を引いて比較する。
            if (entry_text_top - ctx.theme.code_block_padding > viewport_bottom) {
                break;
            }
            const auto& node = ctx.nodes[i];
            if (node.type != NodeType::CodeBlock) {
                continue;
            }
            if (matches(i, node, ctx.cache[i], entry_text_top, dip_x, dip_y)) {
                cache.Store(ctx, gen, i);
                return i;
            }
        }
        cache.Store(ctx, gen, -1);
        return -1;
    }

    mutable HitCache<HitResult> last_md_hit_{};
    mutable HitCache<int> last_copy_hit_{ .result = -1 };
    mutable HitCache<int> last_save_hit_{ .result = -1 };
    mutable HitCache<int> last_svg_copy_hit_{ .result = -1 };
};
