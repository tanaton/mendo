#include "command_generator.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
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
#include <optional>
#include <ranges>
#include <span>
#include <utility>

using layout::NodeBoxPadBottom;
using layout::NodeBoxPadTop;
using layout::NodeContentWidth;
using layout::NodeTextXOffset;

namespace {

struct LineHit {
    uint32_t pos;
    float line_top;
    float line_bottom;
};

std::optional<LineHit> HitLineAt(IDWriteTextLayout* layout, float y) noexcept
{
    BOOL is_trailing = FALSE;
    BOOL is_inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(layout->HitTestPoint(0.0f, y, &is_trailing, &is_inside, &m))) {
        return std::nullopt;
    }
    const auto line = LineBoxAt(layout, m.textPosition);
    if (!line) {
        return std::nullopt;
    }
    return LineHit{ m.textPosition, line->top, line->top + line->height };
}

// 本文 text_layout の、view (layout ローカル Y) にかかる行とその上下 1 行のテキスト範囲 [first, second) (UTF-16)。
// 検索・選択ハイライトが矩形を引く範囲を絞るのに使う。行の位置を覚えず毎フレーム引く (BM_SelectionFrame / BM_SearchFrame)。
// 行のテキスト位置を隣の行の位置で挟む。行のテキスト範囲は論理順に並ぶので、隣の行ならどの x で引いても
// 境界になる (RTL の行でも成り立つ)。layout の端を越えた側は先に行が無いので引かない。
std::pair<uint32_t, uint32_t> TextRangeOfVisibleLines(const NodeLayoutEntry& entry, YRange view)
{
    auto* const layout = entry.text_layout();
    uint32_t begin = 0;
    uint32_t end = UINT32_MAX;
    if (view.top > 0.0f) {
        if (const auto first = HitLineAt(layout, view.top); first && first->line_top > 0.0f) {
            if (const auto prev = HitLineAt(layout, first->line_top - 0.5f); prev && prev->line_top < first->line_top) {
                begin = prev->pos;
            }
        }
    }
    if (view.bottom < entry.height) {
        if (const auto last = HitLineAt(layout, view.bottom)) {
            if (const auto next = HitLineAt(layout, last->line_bottom + 0.5f); next && next->line_top > last->line_top) {
                end = next->pos;
            }
        }
    }
    return { begin, end };
}

constexpr D2D1_RECT_F SquareAround(float cx, float cy, float half) noexcept
{
    return D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
}

// DIP。
constexpr float NAV_BTN_CORNER = 6.0f;
constexpr float COPY_BTN_CORNER = 4.0f;
constexpr float INLINE_CODE_CORNER = 6.0f;
constexpr float CODE_BLOCK_CORNER = 4.0f;
constexpr float LIST_BULLET_X_FACTOR = 0.6f;
// 円と同じ直径の四角は面積が 4/π 倍あって重く見えるので、半辺を半径より少し小さくする。
constexpr float LIST_SQUARE_HALF_FACTOR = 0.85f;
constexpr float LIST_NUMBER_PAD_LEFT = 4.0f;
constexpr float LIST_NUMBER_PAD_RIGHT = 8.0f;
constexpr float SEARCH_BAR_CORNER = 4.0f;
constexpr float SPINNER_DOT_RADIUS = 3.0f;
constexpr float SPINNER_DOT_FADE_FACTOR = 0.85f;
constexpr float GESTURE_OVERLAY_HEIGHT = 80.0f;
constexpr float GESTURE_OVERLAY_CORNER = 12.0f;
constexpr float TOAST_OVERLAY_WIDTH = 320.0f;
constexpr float TOAST_OVERLAY_CORNER = 8.0f;
constexpr float TOAST_OVERLAY_BOTTOM_OFFSET = 16.0f;

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
    return { r, INLINE_CODE_CORNER, BrushId::InlineCodeBg };
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

// MeasureNode 時に確定済みの first_line_height を優先利用する。
// 0 (text_layout の無いゆるいリストと空の項目) は、続く段落の 1 行目と同じ本文の行送りにする。
// 描画ホットパスから COM 越境呼び出しを排除する。
float GetFirstLineHeight(const NodeLayoutEntry& entry, float font_size) noexcept
{
    return (entry.first_line_height() > 0.0f) ? entry.first_line_height() : font_size * BODY_LINE_HEIGHT_FACTOR;
}

// 失敗していなければ空。
std::wstring_view DiagramErrorText(const DiagramError& error) noexcept
{
    switch (error.kind) {
    case DiagramError::Kind::None:
        return {};
    case DiagramError::Kind::Render:
        return error.message.empty() ? i18n::S().diagram_error : std::wstring_view{ error.message };
    case DiagramError::Kind::Unsupported:
        return i18n::S().diagram_unsupported;
    case DiagramError::Kind::ImageUnavailable:
        return error.message;
    }
    return {};
}

// 段ごとに繰り返す書式の何番目か。GitHub (ブラウザ既定) は 3 段目より深くても 3 種類目のままだが、深い段も見分けられる
// ようにする。段を持たない (0 以下) ときは最外側として扱う。
constexpr int ListStyleIndex(int list_depth, int styles) noexcept
{
    return (std::max(list_depth, 1) - 1) % styles;
}

// 番号は 1. → i. → a. の 3 種類。
constexpr int kListNumberStyles = 3;
// 記号は ● ○ ■ □ の 4 種類。3 種類では ■ の次が ● になり、塗った記号が続いて段の境が見分けにくい (#407)。
// 菱形は描画命令を足す必要があるので入れない。
constexpr int kListBulletStyles = 4;

} // namespace

void CmdWriter::Button(IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, std::wstring_view label, ButtonBg bg, BrushId fg, float fg_opacity) const
{
    if (bg.brush != BrushId::None) {
        if (bg.corner > 0.0f) {
            out.push_back(FillRoundedRectCmd{ .rect = rect, .radius = bg.corner, .brush_id = bg.brush, .opacity = bg.opacity });
        }
        else {
            out.push_back(FillRectCmd{ .rect = rect, .brush_id = bg.brush });
        }
    }
    Centered(label, fmt, rect, fg, fg_opacity);
}

std::wstring_view FormatListNumber(std::span<wchar_t, kListNumberMaxLen> buf, int32_t number, int list_depth) noexcept
{
    const int style = ListStyleIndex(list_depth, kListNumberStyles);
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
    return VScrollThumb(pane, ComputeScrollInfo(pane, header_height, total_content_height), scroll_y, may_grow);
}

std::optional<FillRoundedRectCmd> VScrollThumb(const PaneRect& pane, const PaneScrollInfo& info, float scroll_y, bool may_grow) noexcept
{
    if (!ShowsVScrollbar(info, may_grow)) {
        return std::nullopt;
    }
    const float x = VScrollbarLeftX(pane.x + pane.width);
    const float thumb_y = ComputeThumbY(info, scroll_y);
    return FillRoundedRectCmd{
        .rect = D2D1::RectF(x, thumb_y, x + PANE_SCROLLBAR_WIDTH, thumb_y + info.thumb_height), .radius = PANE_SCROLLBAR_WIDTH / 2.0f, .brush_id = BrushId::ScrollbarThumb
    };
}

