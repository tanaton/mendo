#include "command_generator.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "hit_test_service.h"
#include "i18n.h"
#include "layout_computer.h"
#include "pane_layout.h"
#include "profiler.h"
#include "ui_constants.h"
#include "utf8_codec.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <utility>

using layout::NodeBoxPadBottom;
using layout::NodeBoxPadTop;
using layout::NodeContentWidth;
using layout::NodeIndent;
using layout::NodeTextXOffset;

#ifdef MENDO_USE_TRACY
namespace {

struct CmdGenStats {
    int64_t hittest_range = 0;
    int64_t sel_hl_cache_hit = 0;
    int64_t sel_hl_cache_miss = 0;
    int64_t search_hl_reset = 0;
    int64_t search_hl_computed = 0;
    int64_t search_hl_provisional = 0;
    int64_t last_visible_node_count = 0;
};
CmdGenStats g_cmd_gen_stats;

void PublishCmdGenStats() noexcept
{
    MENDO_PLOT("cmdgen.hittest_range", g_cmd_gen_stats.hittest_range);
    MENDO_PLOT("cmdgen.sel_hl_cache_hit", g_cmd_gen_stats.sel_hl_cache_hit);
    MENDO_PLOT("cmdgen.sel_hl_cache_miss", g_cmd_gen_stats.sel_hl_cache_miss);
    MENDO_PLOT("cmdgen.search_hl_reset", g_cmd_gen_stats.search_hl_reset);
    MENDO_PLOT("cmdgen.search_hl_computed", g_cmd_gen_stats.search_hl_computed);
    MENDO_PLOT("cmdgen.search_hl_provisional", g_cmd_gen_stats.search_hl_provisional);
    MENDO_PLOT("cmdgen.visible_node_count", g_cmd_gen_stats.last_visible_node_count);
}

} // namespace
#endif

namespace {

// 可視 Y 範囲 (ペインローカル) と重なるか。
constexpr bool OverlapsY(const D2D1_RECT_F& r, YRange visible) noexcept
{
    return !IsOffscreen(r.top, r.bottom - r.top, visible.top, visible.bottom);
}

// 可視範囲より高いレイアウトだけ、executor に範囲外のグリフランを飛ばさせる。
DrawTextLayoutCmd MakeTextLayoutCmd(D2D1_POINT_2F origin, IDWriteTextLayout* layout, float height, BrushId brush_id, YRange visible) noexcept
{
    DrawTextLayoutCmd cmd{ origin, layout, brush_id };
    if (height > visible.bottom - visible.top) {
        cmd.cull_top = visible.top;
        cmd.cull_bottom = visible.bottom;
    }
    return cmd;
}

FillRoundedRectCmd MakeInlineCodeBgCmd(const D2D1_RECT_F& r) noexcept
{
    return { r, INLINE_CODE_CORNER, INLINE_CODE_CORNER, BrushId::InlineCodeBg };
}

// bgs はレイアウト原点相対矩形。
void GenInlineCodeBgs(DrawCommandList& cmds, std::span<const InlineCodeBg> bgs, float origin_x, float origin_y, YRange visible)
{
    for (const auto& bg : bgs) {
        const auto r = OffsetRectF(bg, origin_x, origin_y);
        if (OverlapsY(r, visible)) {
            cmds.emplace_back(MakeInlineCodeBgCmd(r));
        }
    }
}

// row_bgs は列の昇順なので、col 以下の分を先頭から消費し col のセルの背景だけを描く。
// 列を昇順に渡す限り、行全体で O(背景数) で済む。
void GenCellInlineCodeBgs(DrawCommandList& cmds, std::span<const CellInlineCodeBg>& row_bgs, size_t col, float origin_x, float origin_y)
{
    while (!row_bgs.empty() && row_bgs.front().col <= col) {
        if (row_bgs.front().col == col) {
            cmds.emplace_back(MakeInlineCodeBgCmd(OffsetRectF(row_bgs.front().rect, origin_x, origin_y)));
        }
        row_bgs = row_bgs.subspan(1);
    }
}

// h1/h2 見出し下線を描画するY座標を heading_spacing_below_h1h2 のこの比率で決める。
// 値を小さくすると下線は見出し文字に近づき、下線と次行の間隔が広がる。
constexpr float HEADING_UNDERLINE_OFFSET_RATIO = 0.25f;

// 見出し下端から下線までの距離。
constexpr float HeadingUnderlineGap(const Theme& theme) noexcept
{
    return theme.heading_spacing_below_h1h2 * HEADING_UNDERLINE_OFFSET_RATIO;
}

// 引用バーはグループの上下へ、アラート背景は四辺へこの分はみ出す。
constexpr float BAR_EXTEND = 2.0f;
constexpr float ALERT_BG_PAD = 4.0f;

// AlertBrushIdFromIndex (brush_id.h) の連番が AlertType 全種を網羅することを担保する。
static_assert(std::to_underlying(BrushId::AlertCaution) - std::to_underlying(BrushId::AlertNote) + 1 == ALERT_TYPE_COUNT,
              "AlertBrushIdFromIndex must cover every AlertType");

// ブロックローカル横スクロールの clip と Translate を RAII で囲み、push/pop の対称性を強制する。
// scroll_x == 0 のときは Translate を省略し、ブロックがスクロールしないときは Clip も省略する。
// スクロールできるが scroll_x == 0 の場合 (初期状態) は、依然として右端で
// Clip しないとペイン余白に内容がはみ出るため、Clip と Translate を独立に判定する。
class BlockHScrollScope {
public:
    BlockHScrollScope(DrawCommandList& cmds, const D2D1_RECT_F& clip,
                      const D2D1::Matrix3x2F& base_transform, float scroll_x, bool needs_clip)
        : cmds_(cmds), restore_(base_transform),
          push_clip_(needs_clip), translate_(needs_clip && scroll_x > 0.0f)
    {
        if (push_clip_) {
            cmds_.emplace_back(PushClipCmd{ clip });
        }
        if (translate_) {
            cmds_.emplace_back(SetTransformCmd{ base_transform * D2D1::Matrix3x2F::Translation(-scroll_x, 0) });
        }
    }
    ~BlockHScrollScope()
    {
        if (translate_) {
            cmds_.emplace_back(SetTransformCmd{ restore_ });
        }
        if (push_clip_) {
            cmds_.emplace_back(PopClipCmd{});
        }
    }
    BlockHScrollScope(const BlockHScrollScope&) = delete;
    BlockHScrollScope& operator=(const BlockHScrollScope&) = delete;

private:
    DrawCommandList& cmds_;
    D2D1::Matrix3x2F restore_;
    bool push_clip_;
    bool translate_;
};

// MeasureNode 時に確定済みの first_line_height を優先利用する。
// 0（未確定）なら DirectWrite に問い合わせず font_size ベースのフォールバックで済ませ、
// 描画ホットパスから COM 越境呼び出しを排除する。
float GetFirstLineHeight(const NodeLayoutEntry& entry, float font_size) noexcept
{
    return (entry.first_line_height() > 0.0f) ? entry.first_line_height() : font_size * FALLBACK_LINE_HEIGHT_FACTOR;
}

// 失敗していなければ空。
std::wstring_view DiagramErrorText(const DiagramError& error) noexcept
{
    switch (error.kind) {
    case DiagramError::Kind::None:
        return {};
    case DiagramError::Kind::Render:
        return error.message.empty() ? i18n::S().diagram_error : std::wstring_view{ error.message };
    case DiagramError::Kind::TooLarge:
        return i18n::S().diagram_too_large;
    }
    return {};
}

// リストの記号と番号の書式は 3 種類を段ごとに繰り返す。GitHub (ブラウザ既定) は 3 段目より深くても 3 種類目のままだが、
// 深い段も見分けられるようにする。段を持たない (0 以下) ときは最外側として扱う。
constexpr int ListStyleIndex(int list_depth) noexcept
{
    return (std::max(list_depth, 1) - 1) % 3;
}

// ボタンの背景。brush が None なら塗らない。角の半径が 0 なら矩形で塗る (不透明度は掛けない)。
struct ButtonBg {
    BrushId brush = BrushId::None;
    float corner = 0.0f;
    float opacity = 1.0f;
};

// アイコンや短い文字列を rect の中央に置くボタン。書式は左右・上下とも中央揃え。text_layouts が無ければ文字は描かない。
void EmitButton(DrawCommandList& out, TextLayoutCache* text_layouts, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, std::wstring_view label,
                ButtonBg bg, BrushId fg, float fg_opacity)
{
    if (bg.brush != BrushId::None) {
        if (bg.corner > 0.0f) {
            out.push_back(FillRoundedRectCmd{ .rect = rect, .rx = bg.corner, .ry = bg.corner, .brush_id = bg.brush, .opacity = bg.opacity });
        }
        else {
            out.push_back(FillRectCmd{ .rect = rect, .brush_id = bg.brush });
        }
    }
    if (text_layouts) {
        text_layouts->EmitCentered(out, label, fmt, rect, fg, fg_opacity);
    }
}

} // namespace

