#include "renderer.h"
#include "ui_constants.h"
#include "d2d_util.h"
#include <algorithm>
#include <format>
#include <wrl/client.h>

void Renderer::DrawSearchBar(const SearchBarRenderState& sb, const PaneRect& md_pane_rect)
{
    if (!sb.visible) {
        return;
    }

    const auto sbl = ComputeSearchBarLayout(
        md_pane_rect.x, md_pane_rect.width,
        md_pane_rect.y + md_pane_rect.height,
        !sb.query.empty());

    const D2D1_RECT_F bar_rect = D2D1::RectF(
        md_pane_rect.x,
        sbl.bar_top,
        md_pane_rect.x + md_pane_rect.width,
        sbl.bar_bottom);
    rt()->FillRectangle(bar_rect, Brush(BrushId::SearchBarBg));

    rt()->DrawLine(D2D1::Point2F(bar_rect.left, sbl.bar_top), D2D1::Point2F(bar_rect.right, sbl.bar_top), Brush(BrushId::SearchBarBorder), 1.0f);

    DrawIcon(L"\uE721", fmt_.search_icon.Get(), sbl.icon_rect, BrushId::SearchInputText, 0.6f);

    const D2D1_ROUNDED_RECT input_rrect = D2D1::RoundedRect(sbl.input_rect, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER);
    const bool no_match = !sb.query.empty() && sb.total_matches == 0;
    rt()->FillRoundedRectangle(input_rrect, Brush(no_match ? BrushId::SearchNoMatchBg : BrushId::SearchInputBg));

    // フォーカス時はリンク色で強調
    if (sb.has_focus) {
        if (auto* focus_brush = Brush(BrushId::Link)) {
            rt()->DrawRoundedRectangle(input_rrect, focus_brush, 1.5f);
        }
    }
    else if (auto* border_brush = Brush(BrushId::SearchBarBorder)) {
        mendo::OpacityScope guard{ border_brush, 0.5f };
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

IDWriteTextLayout* Renderer::AcquireSearchInputLayout(const SearchBarRenderState& sb, int comp_start, float width, float height, bool& cache_hit)
{
    auto& c = search_cache_;
    const bool has_comp = !sb.ime_composition.empty();
    const int key_caret_pos = has_comp ? comp_start : -1;

    // 比較は scalar → 空になりやすい ime_comp → query の順で短絡させる
    cache_hit = c.layout && c.width == width && c.caret_pos == key_caret_pos && c.ime_comp == sb.ime_composition && c.query == sb.query;
    if (cache_hit) {
        // 前フレームの下線範囲と異なる可能性があるため、キャッシュ上に下線が残っていれば
        // 常に全体をクリアする。IME 非アクティブ継続時はクリアも発行されない。
        if (c.has_underline) {
            c.layout->SetUnderline(FALSE, DWRITE_TEXT_RANGE{ 0, static_cast<UINT32>(c.text.size()) });
            c.has_underline = false;
        }
        return c.layout.Get();
    }

    // IMEコンポジション中は確定済みテキストのキャレット位置に変換中テキストを挿入して表示する。
    c.text.assign(sb.query);
    if (has_comp) {
        c.text.insert(static_cast<size_t>(comp_start), sb.ime_composition);
    }
    c.layout.Reset();
    backend_.GetDWriteFactory()->CreateTextLayout(
        c.text.data(),
        static_cast<UINT32>(c.text.size()),
        fmt_.search_input.Get(),
        width,
        height,
        &c.layout);
    if (!c.layout) {
        c.Reset();
        return nullptr;
    }
    c.query.assign(sb.query);
    c.ime_comp.assign(sb.ime_composition);
    c.caret_pos = key_caret_pos;
    c.width = width;
    c.has_underline = false;
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
    bool cache_hit = false;
    IDWriteTextLayout* const text_layout = AcquireSearchInputLayout(
        sb, comp_start, sbl.text_width(), sbl.input_rect.bottom - sbl.input_rect.top, cache_hit);
    if (!text_layout) {
        return text_left;
    }

    if (has_comp) {
        text_layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{ static_cast<UINT32>(comp_start), static_cast<UINT32>(comp_len) });
        search_cache_.has_underline = true;
    }

    // 選択範囲はテキストの背面に描画
    const int text_len = static_cast<int>(search_cache_.text.size());
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
    // layout が同じで effective_pos が一致するフレーム（典型的にはキャレット点滅で
    // 直前と同じ入力）は HitTestTextPosition を省く。キャッシュミス時は layout を
    // 作り直しているため、caret_x キャッシュも cache_hit 時のみ信用する。
    if (cache_hit && search_cache_.effective_pos == effective_pos) {
        return search_cache_.caret_x;
    }
    FLOAT px, py;
    DWRITE_HIT_TEST_METRICS htm{};
    text_layout->HitTestTextPosition(static_cast<UINT32>(effective_pos), false, &px, &py, &htm);
    const float caret_x = text_left + px;
    search_cache_.effective_pos = effective_pos;
    search_cache_.caret_x = caret_x;
    return caret_x;
}

void Renderer::DrawSearchBarButtons(const SearchBarRenderState& sb, const SearchBarLayout& sbl)
{
    const auto draw_icon_btn = [&](const D2D1_RECT_F& r, const wchar_t* icon, bool hovered, float alpha = 1.0f) {
        if (hovered) {
            rt()->FillRoundedRectangle(D2D1::RoundedRect(r, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER), Brush(BrushId::TitleBarButtonHover));
        }
        DrawIcon(icon, fmt_.search_icon.Get(), r, BrushId::SearchInputText, alpha);
    };

    const auto draw_toggle_btn = [&](const D2D1_RECT_F& r, std::wstring_view label, IDWriteTextFormat* fmt, bool checked, bool hovered) {
        if (hovered || checked) {
            rt()->FillRoundedRectangle(
                D2D1::RoundedRect(r, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER),
                Brush(checked ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover));
        }
        DrawIcon(label, fmt, r, BrushId::SearchInputText, checked ? 1.0f : 0.5f);
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
        DrawCenteredText(search_count_layout_, { count_text, written }, fmt_.search_count.Get(), sbl.count_rect, BrushId::SearchInputText, 0.7f);
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
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    // DrawSearchBar が直前に作成した search_cache_.layout を再利用できるケース
    // （IME コンポジションが無く、query と表示テキストが一致）を高速パスに。
    // IME 合成中は表示テキスト = query + comp string となり layout のヒット判定対象が
    // ずれるため、search_cache_.caret_pos != -1 (= 合成中) の場合は安全側でキャッシュを捨てて
    // 再生成する。クリック応答性は損なわない (IME 合成中はキャレット移動を主に DefSubclassProc が処理)。
    const bool cache_hit = search_cache_.layout && search_cache_.width == max_width && search_cache_.caret_pos == -1 && search_cache_.query == query;
    if (cache_hit) {
        layout = search_cache_.layout;
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
    return static_cast<int>(htm.textPosition) + (is_trailing ? 1 : 0);
}
