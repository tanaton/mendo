#pragma once
#include "ui_types.h"
#include <d2d1.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

// スクロール上限。内容の高さは計測や図の確定で変わり続けるので、値として持たずに使う時点で求める。
constexpr float MaxScroll(float total_height, float viewport_height) noexcept
{
    return std::max(0.0f, total_height - viewport_height);
}

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

inline constexpr float PANE_CLOSE_BTN_MARGIN = 2.0f;

// ペインヘッダーのボタン。右端から 閉じる・更新・「現在のファイルの場所へ移動」の順に並ぶ。
enum class PaneHeaderButton : uint8_t {
    None,
    Close,
    Refresh, // ファイルペインのみ
    Reveal,  // ファイルペインのみ
};

// ペインヘッダー内のボタン矩形を返す（ペインローカル座標）。None は渡さない。
inline D2D1_RECT_F PaneHeaderButtonRect(float pane_width, float header_height, PaneHeaderButton button) noexcept
{
    const float slot = static_cast<float>(std::to_underlying(button) - 1);
    const float btn_size = header_height - 2.0f * PANE_CLOSE_BTN_MARGIN;
    const float btn_x = pane_width - btn_size - PANE_CLOSE_BTN_MARGIN - slot * (btn_size + PANE_CLOSE_BTN_MARGIN);
    const float btn_y = (header_height - btn_size) / 2.0f;
    return D2D1::RectF(btn_x, btn_y, btn_x + btn_size, btn_y + btn_size);
}

// ペインヘッダーの見出し矩形（ペインローカル座標）。ウィンドウが狭くサイド幅が縮小されると
// ボタンだけで埋まり矩形が反転するため、その場合は nullopt (見出しを描かない)。
inline std::optional<D2D1_RECT_F> PaneHeaderTextRect(float pane_width, float header_height, bool with_file_buttons) noexcept
{
    constexpr float TEXT_LEFT = 8.0f;
    constexpr float BUTTON_GAP = 4.0f;
    const D2D1_RECT_F leftmost_button = PaneHeaderButtonRect(pane_width, header_height, with_file_buttons ? PaneHeaderButton::Reveal : PaneHeaderButton::Close);
    const float right = leftmost_button.left - BUTTON_GAP;
    if (right <= TEXT_LEFT) {
        return std::nullopt;
    }
    return D2D1::RectF(TEXT_LEFT, 0.0f, right, header_height);
}

// 描画とヒット判定でボタンの並びと有効かどうかを共有する。矩形はペインローカル座標。
template <typename Fn>
void ForEachPaneHeaderButton(float pane_width, float header_height, bool is_file_pane, bool reveal_enabled, Fn&& fn)
{
    const auto emit = [&](PaneHeaderButton button, bool enabled) { fn(PaneHeaderButtonRect(pane_width, header_height, button), button, enabled); };
    emit(PaneHeaderButton::Close, true);
    if (is_file_pane) {
        emit(PaneHeaderButton::Refresh, true);
        emit(PaneHeaderButton::Reveal, reveal_enabled);
    }
}

constexpr std::optional<PaneTarget> ToPaneTarget(PaneZone zone) noexcept
{
    switch (zone) {
    case PaneZone::FilePane:
        return PaneTarget::File;
    case PaneZone::TocPane:
        return PaneTarget::Toc;
    case PaneZone::None:
    case PaneZone::Splitter1:
    case PaneZone::Splitter2:
    case PaneZone::MdPane:
        return std::nullopt;
    }
    std::unreachable();
}