std::wstring_view FormatListNumber(std::span<wchar_t, kListNumberMaxLen> buf, int32_t number, int list_depth) noexcept
{
    const int style = ListStyleIndex(list_depth);
    size_t len = 0;
    if (style == 1 && number >= 1 && number <= 3999) {
        static constexpr std::pair<int32_t, std::wstring_view> kRoman[] = {
            { 1000, L"m"  },
            { 900,  L"cm" },
            { 500,  L"d"  },
            { 400,  L"cd" },
            { 100,  L"c"  },
            { 90,   L"xc" },
            { 50,   L"l"  },
            { 40,   L"xl" },
            { 10,   L"x"  },
            { 9,    L"ix" },
            { 5,    L"v"  },
            { 4,    L"iv" },
            { 1,    L"i"  },
        };
        for (int32_t rest = number; const auto& [value, digits] : kRoman) {
            for (; rest >= value; rest -= value) {
                for (const wchar_t ch : digits) {
                    buf[len++] = ch;
                }
            }
        }
    }
    else if (style == 2 && number >= 1) {
        // a..z の次は aa (0 を持たない 26 進)。下の桁から書いて反転する。
        for (int32_t rest = number; rest > 0; rest = (rest - 1) / 26) {
            buf[len++] = static_cast<wchar_t>(L'a' + (rest - 1) % 26);
        }
        std::ranges::reverse(buf.first(len));
    }
    else {
        len = static_cast<size_t>(std::format_to_n(buf.data(), buf.size() - 1, L"{}", number).size);
    }
    buf[len++] = L'.';
    return { buf.data(), len };
}

// つまみはペイン右端から VScrollbarLeftX で内側に寄せる。
std::optional<FillRoundedRectCmd> VScrollThumb(const PaneRect& pane, float header_height, float scroll_y, float total_content_height, bool may_grow) noexcept
{
    const PaneScrollInfo info = ComputeScrollInfo(pane, header_height, total_content_height);
    if (!ShowsVScrollbar(info, may_grow)) {
        return std::nullopt;
    }
    const float x = VScrollbarLeftX(pane.x + pane.width);
    const float thumb_y = ComputeThumbY(info, scroll_y);
    constexpr float radius = PANE_SCROLLBAR_WIDTH / 2.0f;
    return FillRoundedRectCmd{
        .rect = D2D1::RectF(x, thumb_y, x + PANE_SCROLLBAR_WIDTH, thumb_y + info.thumb_height), .rx = radius, .ry = radius, .brush_id = BrushId::ScrollbarThumb
    };
}

SearchInputMetrics SearchInputLayout::Measure(const SearchBarRenderState& sb, float width)
{
    const bool has_comp = !sb.ime_composition.empty();
    if (!factory_ || !fmt_ || (sb.query.empty() && !has_comp)) {
        return {};
    }

    const int comp_len = static_cast<int>(sb.ime_composition.size());
    int comp_start = 0;
    if (has_comp) {
        const int qlen = static_cast<int>(sb.query.size());
        comp_start = sb.caret_pos;
        if (comp_start < 0 || comp_start > qlen) {
            comp_start = qlen;
        }
    }

    // IMEコンポジション中は確定済みテキストのキャレット位置に変換中テキストを挿入して表示する。
    // 下線はレイアウト生成時にだけ付けるので、合成範囲もキーに含める。
    scratch_.assign(sb.query);
    scratch_.insert(static_cast<size_t>(comp_start), sb.ime_composition);
    const DWRITE_TEXT_RANGE underline{ static_cast<UINT32>(comp_start), static_cast<UINT32>(comp_len) };
    if (!Holds(scratch_, underline, width)) {
        layout_ = Create(scratch_, width);
        if (!layout_) {
            Forget();
            return {};
        }
        if (underline.length > 0) {
            layout_->SetUnderline(TRUE, underline);
        }
        text_.swap(scratch_);
        width_ = width;
        underline_ = underline;
    }
    SearchInputMetrics m{ .layout = layout_.Get() };

    const int text_len = static_cast<int>(text_.size());
    const bool has_selection = !has_comp && sb.selection_start >= 0 && sb.caret_pos >= 0 && sb.selection_start != sb.caret_pos;
    if (has_selection) {
        const int sel_min = std::clamp(std::min(sb.selection_start, sb.caret_pos), 0, text_len);
        const int sel_max = std::clamp(std::max(sb.selection_start, sb.caret_pos), 0, text_len);
        if (sel_min < sel_max) {
            // 単一行テキストなのでメトリクスは1つで十分
            DWRITE_HIT_TEST_METRICS htm_sel{};
            UINT32 actual = 0;
            layout_->HitTestTextRange(static_cast<UINT32>(sel_min), static_cast<UINT32>(sel_max - sel_min), 0.0f, 0.0f, &htm_sel, 1, &actual);
            if (actual > 0) {
                m.selection_x = htm_sel.left;
                m.selection_width = htm_sel.width;
            }
        }
    }

    // コンポジション中はその末尾、それ以外は通常のキャレット位置
    int effective_pos = has_comp ? comp_start + comp_len : sb.caret_pos;
    if (!has_comp && (effective_pos < 0 || effective_pos > text_len)) {
        effective_pos = text_len;
    }
    FLOAT px, py;
    DWRITE_HIT_TEST_METRICS htm{};
    layout_->HitTestTextPosition(static_cast<UINT32>(effective_pos), false, &px, &py, &htm);
    m.caret_x = px;
    return m;
}

int SearchInputLayout::HitTest(std::wstring_view query, float local_x, float width) const
{
    if (query.empty() || !factory_ || !fmt_) {
        return 0;
    }
    // IME 合成中は表示テキストが query と異なるので作り直す。
    const auto layout = Holds(query, {}, width) ? layout_ : Create(query, width);
    if (!layout) {
        return 0;
    }
    // HitTestPoint が失敗時に out 引数未書込みでも未初期化値を読まないようにする。
    BOOL is_trailing = FALSE;
    BOOL is_inside = FALSE;
    DWRITE_HIT_TEST_METRICS htm{};
    if (FAILED(layout->HitTestPoint(local_x, 0.0f, &is_trailing, &is_inside, &htm))) {
        return 0;
    }
    // 結合文字やサロゲートペアの途中に置かないよう、クラスタの長さ分進める。
    return static_cast<int>(htm.textPosition + (is_trailing ? htm.length : 0));
}

Microsoft::WRL::ComPtr<IDWriteTextLayout> SearchInputLayout::Create(std::wstring_view text, float width) const
{
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt_.Get(), width, SEARCH_INPUT_HEIGHT, &layout))) {
        return nullptr;
    }
    return layout;
}

namespace overlay {

void EmitLoadingSpinner(DrawCommandList& out, float angle, const PaneRect& md_pane_rect)
{
    // 不透明度は点の番号だけで決まる。
    static constexpr auto kAlphas = []() static noexcept {
        std::array<float, spinner::DOT_COUNT> a{};
        for (int i = 0; i < spinner::DOT_COUNT; ++i) {
            a[i] = 1.0f - i * (spinner::DOT_FADE_FACTOR / spinner::DOT_COUNT);
        }
        return a;
    }();
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    for (int i = 0; i < spinner::DOT_COUNT; i++) {
        const float a = angle - i * (TWO_PI / spinner::DOT_COUNT);
        out.push_back(FillEllipseCmd{
            .center = D2D1::Point2F(cx + spinner::RADIUS * std::cos(a), cy + spinner::RADIUS * std::sin(a)),
            .rx = spinner::DOT_RADIUS,
            .ry = spinner::DOT_RADIUS,
            .brush_id = BrushId::Text,
            .opacity = kAlphas[i],
        });
    }
}

void EmitNavButtons(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect,
                    bool dark, bool can_back, bool can_forward, NavDirection hovered)
{
    // 色を差し替えず、白黒の固定ブラシに不透明度を掛ける。
    const auto emit_button = [&](const D2D1_RECT_F& rect, bool enabled, bool is_hovered, std::wstring_view arrow) {
        const float bg_alpha = !enabled ? (dark ? 0.08f : 0.05f) : is_hovered ? (dark ? 0.35f : 0.25f) : (dark ? 0.15f : 0.10f);
        const float text_alpha = !enabled ? (dark ? 0.2f : 0.15f) : is_hovered ? 1.0f : (dark ? 0.6f : 0.5f);
        EmitButton(out, &text_layouts, fmt, rect, arrow, { BrushId::Overlay, NAV_BTN_CORNER, bg_alpha }, BrushId::Overlay, text_alpha);
    };
    // クリック判定 (NavButtonHitTest) と同じ矩形 API を使い、描画とヒットのズレを防ぐ
    emit_button(NavBackButtonRect(md_pane_rect), can_back, hovered == NavDirection::Back, L"\x25C0");
    emit_button(NavForwardButtonRect(md_pane_rect), can_forward, hovered == NavDirection::Forward, L"\x25B6");
}

namespace {
// ジェスチャー/トーストの背景パネルと文言。
// box は rect の大きさ。矩形の差から求めると丸めで値が揺れ、文字レイアウトのキャッシュが外れる。
void EmitPanel(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, D2D1_SIZE_F box,
               float corner, float alpha, std::wstring_view text)
{
    out.push_back(FillRoundedRectCmd{ .rect = rect, .rx = corner, .ry = corner, .brush_id = BrushId::OverlayGestureBg, .opacity = alpha });
    text_layouts.Emit(out, text, fmt, D2D1::Point2F(rect.left, rect.top), box, BrushId::OverlayWhite, 1.0f);
}
} // namespace

void EmitGestureOverlay(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect,
                        bool dark, NavDirection direction)
{
    if (direction == NavDirection::None) {
        return;
    }
    constexpr float w = GESTURE_OVERLAY_WIDTH;
    constexpr float h = GESTURE_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    const auto& text = (direction == NavDirection::Back) ? i18n::S().gesture_back : i18n::S().gesture_forward;
    EmitPanel(out, text_layouts, fmt, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), D2D1::SizeF(w, h),
              GESTURE_OVERLAY_CORNER, dark ? 0.8f : 0.6f, text);
}

