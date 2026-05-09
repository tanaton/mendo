#pragma once
#include "ui_constants.h"
#include "ui_types.h"

// ペイン領域識別子（ある座標がどのペインに属するか）
enum class PaneZone : uint8_t {
    None,
    FilePane,
    Splitter1,
    TocPane,
    Splitter2,
    MdPane
};

struct PaneLayout {
    PaneRect file_rect{};
    PaneRect toc_rect{};
    PaneRect md_rect{};
};

// スクロールバーの描画と操作に使う情報。
struct PaneScrollInfo {
    float content_top = 0.0f;
    float content_height = 0.0f;
    float total_content = 0.0f;
    float max_scroll = 0.0f;
    float thumb_height = 0.0f;
};

// 現フレームの PaneLayout 計算結果と、それを生成したウィンドウ幅をまとめてキャッシュする。
// Invalidate() / Set() を経由することで、Reducer / App が個別のフラグや幅を直接触らずに済む。
class PaneLayoutCache {
public:
    constexpr void Invalidate() noexcept
    {
        valid_ = false;
    }
    constexpr void Set(float window_width, const PaneLayout& layout) noexcept
    {
        window_width_ = window_width;
        layout_ = layout;
        valid_ = true;
    }
    constexpr bool IsValid() const noexcept
    {
        return valid_;
    }
    constexpr const PaneLayout& Get() const noexcept
    {
        return layout_;
    }
    constexpr float WindowWidth() const noexcept
    {
        return window_width_;
    }

private:
    PaneLayout layout_{};
    float window_width_ = 0.0f;
    bool valid_ = false;
};

PaneLayout ComputePaneLayout(
    float total_width, float total_height,
    float file_pane_width, float toc_pane_width,
    float splitter_width, bool show_file, bool show_toc,
    float md_min_width = MD_PANE_MIN_WIDTH, float top_offset = 0.0f) noexcept;
PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout, float splitter_width, bool show_file, bool show_toc) noexcept;
PaneScrollInfo ComputeScrollInfo(const PaneRect& rect, float header_height, float total_content, float thumb_min = PANE_SCROLLBAR_THUMB_MIN) noexcept;
float ComputeThumbY(const PaneScrollInfo& info, float scroll_y) noexcept;
float ScrollFromThumbY(const PaneScrollInfo& info, float thumb_y) noexcept;