struct PaneLayout {
    PaneRect file_rect{};
    PaneRect toc_rect{};
    PaneRect md_rect{};
    // 隠したペインの矩形は空なので、ゾーン判定は表示中かをこちらで見る。
    bool show_file = false;
    bool show_toc = false;

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

// 縦スクロールバーを出すか。描画とヒット判定で共有する。may_grow (計測待ちで内容が伸びうる) の間は、
// ちょうど収まっていても出し続けてちらつきを防ぐ。それ以外はつまみを動かせるときだけ出す。
constexpr bool ShowsVScrollbar(const PaneScrollInfo& info, bool may_grow) noexcept
{
    if (info.content_height <= 0.0f) {
        return false;
    }
    if (may_grow) {
        return info.total_content >= info.content_height;
    }
    return info.thumb_height < info.content_height;
}

// サイドペイン (ファイル/TOC) の総コンテンツ高。スクロール計算・描画・自動スクロールの
// 全層がこの式を共有する (項目の可変高等を導入する際はここだけ変える)。
constexpr float SidePaneContentHeight(size_t item_count, float pane_item_height) noexcept
{
    return static_cast<float>(item_count) * pane_item_height;
}

// ヘッダーを除いた内容領域の上端 (ウィンドウ DIP)。
constexpr float PaneContentTop(const PaneRect& rect, float header_height) noexcept
{
    return rect.y + header_height;
}

// ウィンドウ DIP の Y を、項目先頭を 0 とするペイン内ローカル Y (スクロール込み) に変換する。
// ホバーとクリックの両方がこの式を共有する。
constexpr float SidePaneLocalY(float dip_y, float content_top, float scroll_y) noexcept
{
    return dip_y - content_top + scroll_y;
}

// 等高リストのペイン内ローカル Y から項目 index を返す。範囲外は -1。
constexpr int HitTestUniformList(float local_y, float item_height, size_t count) noexcept
{
    if (local_y < 0 || item_height <= 0) {
        return -1;
    }
    const int index = static_cast<int>(local_y / item_height);
    if (index < 0 || index >= static_cast<int>(count)) {
        return -1;
    }
    return index;
}

// 指定項目をペイン表示域の縦中央に置くスクロール位置。先頭/末尾付近は端にクランプする (issue#259)。
constexpr float CenterPaneScrollOnItem(size_t index, float item_height, const PaneScrollInfo& info) noexcept
{
    const float item_y = SidePaneContentHeight(index, item_height);
    return std::clamp(item_y - (info.content_height - item_height) * 0.5f, 0.0f, info.max_scroll);
}

// サイドペイン (ファイル一覧・目次) の表示状態・幅・スクロール・倍率。
class PaneController {
public:
    constexpr bool IsSidePaneVisible(PaneTarget t) const noexcept
    {
        return Inst(t).show;
    }
    constexpr void SetSidePaneVisible(PaneTarget t, bool v) noexcept
    {
        Inst(t).show = v;
    }
    constexpr void ToggleSidePane(PaneTarget t) noexcept
    {
        SetSidePaneVisible(t, !Inst(t).show);
    }

    constexpr float GetSidePaneWidth(PaneTarget t) const noexcept
    {
        return Inst(t).width;
    }
    constexpr void SetSidePaneWidth(PaneTarget t, float w) noexcept
    {
        Inst(t).width = std::max(w, SIDE_PANE_MIN_WIDTH);
    }

    constexpr auto& SidePaneScroll(this auto& self, PaneTarget t) noexcept
    {
        return self.Inst(t).scroll_y;
    }
    // 一覧の項目 (文字・行の高さ) だけに掛かる倍率。ヘッダーとペイン幅は変えない。
    constexpr auto& SidePaneZoom(this auto& self, PaneTarget t) noexcept
    {
        return self.Inst(t).zoom;
    }

    constexpr void ResetScrollStates() noexcept
    {
        for (auto& inst : instances_) {
            inst.scroll_y = 0.0f;
        }
    }

    // 変化した場合 true。
    bool ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept;

    // edge のペインの右端にあるスプリッタを dip_x へ動かす。
    void DragSplitterTo(PaneTarget edge, float dip_x, float total_width, float splitter_w) noexcept;

    // 左からファイル・目次・本文と並べる。top_offset はタイトルバーの高さ。
    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w, float top_offset) const noexcept;

private:
    struct Instance {
        float scroll_y = 0.0f;
        float width = SIDE_PANE_DEFAULT_WIDTH;
        ZoomLevel zoom;
        bool show = true;
    };
    Instance instances_[2];

    constexpr auto& Inst(this auto& self, PaneTarget t) noexcept
    {
        return self.instances_[static_cast<size_t>(t)];
    }
};

PaneZone DetectPaneZone(float dip_x, const PaneLayout& layout, float splitter_width) noexcept;
PaneScrollInfo ComputeScrollInfo(const PaneRect& rect, float header_height, float total_content) noexcept;
float ComputeThumbY(const PaneScrollInfo& info, float scroll_y) noexcept;
float ScrollFromThumbY(const PaneScrollInfo& info, float thumb_y) noexcept;

// 縦スクロールバーの左端 X。right_edge はペイン右端 (ローカル/スクリーンどちらの系でも可)。
// 描画 (command_generator) とヒットテスト (app_controller) で同じ式を共有する。
constexpr float VScrollbarLeftX(float right_edge) noexcept
{
    return right_edge - PANE_SCROLLBAR_WIDTH - PANE_SCROLLBAR_MARGIN;
}

// 縦スクロールバーのヒット判定の X 範囲。細いバーを掴みやすいよう左に広げ、右端の余白は外す
// (md ペインはウィンドウ右端に接するので、そこはリサイズ枠に譲る)。
constexpr bool InVScrollbarHitX(float x, float right_edge) noexcept
{
    return x >= VScrollbarLeftX(right_edge) - PANE_SCROLLBAR_HIT_PADDING && x <= right_edge - PANE_SCROLLBAR_MARGIN;
}

inline constexpr float MOUSE_WHEEL_SCROLL_MULTIPLIER = 0.8f;

inline constexpr float TOC_INDENT_PER_LEVEL = 12.0f;

// マークダウンペインの最小幅 (DIP)。スプリッタードラッグでこれ以上 md ペインを縮められない。
// PaneController の表示幅の縮小とスプリッタードラッグの制約で共有する。
inline constexpr float MD_PANE_MIN_WIDTH = 200.0f;