void EmitToast(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect,
               bool dark, std::wstring_view message)
{
    if (message.empty()) {
        return;
    }
    constexpr float w = TOAST_OVERLAY_WIDTH;
    constexpr float h = TOAST_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float bottom_y = NavBackButtonRect(md_pane_rect).top - TOAST_OVERLAY_BOTTOM_OFFSET;
    EmitPanel(out, text_layouts, fmt, D2D1::RectF(cx - w / 2, bottom_y - h, cx + w / 2, bottom_y), D2D1::SizeF(w, h),
              TOAST_OVERLAY_CORNER, dark ? 0.85f : 0.7f, message);
}

void EmitSearchBar(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* count_fmt,
                   const SearchBarLayout& sbl, const SearchBarRenderState& sb, const SearchInputMetrics& input)
{
    const D2D1_RECT_F& bar_rect = sbl.bar_rect;
    out.push_back(FillRectCmd{ .rect = bar_rect, .brush_id = BrushId::SearchBarBg });
    out.push_back(DrawLineCmd{
        .p0 = D2D1::Point2F(bar_rect.left, bar_rect.top), .p1 = D2D1::Point2F(bar_rect.right, bar_rect.top), .stroke_width = 1.0f, .brush_id = BrushId::SearchBarBorder });

    text_layouts.EmitCentered(out, L"\uE721", icon_fmt, sbl.icon_rect, BrushId::SearchInputText, 0.6f);

    const bool no_match = !sb.query.empty() && sb.total_matches == 0;
    out.push_back(FillRoundedRectCmd{
        .rect = sbl.input_rect, .rx = SEARCH_BAR_CORNER, .ry = SEARCH_BAR_CORNER, .brush_id = no_match ? BrushId::SearchNoMatchBg : BrushId::SearchInputBg });
    // 表示中は入力欄が常にフォーカスを持つので、フォーカス枠で描く。
    out.push_back(DrawRoundedRectCmd{ .rect = sbl.input_rect, .rx = SEARCH_BAR_CORNER, .ry = SEARCH_BAR_CORNER, .stroke_width = 1.5f, .brush_id = BrushId::Link });

    const float text_left = sbl.text_left();
    if (input.layout) {
        // 文字を隠さないよう、選択範囲はテキストより先に塗る。
        if (input.selection_width > 0.0f) {
            const float sel_left = text_left + input.selection_x;
            out.push_back(FillRectCmd{
                .rect = D2D1::RectF(sel_left, sbl.input_rect.top + 2.0f, sel_left + input.selection_width, sbl.input_rect.bottom - 2.0f),
                .brush_id = BrushId::Selection,
            });
        }
        // IME の合成範囲の下線は executor (VisibleRunRenderer::DrawUnderline) が同じブラシで描く。
        out.push_back(DrawTextLayoutCmd{
            .origin = D2D1::Point2F(text_left, sbl.input_rect.top),
            .layout = input.layout,
            .brush_id = BrushId::SearchInputText,
        });
    }

    // コンポジション中はIME側がキャレットを表示するため非表示
    if (sb.caret_visible && sb.ime_composition.empty()) {
        const float x = std::min(text_left + input.caret_x + 1.0f, sbl.text_right());
        out.push_back(DrawLineCmd{
            .p0 = D2D1::Point2F(x, sbl.input_rect.top + 3.0f),
            .p1 = D2D1::Point2F(x, sbl.input_rect.bottom - 3.0f),
            .stroke_width = 1.0f,
            .brush_id = BrushId::SearchInputText,
        });
    }

    const auto draw_icon_btn = [&](const D2D1_RECT_F& r, std::wstring_view icon, bool hovered, float alpha = 1.0f) {
        const ButtonBg bg{ hovered ? BrushId::TitleBarButtonHover : BrushId::None, SEARCH_BAR_CORNER };
        EmitButton(out, &text_layouts, icon_fmt, r, icon, bg, BrushId::SearchInputText, alpha);
    };

    const auto draw_toggle_btn = [&](const D2D1_RECT_F& r, std::wstring_view label, IDWriteTextFormat* fmt, bool checked, bool hovered) {
        const BrushId bg = checked ? BrushId::TitleBarButtonActive : hovered ? BrushId::TitleBarButtonHover : BrushId::None;
        EmitButton(out, &text_layouts, fmt, r, label, { bg, SEARCH_BAR_CORNER }, BrushId::SearchInputText, checked ? 1.0f : 0.5f);
    };

    const float nav_alpha = sb.total_matches > 0 ? 1.0f : 0.3f;
    draw_icon_btn(sbl.up_btn, L"\uE70E", sb.hovered == SearchBarHitZone::Up, nav_alpha);
    draw_icon_btn(sbl.down_btn, L"\uE70D", sb.hovered == SearchBarHitZone::Down, nav_alpha);

    if (count_fmt && !sb.query.empty() && sb.total_matches >= 0) {
        // 「N / M」形式で表示。M は実用上 4-5 桁に収まる (md 内 hit 数) ので
        // 32 wchar_t は十分な余裕。format_to_n で先端から書き、長さを返してもらう。
        constexpr size_t kCountBufLen = 32;
        wchar_t count_text[kCountBufLen];
        const auto r = (sb.total_matches == 0)
                           ? std::format_to_n(count_text, kCountBufLen - 1, L"0")
                           : std::format_to_n(count_text, kCountBufLen - 1, L"{} / {}", sb.current_match + 1, sb.total_matches);
        const auto written = static_cast<size_t>(r.out - count_text);
        text_layouts.EmitCentered(out, { count_text, written }, count_fmt, sbl.count_rect, BrushId::SearchInputText, 0.7f);
    }

    draw_toggle_btn(sbl.case_btn, L"Aa", count_fmt, sb.case_sensitive, sb.hovered == SearchBarHitZone::CaseSensitive);
    draw_toggle_btn(sbl.highlight_btn, L"\uE7E6", icon_fmt, sb.highlight_enabled, sb.hovered == SearchBarHitZone::Highlight);
    draw_icon_btn(sbl.close_btn, L"\uE8BB", sb.hovered == SearchBarHitZone::Close);
}

} // namespace overlay

