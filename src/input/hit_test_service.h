#pragma once
#include "doc_dwrite_bridge.h"
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "ui_types.h"
#include "theme.h"
#include "ui_constants.h"
#include <concepts>
#include <limits>
#include <memory_resource>
#include <unordered_map>

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

// NavButtonHitTest と同一の式。
inline constexpr ButtonRect NavBackButtonRect(const PaneRect& md_rect) noexcept
{
    const float x = md_rect.x + md_rect.width - NAV_BTN_MARGIN - NAV_BTN_SIZE * 2.0f - NAV_BTN_GAP - NAV_BTN_SCROLLBAR_OFFSET;
    const float y = md_rect.y + md_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE;
    return { x, y, NAV_BTN_SIZE, NAV_BTN_SIZE };
}

inline constexpr ButtonRect NavForwardButtonRect(const PaneRect& md_rect) noexcept
{
    const ButtonRect back = NavBackButtonRect(md_rect);
    return { back.x + NAV_BTN_SIZE + NAV_BTN_GAP, back.y, NAV_BTN_SIZE, NAV_BTN_SIZE };
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

    HitResult HitTest(const MdPaneHitContext& ctx) const noexcept;

    HitResult HitTestTable(
        const Node& node, const NodeLayoutEntry& entry,
        float entry_text_top,
        int node_index,
        const Theme& theme,
        float dip_x, float dip_y, float h_scroll_x = 0.0f) const noexcept;

    NavButtonHover NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) const noexcept;

    int CopyButtonHitTest(const MdPaneHitContext& ctx) const noexcept;

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

    mutable HitCache<HitResult> last_md_hit_{};
    mutable HitCache<CodeBlockButtonHit> button_cache_{};

    // HitTestPoint の UTF-16→UTF-8 逆変換を同一ノード/セル間で再利用する
    // (ドラッグ選択時の連続呼び出しで decode を抑える)。
    // HitTest 冒頭で ResetIfBufferChanged を呼び、ドキュメント切替時の string_view
    // dangling を防ぐ。
    mutable mendo::WideViewCache md_wv_cache_;
    mutable mendo::WideViewCache cell_wv_cache_;
};