SearchInputMetrics MeasureSearchInput(TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const SearchBarRenderState& sb, float width)
{
    const bool has_comp = !sb.ime_composition.empty();
    const D2D1_SIZE_F box{ width, SEARCH_INPUT_HEIGHT };

    const int text_len = static_cast<int>(sb.query.size());
    const int comp_len = static_cast<int>(sb.ime_composition.size());
    const int caret = (sb.caret_pos < 0 || sb.caret_pos > text_len) ? text_len : sb.caret_pos;

    IDWriteTextLayout* layout = nullptr;
    if (has_comp) {
        // IMEコンポジション中は確定済みテキストのキャレット位置に変換中テキストを挿入して表示する。
        std::wstring text(sb.query);
        text.insert(static_cast<size_t>(caret), sb.ime_composition);
        layout = text_layouts.Get(fmt, text, box, { static_cast<UINT32>(caret), static_cast<UINT32>(comp_len) });
    }
    else {
        layout = text_layouts.Get(fmt, sb.query, box);
    }
    if (!layout) {
        return {};
    }
    SearchInputMetrics m{ .layout = layout };

    const bool has_selection = !has_comp && sb.selection_start >= 0 && sb.caret_pos >= 0 && sb.selection_start != sb.caret_pos;
    if (has_selection) {
        const int sel_min = std::clamp(std::min(sb.selection_start, sb.caret_pos), 0, text_len);
        const int sel_max = std::clamp(std::max(sb.selection_start, sb.caret_pos), 0, text_len);
        if (sel_min < sel_max) {
            // 単一行テキストなのでメトリクスは1つで十分
            DWRITE_HIT_TEST_METRICS htm_sel{};
            UINT32 actual = 0;
            layout->HitTestTextRange(static_cast<UINT32>(sel_min), static_cast<UINT32>(sel_max - sel_min), 0.0f, 0.0f, &htm_sel, 1, &actual);
            if (actual > 0) {
                m.selection_x = htm_sel.left;
                m.selection_width = htm_sel.width;
            }
        }
    }

    // コンポジション中はその末尾に置く。
    FLOAT px, py;
    DWRITE_HIT_TEST_METRICS htm{};
    layout->HitTestTextPosition(static_cast<UINT32>(caret + comp_len), false, &px, &py, &htm);
    m.caret_x = px;
    return m;
}

int HitTestSearchInput(TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, std::wstring_view query, float local_x, float width)
{
    auto* const layout = text_layouts.Get(fmt, query, { width, SEARCH_INPUT_HEIGHT });
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

namespace overlay {

void EmitLoadingSpinner(DrawCommandList& out, float angle, const PaneRect& md_pane_rect)
{
    // 不透明度は点の番号だけで決まる。
    static constexpr auto kAlphas = []() static noexcept {
        std::array<float, spinner::DOT_COUNT> a{};
        for (int i = 0; i < spinner::DOT_COUNT; ++i) {
            a[i] = 1.0f - i * (SPINNER_DOT_FADE_FACTOR / spinner::DOT_COUNT);
        }
        return a;
    }();
    const auto [cx, cy] = RectCenter(ToD2DRect(md_pane_rect));
    for (int i = 0; i < spinner::DOT_COUNT; i++) {
        const float a = angle - i * (TWO_PI / spinner::DOT_COUNT);
        out.push_back(FillRoundedRectCmd{
            .rect = SquareAround(cx + spinner::RADIUS * std::cos(a), cy + spinner::RADIUS * std::sin(a), SPINNER_DOT_RADIUS),
            .radius = SPINNER_DOT_RADIUS,
            .brush_id = BrushId::Text,
            .opacity = kAlphas[i],
        });
    }
}

void EmitNavButtons(CmdWriter w, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect, bool dark, bool can_back, bool can_forward, NavDirection hovered)
{
    // 色を差し替えず、白黒の固定ブラシに不透明度を掛ける。
    const auto emit_button = [&](const D2D1_RECT_F& rect, bool enabled, bool is_hovered, std::wstring_view arrow) {
        const float bg_alpha = !enabled ? (dark ? 0.08f : 0.05f) : is_hovered ? (dark ? 0.35f : 0.25f)
                                                                              : (dark ? 0.15f : 0.10f);
        const float text_alpha = !enabled ? (dark ? 0.2f : 0.15f) : is_hovered ? 1.0f
                                                                               : (dark ? 0.6f : 0.5f);
        w.Button(fmt, rect, arrow, { BrushId::Overlay, NAV_BTN_CORNER, bg_alpha }, BrushId::Overlay, text_alpha);
    };
    // クリック判定 (NavButtonHitTest) と同じ矩形 API を使い、描画とヒットのズレを防ぐ
    emit_button(NavBackButtonRect(md_pane_rect), can_back, hovered == NavDirection::Back, L"\x25C0");
    emit_button(NavForwardButtonRect(md_pane_rect), can_forward, hovered == NavDirection::Forward, L"\x25B6");
}

namespace {
// ジェスチャー/トーストの背景パネルと文言。
// box は rect の大きさ。矩形の差から求めると丸めで値が揺れ、文字レイアウトのキャッシュが外れる。
void EmitPanel(CmdWriter w, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, D2D1_SIZE_F box, float corner, float alpha, std::wstring_view text)
{
    w.out.push_back(FillRoundedRectCmd{ .rect = rect, .radius = corner, .brush_id = BrushId::OverlayGestureBg, .opacity = alpha });
    w.Text(text, fmt, D2D1::Point2F(rect.left, rect.top), box, BrushId::OverlayWhite);
}
} // namespace

void EmitGestureOverlay(CmdWriter w, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect, bool dark, NavDirection direction)
{
    if (direction == NavDirection::None) {
        return;
    }
    constexpr float h = GESTURE_OVERLAY_HEIGHT;
    const auto [cx, cy] = RectCenter(ToD2DRect(md_pane_rect));
    const auto& text = (direction == NavDirection::Back) ? i18n::S().gesture_back : i18n::S().gesture_forward;
    EmitPanel(w, fmt, D2D1::RectF(cx - GESTURE_OVERLAY_WIDTH / 2, cy - h / 2, cx + GESTURE_OVERLAY_WIDTH / 2, cy + h / 2),
              D2D1::SizeF(GESTURE_OVERLAY_WIDTH, h), GESTURE_OVERLAY_CORNER, dark ? 0.8f : 0.6f, text);
}

void EmitToast(CmdWriter w, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect, bool dark, std::wstring_view message)
{
    if (message.empty()) {
        return;
    }
    constexpr float h = TOAST_OVERLAY_HEIGHT;
    const float cx = RectCenter(ToD2DRect(md_pane_rect)).x;
    const float bottom_y = NavBackButtonRect(md_pane_rect).top - TOAST_OVERLAY_BOTTOM_OFFSET;
    EmitPanel(w, fmt, D2D1::RectF(cx - TOAST_OVERLAY_WIDTH / 2, bottom_y - h, cx + TOAST_OVERLAY_WIDTH / 2, bottom_y),
              D2D1::SizeF(TOAST_OVERLAY_WIDTH, h), TOAST_OVERLAY_CORNER, dark ? 0.85f : 0.7f, message);
}

void EmitSearchBar(CmdWriter w, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* count_fmt, const SearchBarLayout& sbl, const SearchBarRenderState& sb,
                   const SearchInputMetrics& input)
{
    auto& out = w.out;
    const D2D1_RECT_F& bar_rect = sbl.bar_rect;
    out.push_back(FillRectCmd{ .rect = bar_rect, .brush_id = BrushId::SearchBarBg });
    out.push_back(DrawLineCmd{
        .p0 = D2D1::Point2F(bar_rect.left, bar_rect.top), .p1 = D2D1::Point2F(bar_rect.right, bar_rect.top), .stroke_width = 1.0f, .brush_id = BrushId::SearchBarBorder });

    w.Centered(L"\uE721", icon_fmt, sbl.icon_rect, BrushId::SearchInputText, 0.6f);

    const bool no_match = !sb.query.empty() && sb.total_matches == 0;
    out.push_back(FillRoundedRectCmd{
        .rect = sbl.input_rect, .radius = SEARCH_BAR_CORNER, .brush_id = no_match ? BrushId::SearchNoMatchBg : BrushId::SearchInputBg });
    // 表示中は入力欄が常にフォーカスを持つので、フォーカス枠で描く。
    out.push_back(DrawRoundedRectCmd{ .rect = sbl.input_rect, .radius = SEARCH_BAR_CORNER, .stroke_width = 1.5f, .brush_id = BrushId::Link });

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
        w.Button(icon_fmt, r, icon, bg, BrushId::SearchInputText, alpha);
    };

    const auto draw_toggle_btn = [&](const D2D1_RECT_F& r, std::wstring_view label, IDWriteTextFormat* fmt, bool checked, bool hovered) {
        const BrushId bg = checked ? BrushId::TitleBarButtonActive : hovered ? BrushId::TitleBarButtonHover
                                                                             : BrushId::None;
        w.Button(fmt, r, label, { bg, SEARCH_BAR_CORNER }, BrushId::SearchInputText, checked ? 1.0f : 0.5f);
    };

    const float nav_alpha = sb.total_matches > 0 ? 1.0f : 0.3f;
    draw_icon_btn(sbl.up_btn, L"\uE70E", sb.hovered == SearchBarHitZone::Up, nav_alpha);
    draw_icon_btn(sbl.down_btn, L"\uE70D", sb.hovered == SearchBarHitZone::Down, nav_alpha);

    if (!sb.query.empty() && sb.total_matches >= 0) {
        // 「N / M」形式で表示。M は実用上 4-5 桁に収まる (md 内 hit 数) ので 32 wchar_t は十分な余裕。
        constexpr size_t kCountBufLen = 32;
        wchar_t buf[kCountBufLen];
        std::wstring_view count_text = L"0";
        if (sb.total_matches > 0) {
            const auto r = std::format_to_n(buf, kCountBufLen - 1, L"{} / {}", sb.current_match + 1, sb.total_matches);
            count_text = { buf, static_cast<size_t>(r.out - buf) };
        }
        w.Centered(count_text, count_fmt, sbl.count_rect, BrushId::SearchInputText, 0.7f);
    }

    draw_toggle_btn(sbl.case_btn, L"Aa", count_fmt, sb.case_sensitive, sb.hovered == SearchBarHitZone::CaseSensitive);
    draw_toggle_btn(sbl.highlight_btn, L"\uE7E6", icon_fmt, sb.highlight_enabled, sb.hovered == SearchBarHitZone::Highlight);
    draw_icon_btn(sbl.close_btn, L"\uE8BB", sb.hovered == SearchBarHitZone::Close);
}

} // namespace overlay