namespace chrome {

void EmitTitleBar(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* title_fmt,
                  ID2D1Bitmap* app_icon, bool dark, const TitleBarRenderState& tb)
{
    const TitleBar& bar = tb.bar;
    // 完全不透明でガラス効果を隠す
    out.push_back(FillRectCmd{ .rect = D2D1::RectF(0.0f, 0.0f, tb.window_width, TitleBar::BASE_HEIGHT), .brush_id = BrushId::TitleBarBg });

    const float text_alpha = tb.window_active ? 1.0f : 0.5f;
    const auto is_hovered = [&](TitleBarHitZone z) noexcept { return tb.hovered_zone == z; };

    const auto gen_button = [&](const D2D1_RECT_F& rect, wchar_t icon, bool show_bg, BrushId bg_id, BrushId text_id, float alpha) {
        EmitButton(out, &text_layouts, icon_fmt, rect, { &icon, 1 }, { show_bg ? bg_id : BrushId::None }, text_id, alpha);
    };

    const auto gen_plain = [&](TitleBarHitZone zone, wchar_t icon) {
        gen_button(bar.GetButton(zone), icon, is_hovered(zone), BrushId::TitleBarButtonHover, BrushId::TitleBarText, text_alpha);
    };
    // active > hover の優先度
    const auto gen_toggle = [&](TitleBarHitZone zone, wchar_t icon, bool active) {
        gen_button(
            bar.GetButton(zone),
            icon,
            active || is_hovered(zone),
            active ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
            BrushId::TitleBarText,
            text_alpha);
    };

    gen_plain(TitleBarHitZone::OpenFile, L'');
    gen_plain(TitleBarHitZone::Help, L'');
    gen_plain(TitleBarHitZone::Settings, L'');
    gen_plain(TitleBarHitZone::ThemeToggle, dark ? L'' : L'');
    gen_toggle(TitleBarHitZone::Search, L'', tb.search_active);
    gen_toggle(TitleBarHitZone::FileToggle, L'', tb.file_pane_visible);
    gen_toggle(TitleBarHitZone::TocToggle, L'', tb.toc_pane_visible);
    gen_plain(TitleBarHitZone::Minimize, L'');
    gen_plain(TitleBarHitZone::Maximize, tb.is_maximized ? L'' : L'');
    if (is_hovered(TitleBarHitZone::Close)) {
        gen_button(bar.GetButton(TitleBarHitZone::Close), L'', true, BrushId::TitleBarCloseRed, BrushId::OverlayWhite, 1.0f);
    }
    else {
        gen_plain(TitleBarHitZone::Close, L'');
    }

    if (app_icon) {
        out.push_back(DrawBitmapCmd{ .bitmap = app_icon, .dest = TitleBar::ICON_RECT, .opacity = text_alpha });
    }

    if (!tb.title_text.empty()) {
        text_layouts.EmitCentered(out, tb.title_text, title_fmt, bar.GetTitleTextRect(), BrushId::TitleBarText, text_alpha);
    }
}

namespace {

// 目次の行は 1 行で狭い。極端に長い見出しでも毎フレーム全文を UTF-16 化・ハッシュしないよう先頭だけ使う。
std::string_view TocItemText(std::string_view heading) noexcept
{
    constexpr uint32_t kMaxBytes = 512;
    return heading.size() <= kMaxBytes ? heading : heading.substr(0, utf8_codec::SnapToCpStart(heading, kMaxBytes));
}

const wchar_t* FileEntryIcon(const FileEntry& entry) noexcept
{
    if (entry.is_parent) {
        return L"";
    }
    if (entry.is_directory) {
        return L"";
    }
    return L"";
}

// NO_WRAP の左寄せテキスト。はみ出しは呼び出し側のクリップで切る。
void EmitLeftText(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, std::wstring_view text, float x, float y, float height)
{
    // 箱の幅で見た目が変わらないので、幅を固定してスプリッタのドラッグ中もレイアウトを使い回す。
    constexpr float kLayoutWidth = 4096.0f;
    text_layouts.Emit(out, text, fmt, D2D1::Point2F(x, y), D2D1::SizeF(kLayoutWidth, height), BrushId::Text, 1.0f);
}

void EmitSidePaneHeader(DrawCommandList& out, TextLayoutCache& text_layouts, const SidePaneFormats& fmts, float header_h,
                        const SidePaneInstance& pane, bool is_file_pane, std::wstring_view header_text)
{
    const PaneRect& r = pane.rect;
    out.push_back(FillRectCmd{ .rect = D2D1::RectF(r.x, r.y, r.x + r.width, r.y + header_h), .brush_id = BrushId::Splitter });

    // 無効なボタンはホバー状態が残っていても強調しない (ヘルプへ切替直後など)。
    const auto gen_button = [&](const D2D1_RECT_F& local, std::wstring_view icon, PaneHeaderButton button, bool enabled = true) {
        const bool hovered = enabled && pane.hovered_button == button;
        EmitButton(out, &text_layouts, fmts.header_icon, OffsetRectF(local, r.x, r.y), icon, { hovered ? BrushId::PaneItemHover : BrushId::None },
                   BrushId::Text, enabled ? 1.0f : DISABLED_UI_ALPHA);
    };

    gen_button(PaneCloseButtonRect(r.width, header_h), L"", PaneHeaderButton::Close);
    if (is_file_pane) {
        gen_button(PaneRefreshButtonRect(r.width, header_h), L"", PaneHeaderButton::Refresh);
        gen_button(PaneRevealButtonRect(r.width, header_h), L"", PaneHeaderButton::Reveal, pane.reveal_enabled);
    }

    // ボタンに重ならないよう切る。
    if (const auto text_rect = PaneHeaderTextRect(r.width, header_h, is_file_pane)) {
        const D2D1_RECT_F clip = OffsetRectF(*text_rect, r.x, r.y);
        out.push_back(PushClipCmd{ clip });
        EmitLeftText(out, text_layouts, fmts.header, header_text, clip.left, clip.top, text_rect->bottom - text_rect->top);
        out.push_back(PopClipCmd{});
    }
}

// 項目のはみ出しは内容領域のクリップ 1 回で切り、項目ごとの Push/Pop を避ける。
template <typename GenItemFn>
void EmitSidePane(DrawCommandList& out, TextLayoutCache& text_layouts, const SidePaneFormats& fmts, float header_h,
                  const SidePaneInstance& pane, bool is_file_pane, size_t item_count, std::wstring_view header_text, GenItemFn gen_item)
{
    const PaneRect& r = pane.rect;
    if (r.width <= 0 || r.height <= 0) {
        return;
    }
    out.push_back(FillRectCmd{ .rect = ToD2DRect(r), .brush_id = BrushId::PaneBg });

    EmitSidePaneHeader(out, text_layouts, fmts, header_h, pane, is_file_pane, header_text);

    const float item_h = pane.item_height;
    const float content_top = PaneContentTop(r, header_h);
    const float scroll_y = pane.scroll.scroll_y;
    out.push_back(PushClipCmd{ D2D1::RectF(r.x, content_top, r.x + r.width, r.y + r.height) });

    const int first = std::max(0, static_cast<int>(scroll_y / item_h));
    const int last = std::min(static_cast<int>(item_count) - 1, static_cast<int>((scroll_y + r.height - header_h) / item_h) + 1);
    for (int i = first; i <= last; i++) {
        const float item_y = content_top + SidePaneContentHeight(i, item_h) - scroll_y;
        gen_item(i, D2D1::RectF(r.x, item_y, r.x + r.width, item_y + item_h));
    }

    out.push_back(PopClipCmd{});
    if (const auto thumb = VScrollThumb(r, header_h, scroll_y, SidePaneContentHeight(item_count, item_h))) {
        out.push_back(*thumb);
    }
}

void EmitFileExplorer(DrawCommandList& out, TextLayoutCache& text_layouts, const SidePaneFormats& fmts, float header_h,
                      const std::vector<FileEntry>& entries, const SidePaneInstance& pane)
{
    const auto k = std::to_underlying(PaneTarget::File);
    const float icon_col_width = 24.0f * pane.zoom;
    const float pad = 4.0f * pane.zoom;
    EmitSidePane(out, text_layouts, fmts, header_h, pane, true, entries.size(), i18n::S().pane_header_files, [&](int i, const D2D1_RECT_F& row) {
        const auto& entry = entries[i];

        if (entry.is_current) {
            out.push_back(FillRectCmd{ .rect = row, .brush_id = BrushId::PaneItemActive });
        }
        else if (i == pane.hovered_index) {
            out.push_back(FillRectCmd{ .rect = row, .brush_id = BrushId::PaneItemHover });
        }

        text_layouts.Emit(out, FileEntryIcon(entry), fmts.file_icon, D2D1::Point2F(row.left + pad, row.top), D2D1::SizeF(icon_col_width, pane.item_height), BrushId::Text, 1.0f);

        EmitLeftText(out, text_layouts, fmts.item[k], entry.GetDisplayName(), row.left + pad + icon_col_width, row.top, pane.item_height);
    });
}

void EmitToc(DrawCommandList& out, TextLayoutCache& text_layouts, const SidePaneFormats& fmts, float header_h,
             const SidePaneState& sp, const SidePaneInstance& pane, std::vector<std::wstring>& toc_titles)
{
    const auto& entries = sp.toc_entries;
    const int active_index = sp.active_toc_index;
    if (toc_titles.size() != entries.size()) {
        toc_titles.assign(entries.size(), {});
    }
    IDWriteTextFormat* const item_fmt = fmts.item[std::to_underlying(PaneTarget::Toc)];
    EmitSidePane(out, text_layouts, fmts, header_h, pane, false, entries.size(), i18n::S().pane_header_toc, [&](int i, const D2D1_RECT_F& row) {
        const Node& heading = sp.nodes[entries[i].node_index];

        if (i == active_index || i == pane.hovered_index) {
            const auto bid = (i == active_index) ? BrushId::PaneItemActive : BrushId::PaneItemHover;
            out.push_back(FillRectCmd{ .rect = row, .brush_id = bid });
        }

        const float text_left = row.left + (8.0f + (heading.heading_level - 1) * TOC_INDENT_PER_LEVEL) * pane.zoom;
        auto& title = toc_titles[i];
        if (title.empty()) {
            DecodeForDWrite(TocItemText(heading.GetText()), title);
        }
        EmitLeftText(out, text_layouts, item_fmt, title, text_left, row.top, pane.item_height);

        if (i == active_index) {
            const float line_y = row.bottom - 1.0f;
            out.push_back(DrawLineCmd{
                .p0 = D2D1::Point2F(text_left, line_y),
                .p1 = D2D1::Point2F(row.right - 4.0f, line_y),
                .stroke_width = 1.5f,
                .brush_id = BrushId::Text,
            });
        }
    });
}

} // namespace

void EmitSidePanes(DrawCommandList& out, TextLayoutCache& text_layouts, const SidePaneFormats& fmts, const Theme& theme,
                   const SidePaneState& sp, std::vector<std::wstring>& toc_titles)
{
    const float header_h = theme.pane_header_height;
    const auto gen_splitter = [&](const PaneRect& r) {
        const float x = r.x + r.width;
        out.push_back(FillRectCmd{ .rect = D2D1::RectF(x, r.y, x + theme.splitter_width, r.y + r.height), .brush_id = BrushId::Splitter });
    };
    if (const auto& fp = sp.Get(PaneTarget::File); fp.show) {
        EmitFileExplorer(out, text_layouts, fmts, header_h, sp.file_entries, fp);
        gen_splitter(fp.rect);
    }
    if (const auto& tp = sp.Get(PaneTarget::Toc); tp.show) {
        EmitToc(out, text_layouts, fmts, header_h, sp, tp, toc_titles);
        gen_splitter(tp.rect);
    }
}

} // namespace chrome

void CommandGenerator::EmitText(std::wstring_view text, IDWriteTextFormat* fmt, D2D1_POINT_2F origin, D2D1_SIZE_F box, BrushId brush_id, float opacity)
{
    if (text_layouts_) {
        text_layouts_->Emit(*cmds_, text, fmt, origin, box, brush_id, opacity);
    }
}

