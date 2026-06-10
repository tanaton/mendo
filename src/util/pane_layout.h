#pragma once
#include "ui_constants.h"
#include "ui_types.h"
#include <optional>

enum class PaneZone : uint8_t {
    None,
    FilePane,
    Splitter1,
    TocPane,
    Splitter2,
    MdPane
};

// PaneZone と異なり File/Toc のみ。
enum class PaneTarget : uint8_t {
    File,
    Toc
};

constexpr std::optional<PaneTarget> ToPaneTarget(PaneZone zone) noexcept
{
    switch (zone) {
    case PaneZone::FilePane:
        return PaneTarget::File;
    case PaneZone::TocPane:
        return PaneTarget::Toc;
    default:
        return std::nullopt;
    }
}

constexpr PaneZone ToPaneZone(PaneTarget target) noexcept
{
    return target == PaneTarget::File ? PaneZone::FilePane : PaneZone::TocPane;
}

struct PaneLayout {
    PaneRect file_rect{};
    PaneRect toc_rect{};
    PaneRect md_rect{};

    constexpr const PaneRect& Get(PaneTarget t) const noexcept
    {
        return t == PaneTarget::File ? file_rect : toc_rect;
    }
};

struct PaneScrollInfo {
    float content_top = 0.0f;
    float content_height = 0.0f;
    float total_content = 0.0f;
    float max_scroll = 0.0f;
    float thumb_height = 0.0f;
};

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

// サイドペイン (ファイル/TOC) の総コンテンツ高。スクロール計算・描画・自動スクロールの
// 全層がこの式を共有する (項目の可変高等を導入する際はここだけ変える)。
constexpr float SidePaneContentHeight(size_t item_count, float pane_item_height) noexcept
{
    return static_cast<float>(item_count) * pane_item_height;
}

PaneLayout ComputePaneLayout(
    float total_width, float total_height,
    float file_pane_width, float toc_pane_width,
    float splitter_width, bool show_file, bool show_toc,
    float md_min_width = MD_PANE_MIN_WIDTH, float top_offset = 0.0f) noexcept;
PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout, float splitter_width, bool show_file, bool show_toc) noexcept;
PaneScrollInfo ComputeScrollInfo(const PaneRect& rect, float header_height, float total_content, float thumb_min = PANE_SCROLLBAR_THUMB_MIN) noexcept;
float ComputeThumbY(const PaneScrollInfo& info, float scroll_y) noexcept;
float ScrollFromThumbY(const PaneScrollInfo& info, float thumb_y) noexcept;
