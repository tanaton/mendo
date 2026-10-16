#include "renderer.h"
#include "ui_constants.h"
#include "d2d_util.h"
#include <algorithm>
#include <format>
#include <wrl/client.h>

void Renderer::DrawSearchBar(const SearchBarRenderState& sb, const PaneRect& md_pane_rect)
{
    const auto sbl = ComputeSearchBarLayout(md_pane_rect, !sb.query.empty());

    const D2D1_RECT_F bar_rect = D2D1::RectF(
        md_pane_rect.x,
        sbl.bar_top,
        md_pane_rect.x + md_pane_rect.width,
        sbl.bar_bottom);
    rt()->FillRectangle(bar_rect, Brush(BrushId::SearchBarBg));

    rt()->DrawLine(D2D1::Point2F(bar_rect.left, sbl.bar_top), D2D1::Point2F(bar_rect.right, sbl.bar_top), Brush(BrushId::SearchBarBorder), 1.0f);

    DrawCenteredText(L"\uE721", fmt_.search_icon.Get(), sbl.icon_rect, BrushId::SearchInputText, 0.6f);

    const D2D1_ROUNDED_RECT input_rrect = D2D1::RoundedRect(sbl.input_rect, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER);
    const bool no_match = !sb.query.empty() && sb.total_matches == 0;
    rt()->FillRoundedRectangle(input_rrect, Brush(no_match ? BrushId::SearchNoMatchBg : BrushId::SearchInputBg));

    if (sb.has_focus) {
        if (auto* focus_brush = Brush(BrushId::Link)) {
            rt()->DrawRoundedRectangle(input_rrect, focus_brush, 1.5f);
        }
    }
    else if (auto* border_brush = Brush(BrushId::SearchBarBorder)) {
        OpacityScope guard{ border_brush, 0.5f };
        rt()->DrawRoundedRectangle(input_rrect, border_brush, 1.0f);
    }

    const float caret_x = DrawSearchInputText(sb, sbl);

    // コンポジション中はIME側がキャレットを表示するため非表示
    if (sb.caret_visible && sb.ime_composition.empty()) {
        const float x = std::min(caret_x + 1.0f, sbl.text_right());
        rt()->DrawLine(
            D2D1::Point2F(x, sbl.input_rect.top + 3.0f),
            D2D1::Point2F(x, sbl.input_rect.bottom - 3.0f),
            Brush(BrushId::SearchInputText),
            1.0f);
    }

    DrawSearchBarButtons(sb, sbl);
}

IDWriteTextLayout* Renderer::AcquireSearchInputLayout(const SearchBarRenderState& sb, int comp_start, float width, float height)
{
    // IMEコンポジション中は確定済みテキストのキャレット位置に変換中テキストを挿入して表示する。
    // 下線はレイアウト生成時にだけ付けるので、合成範囲もキーに含める。
    auto& text = search_text_scratch_;
    text.assign(sb.query);
    text.insert(static_cast<size_t>(comp_start), sb.ime_composition);
    const DWRITE_TEXT_RANGE underline{ static_cast<UINT32>(comp_start), static_cast<UINT32>(sb.ime_composition.size()) };

    auto& c = search_input_;
    if (c.layout && c.width == width && c.underline.startPosition == underline.startPosition &&
        c.underline.length == underline.length && c.text == text) {
        return c.layout.Get();
    }
    c.layout.Reset();
    if (FAILED(backend_.GetDWriteFactory()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt_.search_input.Get(), width, height, &c.layout))) {
        c = {};
        return nullptr;
    }
    if (underline.length > 0) {
        c.layout->SetUnderline(TRUE, underline);
    }
    c.text.swap(text);
    c.width = width;
    c.underline = underline;
    return c.layout.Get();
}