// SelectionHlCache は lazy 確保のみで自動破棄経路が無く、選択範囲外に出たノード分が
// 居残ってメモリが漸増する。前フレームの範囲との差分区間だけ巻き戻す
// (Ctrl+A 後のドラッグ等で範囲全体を毎回走査しない)。
void CommandGenerator::ReleaseStaleSelectionHlCaches(const LayoutCache& cache, const TextSelection& selection)
{
    const int new_start = selection.active() ? selection.start_node : -1;
    const int new_end = selection.active() ? selection.end_node : -1;
    if ((new_start != prev_sel_start_node_ || new_end != prev_sel_end_node_) && prev_sel_start_node_ >= 0) {
        const int lo = prev_sel_start_node_;
        const int hi = std::min(prev_sel_end_node_, static_cast<int>(cache.size()) - 1);
        const auto invalidate_range = [&](int first, int last) {
            for (int i = std::max(first, lo); i <= std::min(last, hi); i++) {
                cache[i].invalidate_selection_hl_cache();
            }
        };
        if (new_start < 0) {
            invalidate_range(lo, hi);
        }
        else {
            invalidate_range(lo, new_start - 1);
            invalidate_range(new_end + 1, hi);
        }
    }
    prev_sel_start_node_ = new_start;
    prev_sel_end_node_ = new_end;
}

void CommandGenerator::GenerateMdPane(DrawCommandList& out, const MdPaneParams& p, float dpi_scale)
{
    MENDO_PROFILE("GenerateMdPane");
    cmds_ = &out;
    const auto& md_pane_rect = p.md_pane_rect;
    const float scroll_y = p.scroll_y;
    const auto& nodes = p.nodes;
    const auto& cache = p.cache;

    cmds_->emplace_back(PushClipCmd{ ToD2DRect(md_pane_rect) });
    // スクロール位置を物理ピクセル境界にスナップし、ClearTypeヒンティングの
    // フレーム間変動によるテキストのガタつきを防止する。スナップは描画 Y のみに
    // 適用し、二分探索 (FindFirstVisibleNodeIndex) には未スナップ scroll_y を渡す。
    const float snapped_y = SnapToPhysicalPixel(scroll_y, dpi_scale);

    // スクロールの Y 平行移動は Transform に乗せず CPU 側で entry ごとに加算する。
    // 大規模ファイルで text_top/scroll_y が 10^7 DIP オーダーになると、D2D 内部の
    // float32 行列演算で catastrophic cancellation が起きるため。
    // ペイン原点は小さい定数なので Transform に乗せ、ローカル Y を [0, ペイン高さ] に揃えて
    // 可視範囲の判定と一致させる。原点を物理ピクセルに揃えておくと、ローカル座標の snap だけで
    // bullet やスクロールバーがピクセル境界に乗る。
    const auto pane_transform = D2D1::Matrix3x2F::Translation(
        SnapToPhysicalPixel(md_pane_rect.x, dpi_scale), SnapToPhysicalPixel(md_pane_rect.y, dpi_scale));
    cmds_->emplace_back(SetTransformCmd{ pane_transform });

    frame_ = {
        .params = &p,
        .content_width = theme_->ContentWidth(md_pane_rect.width),
        .dpi_scale = dpi_scale,
        .snapped_scroll_y = snapped_y,
        .visible = { 0.0f, md_pane_rect.height },
        .pane_transform = pane_transform,
        .overhang = MaxNodeOverhang(),
    };

    ReleaseStaleSelectionHlCaches(cache, p.selection);

    const int node_count = static_cast<int>(nodes.size());
    // 二分探索キーは未スナップのドキュメント Y (cache[i].text_top と同じ系)。
    // 下端より下にはみ出す描画 (見出し下線・空 LI の bullet 等) を上端で落とさないよう、
    // はみ出しの上限だけ手前から始める。
    const int first_visible = FindFirstVisibleNodeIndex(cache, scroll_y - frame_.overhang.below);

    // 同一 blockquote_group のノードをまとめてバー/背景を描画する。
    // ノード単体ごとではなく一括で描画することで、複数行の引用ブロックが連続した見た目になる。
    GenBlockQuoteGroupDecorations(first_visible);

    int visible_count = 0;
    for (int i = first_visible; i < node_count; i++) {
        const float local_text_top = cache.Top(i) - snapped_y;
        if (local_text_top - frame_.overhang.above > frame_.visible.bottom) {
            break;
        }
        const Node& node = nodes[i];
        const DiagramEntry* diagram = IsBitmapDrawNode(node) ? cache.FindDiagram(i) : nullptr;
        GenerateNode(node, cache[i], diagram, i, local_text_top);
        ++visible_count;
    }

    cmds_->emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    cmds_->emplace_back(PopClipCmd{});
    frame_.params = nullptr;
    cmds_ = nullptr;
    MENDO_COUNT_SET(g_cmd_gen_stats.last_visible_node_count, visible_count);
    MENDO_IF_TRACY(PublishCmdGenStats());
}

void CommandGenerator::GenerateNode(const Node& node, const NodeLayoutEntry& entry, const DiagramEntry* diagram, int node_index, float entry_text_top)
{
    const float x = layout::NodeBlockX(node, *theme_);
    const float cw = NodeContentWidth(node, *theme_, frame_.content_width);
    const float text_x = x + NodeTextXOffset(node, *theme_);

    // ブロック横スクロール対象 (テーブル/コードブロック) の内容を clip + 平行移動で囲み、
    // 直後にスクロールバーを積む。幾何はヒットテスト/reducer と共有の GetBlockHScrollGeometry に寄せる。
    const auto gen_h_scroll_block = [&](const D2D1_RECT_F& clip, float pad_y, auto&& gen_content) {
        const auto geom = GetBlockHScrollGeometry(frame_.params->nodes, frame_.params->cache, static_cast<size_t>(node_index), cw);
        const float scroll_x = LookupBlockScrollX(frame_.params->block_h_scroll.scroll_x, geom);
        {
            BlockHScrollScope guard(*cmds_, clip, frame_.pane_transform, scroll_x, geom.can_scroll());
            gen_content(scroll_x);
        }
        EmitBlockHScrollbarIfActive(node_index, x, BlockHScrollbarBarY(entry_text_top, entry.height, pad_y), geom, scroll_x);
    };

    switch (node.type) {
    case NodeType::HorizontalRule:
        GenHorizontalRule(x, cw, entry_text_top);
        return;

    case NodeType::Image:
        if (diagram && diagram->bitmap) {
            const float draw_h = entry.height;
            const float draw_w = (diagram->height > 0) ? diagram->width * (draw_h / diagram->height) : diagram->width;
            cmds_->emplace_back(DrawBitmapCmd{ diagram->bitmap.Get(), D2D1::RectF(x, entry_text_top, x + draw_w, entry_text_top + draw_h) });
        }
        else {
            GenDiagramPlaceholder(x, entry_text_top, cw, entry.height);
        }
        return;

    case NodeType::Table:
        gen_h_scroll_block(
            D2D1::RectF(x, entry_text_top, x + cw, entry_text_top + entry.height),
            NodeBoxPadBottom(node, *theme_),
            [&](float scroll_x) { GenTable(node, entry, node_index, x, cw, entry_text_top, scroll_x); });
        return;

    case NodeType::Diagram:
        if (diagram && diagram->bitmap) {
            cmds_->emplace_back(DrawBitmapCmd{ diagram->bitmap.Get(), MermaidBitmapRect(diagram->width, diagram->height, x, cw, entry_text_top) });
            GenOverlayButtons(node, diagram, x, cw, entry_text_top, node_index);
        }
        else {
            GenDiagramPlaceholder(x, entry_text_top, cw, entry.height, diagram ? DiagramErrorText(diagram->error) : std::wstring_view{});
        }
        return;

    case NodeType::CodeBlock: {
        const float pad_bottom = NodeBoxPadBottom(node, *theme_);
        const D2D1_RECT_F box = D2D1::RectF(x, entry_text_top - NodeBoxPadTop(node, *theme_), x + cw, entry_text_top + entry.height + pad_bottom);
        GenCodeBlockBg(box, !node.continues_prev(), !node.continues_next());
        // 背景とコピーボタンはクリップ外で固定描画 (GitHub と同じ挙動)。テキスト本体だけ scroll_x 分平行移動。
        gen_h_scroll_block(box, pad_bottom, [&](float) { GenNodeTextDecorations(node, entry, node_index, text_x, entry_text_top); });
        GenOverlayButtons(node, nullptr, x, cw, entry_text_top, node_index);
        return;
    }

    case NodeType::ListItem:
        GenListBullet(node, entry, x, entry_text_top);
        break;

    case NodeType::TaskListItem:
        // GenNodeTextDecorations は text_layout=nullptr (loose で空 TaskListItem) で early return
        // するため、checkbox は case 側で emit する。
        GenTaskListCheckbox(node, x, entry_text_top);
        break;

    case NodeType::BlockQuote:
        // バーと背景はグループ単位で GenBlockQuoteGroupDecorations から描画済み
        break;

    case NodeType::Heading: {
        const int8_t lv = node.heading_level;
        if (lv <= 2) {
            const float line_y = entry_text_top + entry.height + HeadingUnderlineGap(*theme_);
            cmds_->emplace_back(DrawLineCmd{
                D2D1::Point2F(x, line_y), D2D1::Point2F(x + cw, line_y),
                theme_->GetHeadingUnderlineThickness(lv), BrushId::Hr });
        }
        break;
    }

    case NodeType::Paragraph:
        break;

    default:
        std::unreachable();
    }

    GenNodeTextDecorations(node, entry, node_index, text_x, entry_text_top);
}