namespace chrome {

void EmitTitleBar(CmdWriter w, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* title_fmt, ID2D1Bitmap* app_icon, bool dark, const TitleBarRenderState& tb)
{
    const TitleBar& bar = tb.bar;
    // 完全不透明でガラス効果を隠す
    w.out.push_back(FillRectCmd{ .rect = D2D1::RectF(0.0f, 0.0f, tb.window_width, TitleBar::BASE_HEIGHT), .brush_id = BrushId::TitleBarBg });

    const float text_alpha = tb.window_active ? 1.0f : 0.5f;
    const auto is_hovered = [&](TitleBarHitZone z) noexcept { return tb.hovered_zone == z; };

    const auto gen_button = [&](const D2D1_RECT_F& rect, wchar_t icon, BrushId bg_id, BrushId text_id, float alpha) {
        w.Button(icon_fmt, rect, { &icon, 1 }, { bg_id }, text_id, alpha);
    };

    // active > hover の優先度
    const auto gen_zone = [&](TitleBarHitZone zone, wchar_t icon, bool active = false) {
        gen_button(
            bar.GetButton(zone),
            icon,
            active ? BrushId::TitleBarButtonActive : is_hovered(zone) ? BrushId::TitleBarButtonHover
                                                                      : BrushId::None,
            BrushId::TitleBarText,
            text_alpha);
    };

    gen_zone(TitleBarHitZone::OpenFile, L'');
    gen_zone(TitleBarHitZone::Help, L'');
    gen_zone(TitleBarHitZone::Settings, L'');
    gen_zone(TitleBarHitZone::ThemeToggle, dark ? L'' : L'');
    gen_zone(TitleBarHitZone::Search, L'', tb.search_active);
    gen_zone(TitleBarHitZone::FileToggle, L'', tb.file_pane_visible);
    gen_zone(TitleBarHitZone::TocToggle, L'', tb.toc_pane_visible);
    gen_zone(TitleBarHitZone::Minimize, L'');
    gen_zone(TitleBarHitZone::Maximize, tb.is_maximized ? L'' : L'');
    if (is_hovered(TitleBarHitZone::Close)) {
        gen_button(bar.GetButton(TitleBarHitZone::Close), L'', BrushId::TitleBarCloseRed, BrushId::OverlayWhite, 1.0f);
    }
    else {
        gen_zone(TitleBarHitZone::Close, L'');
    }

    if (app_icon) {
        w.out.push_back(DrawBitmapCmd{ .bitmap = app_icon, .dest = TitleBar::ICON_RECT, .opacity = text_alpha });
    }

    if (!tb.title_text.empty()) {
        const auto& rect = bar.GetTitleTextRect();
        w.Text(tb.title_text, title_fmt, D2D1::Point2F(rect.left, rect.top), D2D1::SizeF(rect.right - rect.left, rect.bottom - rect.top), BrushId::TitleBarText, text_alpha);
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
void EmitLeftText(CmdWriter w, IDWriteTextFormat* fmt, std::wstring_view text, float x, float y, float height, const FallbackFontMap* fonts)
{
    // 箱の幅で見た目が変わらないので、幅を固定してスプリッタのドラッグ中もレイアウトを使い回す。
    constexpr float kLayoutWidth = 4096.0f;
    w.Text(text, fmt, D2D1::Point2F(x, y), D2D1::SizeF(kLayoutWidth, height), BrushId::Text, 1.0f, fonts);
}

const wchar_t* PaneHeaderButtonIcon(PaneHeaderButton button) noexcept
{
    switch (button) {
    case PaneHeaderButton::Close:
        return L"";
    case PaneHeaderButton::Refresh:
        return L"";
    case PaneHeaderButton::Reveal:
        return L"";
    case PaneHeaderButton::None:
        break;
    }
    std::unreachable();
}

void EmitSidePaneHeader(CmdWriter w, const SidePaneFormats& fmts, float header_h, const SidePaneInstance& pane, bool is_file_pane, std::wstring_view header_text)
{
    const PaneRect& r = pane.rect;
    w.out.push_back(FillRectCmd{ .rect = D2D1::RectF(r.x, r.y, r.x + r.width, r.y + header_h), .brush_id = BrushId::Splitter });

    ForEachPaneHeaderButton(r.width, header_h, is_file_pane, pane.reveal_enabled, [&](const D2D1_RECT_F& local, PaneHeaderButton button, bool enabled) {
        // 無効なボタンはホバー状態が残っていても強調しない (ヘルプへ切替直後など)。
        const bool hovered = enabled && pane.hovered_button == button;
        w.Button(fmts.header_icon, OffsetRectF(local, r.x, r.y), PaneHeaderButtonIcon(button), { hovered ? BrushId::PaneItemHover : BrushId::None }, BrushId::Text,
                 enabled ? 1.0f : DISABLED_UI_ALPHA);
    });

    // ボタンに重ならないよう切る。
    if (const auto text_rect = PaneHeaderTextRect(r.width, header_h, is_file_pane)) {
        const D2D1_RECT_F clip = OffsetRectF(*text_rect, r.x, r.y);
        w.out.push_back(PushClipCmd{ clip });
        EmitLeftText(w, fmts.header, header_text, clip.left, clip.top, text_rect->bottom - text_rect->top, nullptr);
        w.out.push_back(PopClipCmd{});
    }
}

// 項目のはみ出しは内容領域のクリップ 1 回で切り、項目ごとの Push/Pop を避ける。
template <typename GenItemFn>
void EmitSidePane(CmdWriter w, const SidePaneFormats& fmts, float header_h, const SidePaneInstance& pane, bool is_file_pane, size_t item_count,
                  std::wstring_view header_text, GenItemFn gen_item)
{
    const PaneRect& r = pane.rect;
    if (r.width <= 0 || r.height <= 0) {
        return;
    }
    auto& out = w.out;
    out.push_back(FillRectCmd{ .rect = ToD2DRect(r), .brush_id = BrushId::PaneBg });

    EmitSidePaneHeader(w, fmts, header_h, pane, is_file_pane, header_text);

    const float item_h = pane.item_height;
    const float content_top = PaneContentTop(r, header_h);
    const float scroll_y = pane.scroll_y;
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

void EmitFileExplorer(CmdWriter w, const SidePaneFormats& fmts, float header_h, const std::vector<FileEntry>& entries, const SidePaneInstance& pane)
{
    const float icon_col_width = 24.0f * pane.zoom;
    const float pad = 4.0f * pane.zoom;
    EmitSidePane(w, fmts, header_h, pane, true, entries.size(), i18n::S().pane_header_files, [&](int i, const D2D1_RECT_F& row) {
        const auto& entry = entries[i];

        if (entry.is_current) {
            w.out.push_back(FillRectCmd{ .rect = row, .brush_id = BrushId::PaneItemActive });
        }
        else if (i == pane.hovered_index) {
            w.out.push_back(FillRectCmd{ .rect = row, .brush_id = BrushId::PaneItemHover });
        }

        w.Text(FileEntryIcon(entry), fmts.file_icon, D2D1::Point2F(row.left + pad, row.top), D2D1::SizeF(icon_col_width, pane.item_height), BrushId::Text);

        EmitLeftText(w, fmts.file_item, entry.GetDisplayName(), row.left + pad + icon_col_width, row.top, pane.item_height, fmts.item_fonts);
    });
}

void EmitToc(CmdWriter w, const SidePaneFormats& fmts, float header_h, const SidePaneState& sp, const SidePaneInstance& pane)
{
    const auto& entries = sp.toc_entries;
    const int active_index = sp.active_toc_index;
    std::wstring title;
    EmitSidePane(w, fmts, header_h, pane, false, entries.size(), i18n::S().pane_header_toc, [&](int i, const D2D1_RECT_F& row) {
        const Node& heading = sp.nodes[entries[i].node_index];

        if (i == active_index || i == pane.hovered_index) {
            const auto bid = (i == active_index) ? BrushId::PaneItemActive : BrushId::PaneItemHover;
            w.out.push_back(FillRectCmd{ .rect = row, .brush_id = bid });
        }

        const float text_left = row.left + (8.0f + (heading.heading_level - 1) * TOC_INDENT_PER_LEVEL) * pane.zoom;
        // 項目の文字列は持たない。見えている 40 行を描くたびに変換して 1 フレーム約 4µs (BM_TocFrame)。
        DecodeForDWrite(TocItemText(heading.GetText()), title);
        EmitLeftText(w, fmts.toc_item, title, text_left, row.top, pane.item_height, fmts.item_fonts);

        if (i == active_index) {
            const float line_y = row.bottom - 1.0f;
            w.out.push_back(DrawLineCmd{
                .p0 = D2D1::Point2F(text_left, line_y),
                .p1 = D2D1::Point2F(row.right - 4.0f, line_y),
                .stroke_width = 1.5f,
                .brush_id = BrushId::Text,
            });
        }
    });
}

} // namespace

void EmitSidePanes(CmdWriter w, const SidePaneFormats& fmts, const Theme& theme, const SidePaneState& sp)
{
    const float header_h = theme.pane_header_height;
    const auto gen_splitter = [&](const PaneRect& r) {
        const float x = r.x + r.width;
        w.out.push_back(FillRectCmd{ .rect = D2D1::RectF(x, r.y, x + theme.splitter_width, r.y + r.height), .brush_id = BrushId::Splitter });
    };
    if (const auto& fp = sp.Get(PaneTarget::File); fp.show) {
        EmitFileExplorer(w, fmts, header_h, sp.file_entries, fp);
        gen_splitter(fp.rect);
    }
    if (const auto& tp = sp.Get(PaneTarget::Toc); tp.show) {
        EmitToc(w, fmts, header_h, sp, tp);
        gen_splitter(tp.rect);
    }
}

} // namespace chrome

namespace {

BrushId GetNodeBaseBrush(const Node& node) noexcept
{
    switch (node.type) {
    case NodeType::Heading:
        return BrushId::Heading;
    case NodeType::Paragraph:
        return (node.quote_depth > 0 && node.alert_type == AlertType::None) ? BrushId::BlockquoteText : BrushId::Text;
    case NodeType::CodeBlock:
        return BrushId::CodeText;
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

// ノードの [text_top, text_top + height] の外へはみ出して描く量の全種別での上限
// (見出し下線・空 LI の bullet・コード背景・引用装飾等)。ノード単位のカリングはこの余裕幅で
// 判定し、実際の切り抜きは md ペインのクリップに任せる。
struct NodeOverhang {
    float above;
    float below;
};

NodeOverhang MaxNodeOverhang(const Theme& theme) noexcept
{
    const float underline = HeadingUnderlineGap(theme) + std::max(theme.GetHeadingUnderlineThickness(1), theme.GetHeadingUnderlineThickness(2));
    // 空 LI は height=0 だが bullet/checkbox は text_top より下に描かれる。
    const float empty_li = layout::EmptyListItemHeight(theme);
    // コードブロックの padding 内の装飾・アラート背景・引用バーは上下両方へ出る。
    const float box = std::max({ theme.code_block_padding, ALERT_BG_PAD, BAR_EXTEND });
    // コピー/保存ボタンはズームに依らない固定サイズで box 上端から下へ伸びるため、縮小表示の
    // 低いブロックでは box の下へはみ出す。ノード上端からのボタン下端までを下側の上限に含める。
    const float copy_button = COPY_BTN_MARGIN + COPY_BTN_SIZE;
    return { box, std::max({ box, underline, empty_li, copy_button }) };
}

// ノードのマッチ。spill_length_w は前のノードのマッチ (first_global - 1 番) がこのノードの先頭へはみ出した長さ。
struct NodeSearchMatches {
    std::span<const SearchMatch> matches;
    size_t first_global = 0;
    uint32_t spill_length_w = 0;
};

// 本文ペインの 1 フレームぶんの描画命令を積む。GenerateMdPane の呼び出しの間だけ生きる。
struct MdPaneEmitter : CmdWriter {
    const MdPaneParams& p;
    const Theme& theme;
    const MdPaneFormats& formats;
    std::vector<DWRITE_HIT_TEST_METRICS>& hit_test_buffer;
    float content_width;
    float dpi_scale;
    float snapped_scroll_y;
    // 可視 Y 範囲 (ペインローカル = 0 〜 pane_height)。entry_text_top も同じローカル Y 系で、
    // ドキュメント Y が 10^7 オーダーで float32 が桁落ちする問題 (#216) を避ける。ノード・行・
    // ハイライト矩形・グリフランのカリングに共通で使う。
    YRange visible;
    // ブロック横スクロールの clip 内で平行移動を合成し、抜けたあと復元するための基底 Translation。
    D2D1::Matrix3x2F pane_transform;
    NodeOverhang overhang;

    void GenerateNode(const Node& node, const NodeLayoutEntry& entry, const DiagramEntry* diagram, int node_index, float entry_text_top)
    {
        const float x = layout::NodeBlockX(node, theme);
        const float cw = NodeContentWidth(node, theme, content_width);
        const float text_x = x + NodeTextXOffset(node, theme);

        // ブロック横スクロール対象 (テーブル/コードブロック) の内容を clip + 平行移動で囲み、
        // 直後にスクロールバーを積む。幾何はヒットテスト/reducer と共有の GetBlockHScrollGeometry に寄せる。
        const auto gen_h_scroll_block = [&](const D2D1_RECT_F& clip, float pad_y, auto&& gen_content) {
            const auto geom = GetBlockHScrollGeometry(p.nodes, p.cache, static_cast<size_t>(node_index), cw);
            const float scroll_x = LookupBlockScrollX(p.block_h_scroll.scroll_x, geom);
            // スクロールできるなら scroll_x が 0 でも右端で切る (ペインの余白に内容がはみ出る)。
            if (geom.can_scroll()) {
                out.emplace_back(PushClipCmd{ clip });
            }
            if (scroll_x > 0.0f) {
                out.emplace_back(SetTransformCmd{ pane_transform * D2D1::Matrix3x2F::Translation(-scroll_x, 0) });
            }
            gen_content(scroll_x);
            if (scroll_x > 0.0f) {
                out.emplace_back(SetTransformCmd{ pane_transform });
            }
            if (geom.can_scroll()) {
                out.emplace_back(PopClipCmd{});
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
                out.emplace_back(DrawBitmapCmd{ diagram->bitmap.Get(), D2D1::RectF(x, entry_text_top, x + draw_w, entry_text_top + draw_h) });
            }
            else if (diagram && diagram->error.kind == DiagramError::Kind::ImageUnavailable) {
                GenImageAlt(x, entry_text_top, cw, entry.height, diagram->error.message);
            }
            else {
                GenDiagramPlaceholder(x, entry_text_top, cw, entry.height);
            }
            return;

        case NodeType::Table:
            gen_h_scroll_block(
                D2D1::RectF(x, entry_text_top, x + cw, entry_text_top + entry.height),
                NodeBoxPadBottom(node, theme),
                [&](float scroll_x) { GenTable(node, entry, node_index, x, cw, entry_text_top, scroll_x); });
            return;

        case NodeType::Diagram:
            if (diagram && diagram->commands) {
                const auto dest = DiagramRect(diagram->width, diagram->height, x, cw, entry_text_top);
                out.emplace_back(DrawDiagramCmd{ diagram->commands.Get(), dest, diagram->width });
                GenOverlayButtons(node, x, cw, entry_text_top, node_index);
            }
            else {
                GenDiagramPlaceholder(x, entry_text_top, cw, entry.height, diagram ? DiagramErrorText(diagram->error) : std::wstring_view{});
            }
            return;

        case NodeType::CodeBlock: {
            const float pad_bottom = NodeBoxPadBottom(node, theme);
            const D2D1_RECT_F box = D2D1::RectF(x, entry_text_top - NodeBoxPadTop(node, theme), x + cw, entry_text_top + entry.height + pad_bottom);
            GenCodeBlockBg(box, !node.continues_prev(), !node.continues_next());
            // 背景とコピーボタンはクリップ外で固定描画 (GitHub と同じ挙動)。テキスト本体だけ scroll_x 分平行移動。
            gen_h_scroll_block(box, pad_bottom, [&](float scroll_x) {
                GenNodeTextDecorations(node, entry, node_index, text_x, entry_text_top, box.left + scroll_x, box.right + scroll_x);
            });
            GenOverlayButtons(node, x, cw, entry_text_top, node_index);
            return;
        }

        case NodeType::ListItem:
            GenListBullet(node, entry, x, entry_text_top);
            break;

        case NodeType::TaskListItem:
            GenTaskListCheckbox(node, x, entry_text_top);
            break;

        case NodeType::Heading: {
            const int8_t lv = node.heading_level;
            if (lv <= 2) {
                const float line_y = entry_text_top + entry.height + HeadingUnderlineGap(theme);
                out.emplace_back(DrawLineCmd{
                    D2D1::Point2F(x, line_y), D2D1::Point2F(x + cw, line_y),
                    theme.GetHeadingUnderlineThickness(lv), BrushId::Hr });
            }
            break;
        }

        case NodeType::Paragraph:
            // 引用のバーと背景はグループ単位で GenBlockQuoteGroupDecorations から描画済み
            break;
        }

        GenNodeTextDecorations(node, entry, node_index, text_x, entry_text_top);
    }

    // ベースカラー、インラインコード背景、検索/選択ハイライト、本文テキストを描画する。
    // コードブロックは横スクロールの clip に映る [cull_left, cull_right] (text_x と同じ系) を渡す。
    void GenNodeTextDecorations(const Node& node, const NodeLayoutEntry& entry, int node_index, float text_x, float entry_text_top,
                                float cull_left = -YRange::kInf, float cull_right = YRange::kInf)
    {
        if (!entry.text_layout()) {
            return;
        }

        GenInlineCodeBgs(out, entry.view_inline_code_bgs(), text_x, entry_text_top, visible);

        // 背の高いノードでは検索と選択を可視範囲の前後の行に絞って引き、全行選択でも矩形を可視行数ぶんで済ませる。
        const auto nm = FindNodeSearchMatches(node_index);
        const bool has_matches = !nm.matches.empty() || nm.spill_length_w != 0;
        const auto [sel_start, sel_end] = p.selection.RangeIn(node_index, node.GetText().size());
        if (has_matches || sel_end > sel_start) {
            const auto lines = TextRangeOfVisibleLines(entry, visible.ToLocal(entry_text_top));
            // 選択が最前面になるよう、検索マッチを先に塗る。
            if (has_matches) {
                GenSearchHighlights(entry, nm, lines, text_x, entry_text_top);
            }
            if (sel_end > sel_start) {
                GenSelectionHighlight(node, entry, sel_start, sel_end - sel_start, lines, text_x, entry_text_top);
            }
        }

        auto cmd = MakeTextLayoutCmd(D2D1::Point2F(text_x, entry_text_top), entry.text_layout(), entry.height, GetNodeBaseBrush(node), visible);
        cmd.syntax_tokens = entry.syntax_tokens();
        cmd.cull_left = cull_left;
        cmd.cull_right = cull_right;
        out.emplace_back(cmd);
    }

    // text_layout 非依存。loose task list (空 TaskListItem) でも GenNodeTextDecorations の
    // early return を経由せず描画したいため独立メソッド化。
    void GenTaskListCheckbox(const Node& node, float x, float entry_text_top)
    {
        const std::wstring_view icon = node.task_checked() ? L"\u2611" : L"\u2610"; // ☑ / ☐
        const float icon_size = theme.font_size_body;
        const float cb_x = x - theme.list_bullet_offset;
        Text(icon, formats.checkbox.Get(), D2D1::Point2F(cb_x, entry_text_top),
             D2D1::SizeF(icon_size, icon_size * TASK_CHECKBOX_HEIGHT_FACTOR), BrushId::Text);
    }

    void GenHorizontalRule(float x, float w, float entry_text_top)
    {
        const float y = entry_text_top + theme.paragraph_spacing * 0.5f;
        out.emplace_back(DrawLineCmd{
            D2D1::Point2F(x, y), D2D1::Point2F(x + w, y),
            theme.hr_thickness, BrushId::Hr });
    }

    // box は padding 込みのコードブロック矩形 (ペインローカル)。
    // 区切ったコードブロックの継ぎ目の側は角を丸めず、隣の背景とつなげる。
    void GenCodeBlockBg(const D2D1_RECT_F& box, bool round_top, bool round_bottom)
    {
        if (!round_top && !round_bottom) {
            out.emplace_back(FillRectCmd{ box, BrushId::CodeBg });
            return;
        }
        out.emplace_back(FillRoundedRectCmd{ box, CODE_BLOCK_CORNER, BrushId::CodeBg });
        // 背景は不透明なので、丸めない側の角を矩形で塗りつぶす。
        if (!round_top) {
            out.emplace_back(FillRectCmd{ D2D1::RectF(box.left, box.top, box.right, box.top + CODE_BLOCK_CORNER), BrushId::CodeBg });
        }
        if (!round_bottom) {
            out.emplace_back(FillRectCmd{ D2D1::RectF(box.left, box.bottom - CODE_BLOCK_CORNER, box.right, box.bottom), BrushId::CodeBg });
        }
    }

    // ブロックローカルの水平スクロールバー。ホバー中 / ドラッグ中の対象ブロックでのみ emit する。
    // block_x はブロック左端、bar_y はバー上端 (ペイン内ローカル座標)。
    void EmitBlockHScrollbarIfActive(int node_index, float block_x, float bar_y, const BlockHScrollGeometry& geom, float scroll_x)
    {
        if (!geom.can_scroll()) {
            return;
        }
        if (node_index != p.block_h_scroll.hovered_block && node_index != p.block_h_scroll.drag_block) {
            return;
        }
        const float track_w = geom.visible_width;
        const float thumb_w = BlockHScrollbarThumbWidth(geom.visible_width, geom.block_width);
        const float scroll_max = geom.scroll_max();
        const float ratio = std::clamp(scroll_x / scroll_max, 0.0f, 1.0f);
        // サブピクセル位置だとアンチエイリアスが上下に漏れて thumb の太さがブレるため snap する。
        const float thumb_x = SnapToPhysicalPixel(block_x + ratio * (track_w - thumb_w), dpi_scale);
        const float thumb_y = SnapToPhysicalPixel(bar_y, dpi_scale);
        const float w_snapped = SnapToPhysicalPixel(thumb_w, dpi_scale);
        const float h_snapped = SnapToPhysicalPixel(PANE_SCROLLBAR_WIDTH, dpi_scale);
        out.emplace_back(FillRoundedRectCmd{
            D2D1::RectF(thumb_x, thumb_y, thumb_x + w_snapped, thumb_y + h_snapped),
            h_snapped / 2.0f,
            BrushId::ScrollbarThumb });
    }

    void GenOverlayButtons(const Node& node, float x, float cw, float entry_text_top, int node_index)
    {
        // 図のボタンは普段は隠し、マウスが図の画像領域かボタンの上にあるときだけ出す。
        if (node.type == NodeType::Diagram && p.hovered_diagram != node_index) {
            return;
        }
        layout::ForEachOverlayButton(node, p.cache, static_cast<size_t>(node_index), x, cw, entry_text_top, theme, [&](const D2D1_RECT_F& btn, OverlayButton kind) {
            // 保存は Download、拡大表示は FullScreen、コピーは Copy (Segoe Fluent Icons)。
            const wchar_t icon = kind == OverlayButton::DiagramSave ? L'\uE896' : kind == OverlayButton::DiagramView ? L'\uE740'
                                                                                                                     : L'\uE8C8';
            GenOverlayButton(btn, icon, p.hovered == OverlayButtonHit{ node_index, kind });
        });
    }

    void GenOverlayButton(D2D1_RECT_F btn, wchar_t icon, bool is_hovered)
    {
        if (!formats.copy_btn_icon) {
            return;
        }
        const bool dark = theme.dark;
        const float bg_alpha = is_hovered ? (dark ? 0.30f : 0.15f) : (dark ? 0.10f : 0.05f);
        const float text_alpha = is_hovered ? (dark ? 0.9f : 0.8f) : (dark ? 0.4f : 0.35f);
        Button(formats.copy_btn_icon.Get(), btn, { &icon, 1 }, { BrushId::Overlay, COPY_BTN_CORNER, bg_alpha }, BrushId::Overlay, text_alpha);
    }

    void GenListBullet(const Node& node, const NodeLayoutEntry& entry, float x, float entry_text_top)
    {
        const float first_line_h = GetFirstLineHeight(entry, theme.font_size_body);
        if (node.list_ordered()) {
            std::array<wchar_t, kListNumberMaxLen> num_buf;
            Text(FormatListNumber(num_buf, node.list_number(), node.list_depth()), formats.list_number.Get(),
                 D2D1::Point2F(x - theme.list_bullet_offset - LIST_NUMBER_PAD_RIGHT, entry_text_top),
                 D2D1::SizeF(theme.list_bullet_offset + LIST_NUMBER_PAD_RIGHT - LIST_NUMBER_PAD_LEFT, first_line_h),
                 BrushId::Text);
            return;
        }

        const float bullet_x = SnapToPhysicalPixel(x - theme.list_bullet_offset * LIST_BULLET_X_FACTOR, dpi_scale);
        const float bullet_y = SnapToPhysicalPixel(entry_text_top + first_line_h * 0.5f, dpi_scale);
        const float r = theme.list_bullet_radius;
        // 中心は物理ピクセル境界にあるので、四角の半辺も物理ピクセル単位にして辺をにじませない。
        const float h = SnapToPhysicalPixel(r * LIST_SQUARE_HALF_FACTOR, dpi_scale);
        switch (ListStyleIndex(node.list_depth(), kListBulletStyles)) {
        case 0:
            out.emplace_back(FillRoundedRectCmd{ SquareAround(bullet_x, bullet_y, r), r, BrushId::Text });
            break;
        case 1:
            out.emplace_back(DrawRoundedRectCmd{ SquareAround(bullet_x, bullet_y, r), r, 1.0f, BrushId::Text });
            break;
        case 2:
            out.emplace_back(FillRectCmd{ SquareAround(bullet_x, bullet_y, h), BrushId::Text });
            break;
        default: {
            // 線は辺を中心に引かれるので、太さを物理ピクセルの整数にして半分だけ内側に引き、■ と同じ外形で画素に揃える。
            const float stroke = std::max(1.0f, std::round(dpi_scale)) / dpi_scale;
            const float e = h - stroke * 0.5f;
            out.emplace_back(DrawRoundedRectCmd{ SquareAround(bullet_x, bullet_y, e), 0.0f, stroke, BrushId::Text });
            break;
        }
        }
    }

    void GenBlockQuoteGroupDecorations(int first_visible)
    {
        const auto& nodes = p.nodes;
        const auto& cache = p.cache;
        const float snap = snapped_scroll_y;
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
                if (cache.Top(i) - snap > visible.bottom) {
                    break;
                }
                i++;
                continue;
            }
            const float group_top = cache.Top(i) - snap;
            if (group_top - overhang.above > visible.bottom) {
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
                if (group_bottom > visible.bottom) {
                    break;
                }
            }

            GenBlockQuoteGroup(i, j, max_depth, group_top, group_bottom);
            // 下端で打ち切ったグループの残りを別グループとして描かない。
            if (j < node_count && nodes[j].blockquote_group == group) {
                break;
            }
            i = j;
        }
    }

    // nodes[first, last) が 1 つの引用グループ。group_top / group_bottom はペインローカル Y。
    void GenBlockQuoteGroup(int first, int last, int max_depth, float group_top, float group_bottom)
    {
        const auto& nodes = p.nodes;
        const auto& cache = p.cache;
        static constexpr float ALERT_BG_CORNER = 4.0f;

        const AlertType alert_type = nodes[first].alert_type;
        const int outer_indent = nodes[first].quote_outer_indent;
        const float outer_x_indent = static_cast<float>(outer_indent) * theme.indent_width;
        if (const auto idx = AlertColorIndex(alert_type); idx < ALERT_TYPE_COUNT) {
            const float outer_x = theme.margin_left + outer_x_indent;
            const float cw = ShrinkKeepingMinContent(content_width, outer_x_indent);
            const D2D1_RECT_F bg_rect = D2D1::RectF(outer_x - ALERT_BG_PAD, group_top - ALERT_BG_PAD, outer_x + cw, group_bottom + ALERT_BG_PAD);
            out.emplace_back(FillRoundedRectCmd{ bg_rect, ALERT_BG_CORNER, AlertBgBrushIdFromIndex(idx) });
        }

        // GitHub と同様に、外側のバーはネストした範囲を貫通して連続描画する。
        // 各 level ごとに quote_depth >= level の連続区間を求めてバーを発行。
        for (int level = 1; level <= max_depth; ++level) {
            const float bar_indent_x = static_cast<float>(outer_indent + level - 1) * theme.indent_width;
            const float bar_x = theme.margin_left + bar_indent_x - theme.indent_width * 0.5f;
            const bool is_alert_bar = (level == 1 && alert_type != AlertType::None);
            const BrushId bar_brush = is_alert_bar ? AlertBrushIdFromIndex(AlertColorIndex(alert_type)) : BrushId::BlockquoteBar;

            const auto emit_bar = [&](float top, float bottom) {
                out.emplace_back(DrawLineCmd{
                    D2D1::Point2F(bar_x, top - BAR_EXTEND),
                    D2D1::Point2F(bar_x, bottom + BAR_EXTEND),
                    theme.blockquote_bar_width, bar_brush });
            };

            bool in_region = false;
            float region_top = 0.0f;
            float region_bottom = 0.0f;
            for (int k = first; k < last; ++k) {
                const float local_top_k = cache.Top(k) - snapped_scroll_y;
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

    // 読めない画像は alt を枠で囲んで出す。ブラウザの壊れた画像と同じく枠は文字に合わせ、内容幅に収まらない分は省略記号で切る。
    void GenImageAlt(float x, float y, float cw, float h, std::wstring_view alt)
    {
        const float pad = layout::ImageAltPadding(theme);
        auto* const layout = text_layouts.Get(formats.image_alt.Get(), alt, D2D1::SizeF(std::max(cw - 2.0f * pad, 0.0f), h));
        DWRITE_TEXT_METRICS m{};
        if (layout) {
            layout->GetMetrics(&m);
        }
        const float w = std::min(m.width + 2.0f * pad, cw);
        constexpr float kStroke = 1.0f;
        constexpr float kHalf = kStroke * 0.5f;
        out.emplace_back(DrawRoundedRectCmd{ D2D1::RectF(x + kHalf, y + kHalf, x + w - kHalf, y + h - kHalf), CODE_BLOCK_CORNER, kStroke, BrushId::Hr });
        if (layout) {
            out.push_back(DrawTextLayoutCmd{ .origin = D2D1::Point2F(x + pad - m.left, y), .layout = layout, .brush_id = BrushId::BlockquoteText });
        }
    }

    // error 非空時は「読み込み中...」の代わりにエラーメッセージを表示する (issue #271)。
    void GenDiagramPlaceholder(float x, float y, float w, float h, std::wstring_view error = {})
    {
        GenCodeBlockBg(D2D1::RectF(x, y, x + w, y + h), true, true);
        const auto origin = D2D1::Point2F(x, y);
        const auto box = D2D1::SizeF(w, h);
        if (!error.empty()) {
            Text(error, formats.placeholder_text.Get(), origin, box, BrushId::AlertCaution);
            return;
        }
        Text(i18n::S().loading, formats.placeholder_text.Get(), origin, box, BrushId::BlockquoteText);
    }

    // selection はテーブル全体の選択範囲 (concat_text 上の UTF-8 byte)。
    void GenCellSelection(const NodeTableData& table, size_t row, size_t col, IDWriteTextLayout* layout, TextSelection::NodeRange selection, float text_x, float text_y)
    {
        const std::string_view cell_text = table.GetCellText(row, col);
        const uint32_t cell_start = table.CellTextStart(row, col);
        const uint32_t ov_start = std::max(selection.start, cell_start);
        const uint32_t ov_end = std::min(selection.end, cell_start + static_cast<uint32_t>(cell_text.size()));
        if (ov_end <= ov_start) {
            return;
        }
        // ov_start/ov_end は UTF-8 byte offset。HitTestTextRange 用に UTF-16 へ変換する。
        Utf16OffsetCursor cursor{ cell_text };
        auto range = cursor.WideRange(ov_start - cell_start, ov_end - ov_start);
        // セルの選択は毎フレーム引くので、広げる (HitTestTextPosition 2 回) のは数式のあるセルだけにする。
        if (std::ranges::any_of(table.GetCellRuns(row, col), &TextRun::math)) {
            range = ExpandToClusters(layout, range.startPosition, range.length);
        }
        if (range.length > 0) {
            EmitRangeRects(layout, range, text_x, text_y, BrushId::Selection);
        }
    }

    // 表は横スクロールの clip [offset_x, offset_x + clip_width] の中に描くので、列はその幅で間引く。
    void GenTable(const Node& node, const NodeLayoutEntry& entry,
                  int node_index, float offset_x, float clip_width, float entry_text_top, float h_scroll_x)
    {
        const auto* tbl = node.table_data();
        const auto* tl_ptr = entry.table_layout();
        if (!tl_ptr) {
            return;
        }

        const float border = TABLE_BORDER_WIDTH;
        const auto& tl = *tl_ptr;
        const auto row_count = tbl->row_count;

        const float table_width = tl.TableWidth();

        const auto selection = p.selection.RangeIn(node_index, node.GetText().size());

        // 可視行帯を row_cum_y の二分探索で求め、その範囲だけループする
        // (FindTableRow と同じパターン)。巨大テーブルで
        // 毎フレーム row_count 回の continue ループが走るのを防ぐ。
        const YRange local_visible = visible.ToLocal(entry_text_top);
        const auto [r_begin, r_end] = tl.VisibleRowRange(local_visible.top, local_visible.bottom);
        float y = entry_text_top + tl.row_cum_y[r_begin];

        // 横スクロール時は clip に映る範囲が h_scroll_x だけ右にずれる。
        const auto [c_begin, c_end] = tl.VisibleColRange(h_scroll_x + border, h_scroll_x + clip_width + border);

        const auto search_nm = FindNodeSearchMatches(node_index);

        for (size_t r = r_begin; r < r_end; r++) {
            const float row_h = tl.row_heights[r];
            const bool is_header_row = tbl->IsHeaderRow(r);
            if (is_header_row || r % 2 == 0) {
                out.emplace_back(FillRectCmd{ D2D1::RectF(offset_x, y, offset_x + table_width, y + row_h + border),
                                              is_header_row ? BrushId::CodeBg : BrushId::TableStripe });
            }

            out.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, y), D2D1::Point2F(offset_x + table_width, y), border, BrushId::Hr });

            std::span<const CellInlineCodeBg> row_bgs;
            if (r < tl.row_code_bgs.size()) {
                row_bgs = tl.row_code_bgs[r];
            }
            for (size_t c = c_begin; c < c_end; c++) {
                const auto [text_x, text_y] = TableCellTextOrigin(offset_x + tl.col_cum_x[c], y);

                GenCellInlineCodeBgs(out, row_bgs, c, text_x, text_y);

                // evict 中のセルは layout が復元されてから描く。
                if (IDWriteTextLayout* const cell_layout = tl.GetCellLayout(r, c)) {
                    GenCellSearchHighlights(search_nm, cell_layout, r, c, text_x, text_y);
                    if (selection.end > selection.start) {
                        GenCellSelection(*tbl, r, c, cell_layout, selection, text_x, text_y);
                    }
                    out.emplace_back(MakeTextLayoutCmd(D2D1::Point2F(text_x, text_y), cell_layout, row_h, is_header_row ? BrushId::Heading : BrushId::Text, visible));
                }
            }
            y += row_h + border;
        }

        // r_end で打ち切った場合も下端線はテーブル全体の底に置く (可視外ならクリップされる)
        const float table_bottom = entry_text_top + tl.row_cum_y[row_count];
        out.emplace_back(DrawLineCmd{ D2D1::Point2F(offset_x, table_bottom), D2D1::Point2F(offset_x + table_width, table_bottom), border, BrushId::Hr });

        // 縦罫線は可視列ごとに表の上端から下端まで 1 本で引く (行ごとに引くと 行×列 本になる)。可視外はクリップされる。
        const auto vline = [&](float x) {
            out.emplace_back(DrawLineCmd{ D2D1::Point2F(x, entry_text_top), D2D1::Point2F(x, table_bottom), border, BrushId::Hr });
        };
        for (size_t c = c_begin; c < c_end; c++) {
            vline(offset_x + tl.col_cum_x[c] - border);
        }
        vline(offset_x + table_width);
    }

    // 返すメトリクスは共有バッファを指し、次の呼び出しまで有効。
    std::span<const DWRITE_HIT_TEST_METRICS> HitTestRange(IDWriteTextLayout* layout, uint32_t start, uint32_t length)
    {
        const UINT32 count = FetchHitTestMetrics(layout, start, length, hit_test_buffer);
        return { hit_test_buffer.data(), count };
    }

    // range の矩形のうち可視 Y 範囲と重なるものを塗る。
    void EmitRangeRects(IDWriteTextLayout* layout, DWRITE_TEXT_RANGE range, float origin_x, float origin_y, BrushId brush)
    {
        for (const auto& m : HitTestRange(layout, range.startPosition, range.length)) {
            if (const auto r = RectFromHitTest(m, origin_x, origin_y); OverlapsY(r, visible)) {
                out.emplace_back(FillRectCmd{ r, brush });
            }
        }
    }

    // 本文ノード（テーブル外）専用の選択ハイライト発行。doc_start / doc_length は UTF-8 byte 単位。
    // lines は可視範囲の前後の行の文字範囲 (TextRangeOfVisibleLines)。
    void GenSelectionHighlight(const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, std::pair<uint32_t, uint32_t> lines,
                               float origin_x, float origin_y)
    {
        auto* layout = entry.text_layout();
        // 結果は持たない。全選択のまま画面いっぱいの 15KB の和文を描いても 1 フレーム 34〜43µs で (BM_SelectionFrame。
        // キャッシュがあった頃は 0.4µs)、16.6ms に対して小さい。
        Utf16OffsetCursor cursor{ node.GetText() };
        const auto wr = cursor.WideRange(doc_start, doc_length);
        const uint32_t begin = std::max(wr.startPosition, lines.first);
        const uint32_t end = std::min(wr.startPosition + wr.length, lines.second);
        if (end <= begin) {
            return;
        }
        // 数式の一部に当たった選択も数式全体を塗れるよう、クラスタまで広げる。
        EmitRangeRects(layout, ExpandToClusters(layout, begin, end - begin), origin_x, origin_y, BrushId::Selection);
    }

    NodeSearchMatches FindNodeSearchMatches(int node_index) const
    {
        const std::span<const SearchMatch> matches = p.search_matches;
        if (matches.empty()) {
            return {};
        }
        const auto range = std::ranges::equal_range(matches, node_index, {}, &SearchMatch::node_index);
        const size_t first_global = static_cast<size_t>(range.begin() - matches.begin());
        return { matches.subspan(first_global, range.size()), first_global, SpillLengthInto(matches, first_global, node_index) };
    }

    // matches[begin, end) の矩形を layout で引き、FillRectCmd を積む。
    void EmitSearchMatches(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t begin, size_t end, float origin_x, float origin_y)
    {
        for (size_t i = begin; i < end; ++i) {
            const auto& m = nm.matches[i];
            EmitSearchSpan(layout, m.start_w, m.length_w, nm.first_global + i, origin_x, origin_y);
        }
    }

    void EmitSearchSpan(IDWriteTextLayout* layout, uint32_t start_w, uint32_t length_w, size_t global_index, float origin_x, float origin_y)
    {
        const bool is_current = (static_cast<int>(global_index) == p.current_match_index);
        const BrushId hl_brush = is_current ? BrushId::SearchHighlightCurrent : BrushId::SearchHighlight;
        // 結果は持たない。英文の e が画面に 1000 個見えるスクロールで 1 フレーム 13 → 310µs (BM_SearchFrame)。
        // 結合文字の基底だけの検索でもクラスタ全体を塗るので、数式の無いノードでも広げる。
        EmitRangeRects(layout, ExpandToClusters(layout, start_w, length_w), origin_x, origin_y, hl_brush);
    }

    // 本文ノードのマッチのうち lines (可視範囲の前後の行) にかかるものを描く。
    void GenSearchHighlights(const NodeLayoutEntry& entry, const NodeSearchMatches& nm, std::pair<uint32_t, uint32_t> lines, float origin_x, float origin_y)
    {
        // マッチは start_w 昇順で重ならないので、終端も昇順に並ぶ。
        const auto [line_begin, line_end] = lines;
        if (nm.spill_length_w > line_begin) {
            EmitSearchSpan(entry.text_layout(), 0, nm.spill_length_w, nm.first_global - 1, origin_x, origin_y);
        }
        const auto first = std::ranges::partition_point(nm.matches, [b = line_begin](const SearchMatch& m) noexcept {
            return m.start_w + m.length_w <= b;
        });
        const auto last = std::ranges::partition_point(first, nm.matches.end(), [e = line_end](const SearchMatch& m) noexcept {
            return m.start_w < e;
        });
        EmitSearchMatches(nm, entry.text_layout(),
                          static_cast<size_t>(first - nm.matches.begin()), static_cast<size_t>(last - nm.matches.begin()), origin_x, origin_y);
    }

    void GenCellSearchHighlights(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t row, size_t col, float origin_x, float origin_y)
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
};

} // namespace

void CommandGenerator::GenerateMdPane(DrawCommandList& out, const MdPaneParams& p, float dpi_scale)
{
    MENDO_PROFILE("GenerateMdPane");
    const auto& md_pane_rect = p.md_pane_rect;
    const float scroll_y = p.scroll_y;
    const auto& nodes = p.nodes;
    const auto& cache = p.cache;

    out.emplace_back(PushClipCmd{ ToD2DRect(md_pane_rect) });
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
    out.emplace_back(SetTransformCmd{ pane_transform });

    MdPaneEmitter e{
        { out,  *text_layouts_      },
        p,
        *theme_,
        *formats_,
        hit_test_buffer_,
        theme_->ContentWidth(md_pane_rect.width),
        dpi_scale,
        snapped_y,
        { 0.0f, md_pane_rect.height },
        pane_transform,
        MaxNodeOverhang(*theme_),
    };

    const int node_count = static_cast<int>(nodes.size());
    // 二分探索キーは未スナップのドキュメント Y (cache.Top(i) と同じ系)。
    // 下端より下にはみ出す描画 (見出し下線・空 LI の bullet 等) を上端で落とさないよう、
    // はみ出しの上限だけ手前から始める。
    const int first_visible = FindFirstVisibleNodeIndex(cache, scroll_y - e.overhang.below);

    // 同一 blockquote_group のノードをまとめてバー/背景を描画する。
    // ノード単体ごとではなく一括で描画することで、複数行の引用ブロックが連続した見た目になる。
    e.GenBlockQuoteGroupDecorations(first_visible);

    int visible_count = 0;
    for (int i = first_visible; i < node_count; i++) {
        const float local_text_top = cache.Top(i) - snapped_y;
        if (local_text_top - e.overhang.above > e.visible.bottom) {
            break;
        }
        const Node& node = nodes[i];
        const DiagramEntry* diagram = IsBitmapDrawNode(node) ? cache.FindDiagram(i) : nullptr;
        e.GenerateNode(node, cache[i], diagram, i, local_text_top);
        ++visible_count;
    }

    out.emplace_back(SetTransformCmd{ D2D1::Matrix3x2F::Identity() });
    out.emplace_back(PopClipCmd{});
    MENDO_PLOT("cmdgen.visible_node_count", static_cast<int64_t>(visible_count));
}