float Renderer::DrawSearchInputText(const SearchBarRenderState& sb, const SearchBarLayout& sbl)
{
    const float text_left = sbl.text_left();
    const bool has_comp = !sb.ime_composition.empty();
    if (!fmt_.search_input || (sb.query.empty() && !has_comp) || !backend_.GetDWriteFactory()) {
        return text_left;
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

    // レイアウトを1回だけ作成し、描画とキャレット計測で共用する。
    IDWriteTextLayout* const text_layout = AcquireSearchInputLayout(
        sb, comp_start, sbl.text_width(), sbl.input_rect.bottom - sbl.input_rect.top);
    if (!text_layout) {
        return text_left;
    }

    // 文字を隠さないよう、選択範囲はテキストより先に塗る。
    const int text_len = static_cast<int>(search_input_.text.size());
    const bool has_selection = !has_comp && sb.selection_start >= 0 && sb.caret_pos >= 0 && sb.selection_start != sb.caret_pos;
    if (has_selection) {
        const int sel_min = std::clamp(std::min(sb.selection_start, sb.caret_pos), 0, text_len);
        const int sel_max = std::clamp(std::max(sb.selection_start, sb.caret_pos), 0, text_len);
        if (sel_min < sel_max) {
            // 単一行テキストなのでメトリクスは1つで十分
            DWRITE_HIT_TEST_METRICS htm_sel{};
            UINT32 actual = 0;
            text_layout->HitTestTextRange(
                static_cast<UINT32>(sel_min),
                static_cast<UINT32>(sel_max - sel_min),
                text_left, sbl.input_rect.top,
                &htm_sel, 1, &actual);
            if (actual > 0) {
                const D2D1_RECT_F sel_rect = D2D1::RectF(
                    htm_sel.left,
                    sbl.input_rect.top + 2.0f,
                    htm_sel.left + htm_sel.width,
                    sbl.input_rect.bottom - 2.0f);
                rt()->FillRectangle(sel_rect, Brush(BrushId::Selection));
            }
        }
    }

    rt()->DrawTextLayout(
        D2D1::Point2F(text_left, sbl.input_rect.top),
        text_layout,
        Brush(BrushId::SearchInputText));

    // コンポジション中はその末尾、それ以外は通常のキャレット位置
    int effective_pos = has_comp ? comp_start + comp_len : sb.caret_pos;
    if (!has_comp && (effective_pos < 0 || effective_pos > text_len)) {
        effective_pos = text_len;
    }
    FLOAT px, py;
    DWRITE_HIT_TEST_METRICS htm{};
    text_layout->HitTestTextPosition(static_cast<UINT32>(effective_pos), false, &px, &py, &htm);
    return text_left + px;
}

void Renderer::DrawSearchBarButtons(const SearchBarRenderState& sb, const SearchBarLayout& sbl)
{
    const auto draw_icon_btn = [&](const D2D1_RECT_F& r, const wchar_t* icon, bool hovered, float alpha = 1.0f) {
        if (hovered) {
            rt()->FillRoundedRectangle(D2D1::RoundedRect(r, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER), Brush(BrushId::TitleBarButtonHover));
        }
        DrawCenteredText(icon, fmt_.search_icon.Get(), r, BrushId::SearchInputText, alpha);
    };

    const auto draw_toggle_btn = [&](const D2D1_RECT_F& r, std::wstring_view label, IDWriteTextFormat* fmt, bool checked, bool hovered) {
        if (hovered || checked) {
            rt()->FillRoundedRectangle(
                D2D1::RoundedRect(r, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER),
                Brush(checked ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover));
        }
        DrawCenteredText(label, fmt, r, BrushId::SearchInputText, checked ? 1.0f : 0.5f);
    };

    const float nav_alpha = sb.total_matches > 0 ? 1.0f : 0.3f;
    draw_icon_btn(sbl.up_btn, L"\uE70E", sb.hovered == SearchBarHitZone::Up, nav_alpha);
    draw_icon_btn(sbl.down_btn, L"\uE70D", sb.hovered == SearchBarHitZone::Down, nav_alpha);

    if (fmt_.search_count && !sb.query.empty()) {
        // 「N / M」形式で表示。M は実用上 4-5 桁に収まる (md 内 hit 数) ので
        // 32 wchar_t は十分な余裕。format_to_n で先端から書き、長さを返してもらう。
        constexpr size_t kCountBufLen = 32;
        wchar_t count_text[kCountBufLen];
        const auto r = (sb.total_matches == 0)
                           ? std::format_to_n(count_text, kCountBufLen - 1, L"0")
                           : std::format_to_n(count_text, kCountBufLen - 1, L"{} / {}", sb.current_match + 1, sb.total_matches);
        const auto written = static_cast<size_t>(r.out - count_text);
        DrawCenteredText({ count_text, written }, fmt_.search_count.Get(), sbl.count_rect, BrushId::SearchInputText, 0.7f);
    }

    draw_toggle_btn(sbl.case_btn, L"Aa", fmt_.search_count.Get(), sb.case_sensitive, sb.hovered == SearchBarHitZone::CaseSensitive);
    draw_toggle_btn(sbl.highlight_btn, L"\uE7E6", fmt_.search_icon.Get(), sb.highlight_enabled, sb.hovered == SearchBarHitZone::Highlight);
    draw_icon_btn(sbl.close_btn, L"\uE8BB", sb.hovered == SearchBarHitZone::Close);
}

int Renderer::HitTestSearchInput(std::wstring_view query, float local_x, float max_width) const
{
    if (query.empty() || !fmt_.search_input || !backend_.GetDWriteFactory()) {
        return 0;
    }
    // ドラッグ選択中は毎 WM_MOUSEMOVE で呼ばれるため、表示用レイアウトが同じテキスト・幅なら使い回す。
    // IME 合成中は表示テキストが query と異なるので作り直す。
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    const auto& c = search_input_;
    if (c.layout && c.underline.length == 0 && c.width == max_width && c.text == query) {
        layout = c.layout;
    }
    else {
        backend_.GetDWriteFactory()->CreateTextLayout(
            query.data(),
            static_cast<UINT32>(query.size()),
            fmt_.search_input.Get(),
            max_width,
            SEARCH_INPUT_HEIGHT,
            &layout);
    }
    if (!layout) {
        return 0;
    }
    // HitTestPoint が失敗時に out 引数未書込みでも未初期化値を読まないようにする。
    BOOL is_trailing = FALSE;
    BOOL is_inside = FALSE;
    DWRITE_HIT_TEST_METRICS htm{};
    const HRESULT hr = layout->HitTestPoint(local_x, 0.0f, &is_trailing, &is_inside, &htm);
    if (FAILED(hr)) {
        return 0;
    }
    // 結合文字やサロゲートペアの途中に置かないよう、クラスタの長さ分進める。
    return static_cast<int>(htm.textPosition + (is_trailing ? htm.length : 0));
}