CommandGenerator::NodeOverhang CommandGenerator::MaxNodeOverhang() const noexcept
{
    const float underline = HeadingUnderlineGap(*theme_) + std::max(theme_->GetHeadingUnderlineThickness(1), theme_->GetHeadingUnderlineThickness(2));
    // 空 LI は height=0 だが bullet/checkbox は text_top より下に描かれる。
    const float empty_li = layout::EmptyListItemHeight(*theme_);
    // コードブロックの padding 内の装飾・アラート背景・引用バーは上下両方へ出る。
    const float box = std::max({ theme_->code_block_padding, ALERT_BG_PAD, BAR_EXTEND });
    // コピー/保存ボタンはズームに依らない固定サイズで box 上端から下へ伸びるため、縮小表示の
    // 低いブロックでは box の下へはみ出す。ノード上端からのボタン下端までを下側の上限に含める。
    const float copy_button = COPY_BTN_MARGIN + COPY_BTN_SIZE;
    return { box, std::max({ box, underline, empty_li, copy_button }) };
}

namespace {
BrushId GetNodeBaseBrush(const Node& node) noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        return BrushId::Heading;
    case NodeType::BlockQuote:
        return (node.alert_type != AlertType::None) ? BrushId::Text : BrushId::BlockquoteText;
    case NodeType::CodeBlock:
        return BrushId::CodeText;
    case NodeType::Paragraph:
    case NodeType::HorizontalRule:
    case NodeType::ListItem:
    case NodeType::Table:
    case NodeType::TaskListItem:
    case NodeType::Image:
    case NodeType::Diagram:
        return BrushId::Text;
    }
    std::unreachable();
}
} // namespace

void CommandGenerator::GenNodeTextDecorations(const Node& node, const NodeLayoutEntry& entry, int node_index, float text_x, float entry_text_top)
{
    if (!entry.text_layout()) {
        return;
    }

    GenInlineCodeBgs(*cmds_, entry.view_inline_code_bgs(), text_x, entry_text_top, frame_.visible);

    // 検索マッチのハイライト（選択より先に描画し、選択が最前面になるようにする）
    GenSearchHighlights(entry, node_index, text_x, entry_text_top);

    if (const auto [sel_start, sel_end] = frame_.params->selection.RangeIn(node_index, node.GetText().size()); sel_end > sel_start) {
        GenSelectionHighlightCached(node, entry, sel_start, sel_end - sel_start, text_x, entry_text_top);
    }

    auto cmd = MakeTextLayoutCmd(D2D1::Point2F(text_x, entry_text_top), entry.text_layout(), entry.height, GetNodeBaseBrush(node), frame_.visible);
    cmd.syntax_tokens = entry.syntax_tokens();
    cmds_->emplace_back(cmd);
}

void CommandGenerator::GenTaskListCheckbox(const Node& node, float x, float entry_text_top)
{
    const std::wstring_view icon = node.task_checked() ? L"\u2611" : L"\u2610"; // ☑ / ☐
    const float icon_size = theme_->font_size_body;
    const float cb_x = x - theme_->list_bullet_offset;
    EmitText(icon, formats_->checkbox.Get(), D2D1::Point2F(cb_x, entry_text_top),
             D2D1::SizeF(icon_size, icon_size * TASK_CHECKBOX_HEIGHT_FACTOR), BrushId::Text);
}

void CommandGenerator::GenHorizontalRule(float x, float w, float entry_text_top)
{
    const float y = entry_text_top + theme_->paragraph_spacing * 0.5f;
    cmds_->emplace_back(DrawLineCmd{
        D2D1::Point2F(x, y), D2D1::Point2F(x + w, y),
        theme_->hr_thickness, BrushId::Hr });
}

void CommandGenerator::GenCodeBlockBg(const D2D1_RECT_F& box, bool round_top, bool round_bottom)
{
    if (!round_top && !round_bottom) {
        cmds_->emplace_back(FillRectCmd{ box, BrushId::CodeBg });
        return;
    }
    cmds_->emplace_back(FillRoundedRectCmd{ box, CODE_BLOCK_CORNER, CODE_BLOCK_CORNER, BrushId::CodeBg });
    // 背景は不透明なので、丸めない側の角を矩形で塗りつぶす。
    if (!round_top) {
        cmds_->emplace_back(FillRectCmd{ D2D1::RectF(box.left, box.top, box.right, box.top + CODE_BLOCK_CORNER), BrushId::CodeBg });
    }
    if (!round_bottom) {
        cmds_->emplace_back(FillRectCmd{ D2D1::RectF(box.left, box.bottom - CODE_BLOCK_CORNER, box.right, box.bottom), BrushId::CodeBg });
    }
}

void CommandGenerator::EmitBlockHScrollbarIfActive(int node_index, float block_x, float bar_y, const BlockHScrollGeometry& geom, float scroll_x)
{
    if (!geom.can_scroll()) {
        return;
    }
    if (node_index != frame_.params->block_h_scroll.hovered_block && node_index != frame_.params->block_h_scroll.drag_block) {
        return;
    }
    const float track_w = geom.visible_width;
    const float thumb_w = BlockHScrollbarThumbWidth(geom.visible_width, geom.block_width);
    const float scroll_max = geom.scroll_max();
    const float ratio = std::clamp(scroll_x / scroll_max, 0.0f, 1.0f);
    // サブピクセル位置だとアンチエイリアスが上下に漏れて thumb の太さがブレるため snap する。
    const float thumb_x = SnapToPhysicalPixel(block_x + ratio * (track_w - thumb_w), frame_.dpi_scale);
    const float thumb_y = SnapToPhysicalPixel(bar_y, frame_.dpi_scale);
    const float w_snapped = SnapToPhysicalPixel(thumb_w, frame_.dpi_scale);
    const float h_snapped = SnapToPhysicalPixel(PANE_SCROLLBAR_WIDTH, frame_.dpi_scale);
    cmds_->emplace_back(FillRoundedRectCmd{
        D2D1::RectF(thumb_x, thumb_y, thumb_x + w_snapped, thumb_y + h_snapped),
        h_snapped / 2.0f, h_snapped / 2.0f,
        BrushId::ScrollbarThumb });
}

void CommandGenerator::GenOverlayButtons(const Node& node, const DiagramEntry* diagram, float x, float cw, float entry_text_top, int node_index)
{
    // 図の保存/コピーは普段は隠し、マウスが図の画像領域へ入ったときだけ出す。
    if (node.type == NodeType::Diagram && frame_.params->hovered_diagram != node_index) {
        return;
    }
    layout::ForEachOverlayButton(node, diagram, x, cw, entry_text_top, *theme_, [&](const D2D1_RECT_F& btn, OverlayButton kind) {
        // 保存は Download、コピーは Copy (Segoe Fluent Icons)。
        const wchar_t icon = kind == OverlayButton::DiagramSave ? L'\uE896' : L'\uE8C8';
        GenOverlayButton(btn, icon, frame_.params->hovered == OverlayButtonHit{ node_index, kind });
    });
}

void CommandGenerator::GenOverlayButton(D2D1_RECT_F btn, wchar_t icon, bool is_hovered)
{
    if (!formats_->copy_btn_icon) {
        return;
    }
    const bool dark = theme_->dark;
    const float bg_alpha = is_hovered ? (dark ? 0.30f : 0.15f) : (dark ? 0.10f : 0.05f);
    const float text_alpha = is_hovered ? (dark ? 0.9f : 0.8f) : (dark ? 0.4f : 0.35f);
    EmitButton(*cmds_, text_layouts_, formats_->copy_btn_icon.Get(), btn, { &icon, 1 }, { BrushId::Overlay, COPY_BTN_CORNER, bg_alpha }, BrushId::Overlay,
               text_alpha);
}

void CommandGenerator::GenListBullet(const Node& node, const NodeLayoutEntry& entry, float x, float entry_text_top)
{
    const float first_line_h = GetFirstLineHeight(entry, theme_->font_size_body);
    if (node.list_ordered()) {
        std::array<wchar_t, kListNumberMaxLen> num_buf;
        EmitText(FormatListNumber(num_buf, node.list_number(), node.list_depth()), formats_->list_number.Get(),
                 D2D1::Point2F(x - theme_->list_bullet_offset - LIST_NUMBER_PAD_RIGHT, entry_text_top),
                 D2D1::SizeF(theme_->list_bullet_offset + LIST_NUMBER_PAD_RIGHT - LIST_NUMBER_PAD_LEFT, first_line_h),
                 BrushId::Text);
        return;
    }

    const float bullet_x = SnapToPhysicalPixel(x - theme_->list_bullet_offset * LIST_BULLET_X_FACTOR, frame_.dpi_scale);
    const float bullet_y = SnapToPhysicalPixel(entry_text_top + first_line_h * 0.5f, frame_.dpi_scale);
    const float r = theme_->list_bullet_radius;
    switch (ListStyleIndex(node.list_depth())) {
    case 0:
        cmds_->emplace_back(FillEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, BrushId::Text });
        break;
    case 1:
        cmds_->emplace_back(DrawEllipseCmd{ D2D1::Point2F(bullet_x, bullet_y), r, r, 1.0f, BrushId::Text });
        break;
    default: {
        // 中心は物理ピクセル境界にあるので、半辺も物理ピクセル単位にして辺をにじませない。
        const float h = SnapToPhysicalPixel(r * LIST_SQUARE_HALF_FACTOR, frame_.dpi_scale);
        cmds_->emplace_back(FillRectCmd{ D2D1::RectF(bullet_x - h, bullet_y - h, bullet_x + h, bullet_y + h), BrushId::Text });
        break;
    }
    }
}

void CommandGenerator::GenBlockQuoteGroupDecorations(int first_visible)
{
    const auto& nodes = frame_.params->nodes;
    const auto& cache = frame_.params->cache;
    const float snap = frame_.snapped_scroll_y;
    const int node_count = static_cast<int>(nodes.size());

    // バーと背景はクリップされるので、グループの可視範囲外の部分は走査しない (巨大な引用でも
    // 可視ノード数ぶんで済む)。first_visible がグループ途中なら、可視範囲より上にある直前の
    // 1 ノードから始めれば、上から続くバーと背景が可視範囲の上端より外から描かれる。
    int i = first_visible;
    if (i > 0 && i < node_count && nodes[i].blockquote_group >= 0 && nodes[i - 1].blockquote_group == nodes[i].blockquote_group) {
        --i;
    }

    while (i < node_count) {
        const int group = nodes[i].blockquote_group;
        if (group < 0) {
            // text_top は単調なので、非引用ノードが下端を超えたら以降のグループも全て可視域外。
            // ここで break しないと引用が可視域以降に無い文書で毎フレーム末尾まで全走査する。
            if (cache.Top(i) - snap > frame_.visible.bottom) {
                break;
            }
            i++;
            continue;
        }
        const float group_top = cache.Top(i) - snap;
        if (group_top - frame_.overhang.above > frame_.visible.bottom) {
            break;
        }

        float group_bottom = group_top + cache[i].height;
        int max_depth = nodes[i].quote_depth;

        // 可視範囲の下端を越えたノードまで含めれば、下へ続くバーと背景は可視範囲の外まで伸びる。
        int j = i + 1;
        while (j < node_count && nodes[j].blockquote_group == group) {
            group_bottom = std::max(group_bottom, cache.Bottom(j) - snap);
            max_depth = std::max<int>(max_depth, nodes[j].quote_depth);
            j++;
            if (group_bottom > frame_.visible.bottom) {
                break;
            }
        }

        if (max_depth > 0 && nodes[i].quote_outer_indent > 0) {
            GenBlockQuoteGroup(i, j, max_depth, group_top, group_bottom);
        }
        // 下端で打ち切ったグループの残りを別グループとして描かない。
        if (j < node_count && nodes[j].blockquote_group == group) {
            break;
        }
        i = j;
    }
}

void CommandGenerator::GenBlockQuoteGroup(int first, int last, int max_depth, float group_top, float group_bottom)
{
    const auto& nodes = frame_.params->nodes;
    const auto& cache = frame_.params->cache;
    static constexpr float ALERT_BG_CORNER = 4.0f;

    const AlertType alert_type = nodes[first].alert_type;
    const int outer_indent = nodes[first].quote_outer_indent;
    const float outer_x_indent = static_cast<float>(outer_indent) * theme_->indent_width;
    if (const auto idx = AlertColorIndex(alert_type); idx < ALERT_TYPE_COUNT) {
        const float outer_x = theme_->margin_left + outer_x_indent;
        const float cw = ShrinkKeepingMinContent(frame_.content_width, outer_x_indent);
        const D2D1_RECT_F bg_rect = D2D1::RectF(outer_x - ALERT_BG_PAD, group_top - ALERT_BG_PAD, outer_x + cw, group_bottom + ALERT_BG_PAD);
        cmds_->emplace_back(FillRoundedRectCmd{ bg_rect, ALERT_BG_CORNER, ALERT_BG_CORNER, AlertBgBrushIdFromIndex(idx) });
    }

    // GitHub と同様に、外側のバーはネストした範囲を貫通して連続描画する。
    // 各 level ごとに quote_depth >= level の連続区間を求めてバーを発行。
    for (int level = 1; level <= max_depth; ++level) {
        const float bar_indent_x = static_cast<float>(outer_indent + level - 1) * theme_->indent_width;
        const float bar_x = theme_->margin_left + bar_indent_x - theme_->indent_width * 0.5f;
        const bool is_alert_bar = (level == 1 && alert_type != AlertType::None);
        const BrushId bar_brush = is_alert_bar ? AlertBrushIdFromIndex(AlertColorIndex(alert_type)) : BrushId::BlockquoteBar;

        const auto emit_bar = [&](float top, float bottom) {
            cmds_->emplace_back(DrawLineCmd{
                D2D1::Point2F(bar_x, top - BAR_EXTEND),
                D2D1::Point2F(bar_x, bottom + BAR_EXTEND),
                theme_->blockquote_bar_width, bar_brush });
        };

        bool in_region = false;
        float region_top = 0.0f;
        float region_bottom = 0.0f;
        for (int k = first; k < last; ++k) {
            const float local_top_k = cache.Top(k) - frame_.snapped_scroll_y;
            if (nodes[k].quote_depth >= level) {
                if (!in_region) {
                    in_region = true;
                    region_top = local_top_k;
                }
                region_bottom = local_top_k + cache[k].height;
            }
            else if (in_region) {
                emit_bar(region_top, region_bottom);
                in_region = false;
            }
        }
        if (in_region) {
            emit_bar(region_top, region_bottom);
        }
    }
}

void CommandGenerator::GenDiagramPlaceholder(float x, float y, float w, float h, std::wstring_view error)
{
    GenCodeBlockBg(D2D1::RectF(x, y, x + w, y + h), true, true);
    const auto origin = D2D1::Point2F(x, y);
    const auto box = D2D1::SizeF(w, h);
    if (!error.empty()) {
        EmitText(error, formats_->placeholder_text.Get(), origin, box, BrushId::AlertCaution);
        return;
    }
    EmitText(i18n::S().loading, formats_->placeholder_text.Get(), origin, box, BrushId::BlockquoteText);
}

void CommandGenerator::GenTableCellContent(std::string_view cell_text, const CellDrawContext& ctx)
{
    if (!ctx.layout) {
        return;
    }
    if (ctx.selection.end > ctx.selection.start) {
        const uint32_t cell_len = static_cast<uint32_t>(cell_text.size());
        const uint32_t ov_start = std::max(ctx.selection.start, ctx.flat_offset);
        const uint32_t ov_end = std::min(ctx.selection.end, ctx.flat_offset + cell_len);
        if (ov_end > ov_start) {
            // sel_start/sel_end は UTF-8 byte offset。HitTestTextRange 用に UTF-16 へ変換する。
            Utf16OffsetCursor cursor{ cell_text };
            auto range = cursor.WideRange(ov_start - ctx.flat_offset, ov_end - ov_start);
            // セルの選択は毎フレーム引くので、広げる (HitTestTextPosition 2 回) のは数式のあるセルだけにする。
            if (std::ranges::any_of(ctx.runs, &TextRun::math)) {
                range = ExpandToClusters(ctx.layout, range.startPosition, range.length);
            }
            if (range.length > 0) {
                for (const auto& m : HitTestRange(ctx.layout, range.startPosition, range.length)) {
                    EmitFillRectIfVisible(RectFromHitTest(m, ctx.text_x, ctx.text_y), BrushId::Selection);
                }
            }
        }
    }
    const BrushId cell_brush = ctx.is_header ? BrushId::Heading : BrushId::Text;
    cmds_->emplace_back(MakeTextLayoutCmd(D2D1::Point2F(ctx.text_x, ctx.text_y), ctx.layout, ctx.height, cell_brush, frame_.visible));
}

void CommandGenerator::GenTable(const Node& node, const NodeLayoutEntry& entry,
    int node_index, float offset_x, float clip_width, float entry_text_top, float h_scroll_x)
{
    const auto* tbl = node.table_data();
    const auto* tl_ptr = entry.table_layout();
    if (!tbl || tbl->row_count == 0 || !tl_ptr || !tl_ptr->HasGeometry()) {
        return;
    }

    const float border = TABLE_BORDER_WIDTH;
    const auto& tl = *tl_ptr;
    const auto row_count = tbl->row_count;

    const float table_width = tl.TableWidth();

    const auto selection = frame_.params->selection.RangeIn(node_index, node.GetText().size());

    // 可視行帯を row_cum_y の二分探索で求め、その範囲だけループする
    // (FindTableRow と同じパターン)。巨大テーブルで
    // 毎フレーム row_count 回の continue ループが走るのを防ぐ。
    const YRange local_visible = frame_.visible.ToLocal(entry_text_top);
    const auto [r_begin, r_end] = tl.VisibleRowRange(local_visible.top, local_visible.bottom);
    float y = entry_text_top + tl.row_cum_y[r_begin];

    // 横スクロール時は clip に映る範囲が h_scroll_x だけ右にずれる。
    const auto [c_begin, c_end] = tl.VisibleColRange(h_scroll_x + border, h_scroll_x + clip_width + border);

    const auto search_nm = PrepareNodeSearchMatches(entry, node_index);

    for (size_t r = r_begin; r < r_end; r++) {
        const float row_h = tl.row_heights[r];
        const bool is_header_row = tbl->IsHeaderRow(r);
        if (is_header_row || r % 2 == 0) {
            cmds_->emplace_back(FillRectCmd{ D2D1::RectF(offset_x, y, offset_x + table_width, y + row_h + border),
                                            is_header_row ? BrushId::CodeBg : BrushId::TableStripe });
        }

        cmds_->emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y), border, BrushId::Hr });

        std::span<const CellInlineCodeBg> row_bgs;
        if (r < tl.row_code_bgs.size()) {
            row_bgs = tl.row_code_bgs[r];
        }
        for (size_t c = c_begin; c < c_end; c++) {
            const auto [text_x, text_y] = TableCellTextOrigin(offset_x + tl.col_cum_x[c], y);

            GenCellInlineCodeBgs(*cmds_, row_bgs, c, text_x, text_y);

            IDWriteTextLayout* const cell_layout = tl.GetCellLayout(r, c);
            GenCellSearchHighlights(search_nm, cell_layout, r, c, text_x, text_y);

            GenTableCellContent(tbl->GetCellText(r, c), CellDrawContext{
                .layout = cell_layout,
                .text_x = text_x,
                .text_y = text_y,
                .height = row_h,
                .flat_offset = tbl->CellTextStart(r, c),
                .runs = tbl->GetCellRuns(r, c),
                .selection = selection,
                .is_header = is_header_row,
            });
        }
        y += row_h + border;
    }

    // r_end で打ち切った場合も下端線はテーブル全体の底に置く (可視外ならクリップされる)
    const float table_bottom = entry_text_top + tl.row_cum_y[row_count];
    cmds_->emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, table_bottom), D2D1::Point2F(offset_x + table_width, table_bottom), border, BrushId::Hr });

    // 縦罫線は可視列ごとに表の上端から下端まで 1 本で引く (行ごとに引くと 行×列 本になる)。可視外はクリップされる。
    const auto vline = [&](float x) {
        cmds_->emplace_back(DrawLineCmd{ D2D1::Point2F(x, entry_text_top), D2D1::Point2F(x, table_bottom), border, BrushId::Hr });
    };
    for (size_t c = c_begin; c < c_end; c++) {
        vline(offset_x + tl.col_cum_x[c] - border);
    }
    vline(offset_x + table_width);
}

std::span<const DWRITE_HIT_TEST_METRICS> CommandGenerator::HitTestRange(IDWriteTextLayout* layout, uint32_t start, uint32_t length)
{
    MENDO_COUNT_INC(g_cmd_gen_stats.hittest_range);
    const UINT32 count = FetchHitTestMetrics(layout, start, length, hit_test_buffer_);
    return { hit_test_buffer_.data(), count };
}

void CommandGenerator::EmitFillRectIfVisible(const D2D1_RECT_F& r, BrushId brush)
{
    if (OverlapsY(r, frame_.visible)) {
        cmds_->emplace_back(FillRectCmd{ r, brush });
    }
}

void CommandGenerator::CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::vector<D2D1_RECT_F>& out)
{
    // 数式の一部に当たった検索ヒットや選択も、数式全体を塗る。引いた箱はキャッシュするので、数式の無い段落でも広げる。
    const auto range = ExpandToClusters(layout, start, length);
    // 少しずつ追記されるので reserve はしない (要求ぴったりの再確保が毎回走る)。
    for (const auto& m : HitTestRange(layout, range.startPosition, range.length)) {
        out.emplace_back(RectFromHitTest(m));
    }
}

const LineWindow& CommandGenerator::LineWindowAround(const NodeLayoutEntry& entry, float origin_y) const
{
    return EnsureLineWindow(entry, frame_.visible.ToLocal(origin_y));
}

void CommandGenerator::GenSelectionHighlightCached(const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y)
{
    auto* layout = entry.text_layout();
    if (!layout || doc_length == 0) {
        return;
    }
    auto& cache = entry.ensure_selection_hl_cache();
    // キャッシュキーは UTF-8 byte 単位 (selection 状態と直接対応)。
    // 位置変換は選択が変わったときだけ前方走査で行う。
    if (cache.start != doc_start || cache.length != doc_length) {
        Utf16OffsetCursor cursor{ node.GetText() };
        const auto wr = cursor.WideRange(doc_start, doc_length);
        cache.start = doc_start;
        cache.length = doc_length;
        cache.start_w = wr.startPosition;
        cache.end_w = wr.startPosition + wr.length;
    }
    // 背の高いノードでは選択範囲を可視範囲の前後の行に絞って引き、全行選択でも矩形を可視行数ぶんで済ませる。
    const auto& window = LineWindowAround(entry, origin_y);
    uint32_t begin = std::max(cache.start_w, window.text_begin);
    uint32_t end = std::min(cache.end_w, window.text_end);
    if (end <= begin) {
        begin = end = 0;
    }
    if (begin != cache.rects_begin_w || end != cache.rects_end_w) {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_miss);
        cache.rects.clear();
        if (end > begin) {
            CollectHitTestRects(layout, begin, end - begin, cache.rects);
        }
        cache.rects_begin_w = begin;
        cache.rects_end_w = end;
    }
    else {
        MENDO_COUNT_INC(g_cmd_gen_stats.sel_hl_cache_hit);
    }
    for (const auto& local : cache.rects) {
        EmitFillRectIfVisible(OffsetRectF(local, origin_x, origin_y), BrushId::Selection);
    }
}

CommandGenerator::NodeSearchMatches CommandGenerator::PrepareNodeSearchMatches(const NodeLayoutEntry& entry, int node_index)
{
    const std::span<const SearchMatch> matches = frame_.params->search_matches;
    if (matches.empty()) {
        return {};
    }
    const auto range = std::ranges::equal_range(matches, node_index, {}, &SearchMatch::node_index);
    const size_t first_global = static_cast<size_t>(range.begin() - matches.begin());
    const uint32_t spill_length_w = SpillLengthInto(matches, first_global, node_index);
    if (range.empty() && spill_length_w == 0) {
        return {};
    }

    // layout 変更時は invalidate_search_hl_cache() でキャッシュ自体が破棄されており、SearchState の
    // generation は 1 から始まるため、cache.gen == search_generation のみでキャッシュ有効性を判定できる。
    auto& cache = entry.ensure_search_hl_cache();
    if (cache.gen != frame_.params->search_generation) {
        MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_reset);
        cache.rects.clear();
        cache.spans.assign(range.size() + (spill_length_w > 0 ? 1 : 0), {});
        cache.gen = frame_.params->search_generation;
    }
    return { matches.subspan(first_global, range.size()), first_global, &cache, spill_length_w };
}

void CommandGenerator::EmitSearchMatches(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t begin, size_t end, float origin_x, float origin_y)
{
    for (size_t i = begin; i < end; ++i) {
        const auto& m = nm.matches[i];
        EmitSearchSpan(*nm.cache, i, layout, m.start_w, m.length_w, nm.first_global + i, origin_x, origin_y);
    }
}

void CommandGenerator::EmitSearchSpan(SearchHlCache& cache, size_t span_index, IDWriteTextLayout* layout, uint32_t start_w, uint32_t length_w,
                                      size_t global_index, float origin_x, float origin_y)
{
    auto& span = cache.spans[span_index];
    if (span.count == SearchHlCache::Span::NOT_COMPUTED) {
        // evict 中のセルは未計算のまま残し、layout が復元されてから計算する。
        if (!layout) {
            MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_provisional);
            return;
        }
        MENDO_COUNT_INC(g_cmd_gen_stats.search_hl_computed);
        span.begin = static_cast<uint32_t>(cache.rects.size());
        if (length_w > 0) {
            CollectHitTestRects(layout, start_w, length_w, cache.rects);
        }
        span.count = static_cast<uint32_t>(cache.rects.size()) - span.begin;
    }

    const bool is_current = (static_cast<int>(global_index) == frame_.params->current_match_index);
    const BrushId hl_brush = is_current ? BrushId::SearchHighlightCurrent : BrushId::SearchHighlight;
    for (const auto& r : std::span{ cache.rects }.subspan(span.begin, span.count)) {
        EmitFillRectIfVisible(OffsetRectF(r, origin_x, origin_y), hl_brush);
    }
}

void CommandGenerator::GenSearchHighlights(const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y)
{
    const auto nm = PrepareNodeSearchMatches(entry, node_index);
    if (nm.matches.empty() && nm.spill_length_w == 0) {
        return;
    }
    // マッチは start_w 昇順で重ならないので、終端も昇順に並ぶ。
    const auto& window = LineWindowAround(entry, origin_y);
    if (nm.spill_length_w > window.text_begin) {
        EmitSearchSpan(*nm.cache, nm.matches.size(), entry.text_layout(), 0, nm.spill_length_w, nm.first_global - 1, origin_x, origin_y);
    }
    const auto first = std::ranges::partition_point(nm.matches, [b = window.text_begin](const SearchMatch& m) noexcept {
        return m.start_w + m.length_w <= b;
    });
    const auto last = std::ranges::partition_point(first, nm.matches.end(), [e = window.text_end](const SearchMatch& m) noexcept {
        return m.start_w < e;
    });
    EmitSearchMatches(nm, entry.text_layout(),
        static_cast<size_t>(first - nm.matches.begin()), static_cast<size_t>(last - nm.matches.begin()), origin_x, origin_y);
}

void CommandGenerator::GenCellSearchHighlights(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t row, size_t col, float origin_x, float origin_y)
{
    if (nm.matches.empty()) {
        return;
    }
    // テーブル内 match は (row, col, start) 昇順なので、セル範囲を二分探索で切り出す。
    const auto cell = std::ranges::equal_range(
        nm.matches, std::pair{ static_cast<int>(row), static_cast<int>(col) }, {},
        [](const SearchMatch& m) static noexcept { return std::pair{ m.table_row, m.table_col }; });
    EmitSearchMatches(nm, layout,
        static_cast<size_t>(cell.begin() - nm.matches.begin()), static_cast<size_t>(cell.end() - nm.matches.begin()), origin_x, origin_y);
}
