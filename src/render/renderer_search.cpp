#include "renderer.h"
#include "ui_constants.h"
#include "d2d_util.h"
#include <algorithm>
#include <format>
#include <ranges>
#include <wrl/client.h>

void Renderer::DrawSearchBar(const SearchBarRenderState& sb, const PaneRect& md_pane_rect)
{
    if (!rt() || !sb.visible) {
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

    if (fmt_.search_icon) {
        auto* brush = Brush(BrushId::SearchInputText);
        if (brush) {
            mendo::OpacityScope guard{ brush, 0.6f };
            rt()->DrawText(L"\uE721", 1, fmt_.search_icon.Get(), sbl.icon_rect, brush);
        }
    }

    const D2D1_ROUNDED_RECT input_rrect = D2D1::RoundedRect(sbl.input_rect, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER);
    const bool no_match = !sb.query.empty() && sb.total_matches == 0;
    rt()->FillRoundedRectangle(input_rrect, Brush(no_match ? BrushId::SearchNoMatchBg : BrushId::SearchInputBg));

    // フォーカス時はリンク色で強調
    if (sb.has_focus) {
        auto* focus_brush = Brush(BrushId::Link);
        if (focus_brush) {
            rt()->DrawRoundedRectangle(input_rrect, focus_brush, 1.5f);
        }
    }
    else {
        auto* border_brush = Brush(BrushId::SearchBarBorder);
        if (border_brush) {
            mendo::OpacityScope guard{ border_brush, 0.5f };
            rt()->DrawRoundedRectangle(input_rrect, border_brush, 1.0f);
        }
    }

    // レイアウトを1回だけ作成し、描画とキャレット計測で共用する。
    // IMEコンポジション中は確定済みテキスト+変換中テキストを合成して表示。
    const float text_left = sbl.input_rect.left + SEARCH_INPUT_TEXT_PAD_LEFT;
    float caret_x = text_left;

    const bool has_comp = !sb.ime_composition.empty();
    const int comp_len = static_cast<int>(sb.ime_composition.size());
    int comp_start = 0;
    if (has_comp) {
        const int qlen = static_cast<int>(sb.query.size());
        comp_start = sb.caret_pos;
        if (comp_start < 0 || comp_start > qlen) {
            comp_start = qlen;
        }
    }
    // キャッシュキーは (query, caret_pos, ime_composition, width, height)。
    // 表示テキスト (display_buf) の合成はキャッシュミス時まで遅延する。
    const int key_caret_pos = has_comp ? comp_start : -1;

    std::wstring_view display_text = sb.query;
    std::pmr::wstring display_buf{ GetThreadLocalPoolResource() };

    if (fmt_.search_input && (!sb.query.empty() || has_comp) && backend_.GetDWriteFactory()) {
        const float input_w = sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT - text_left;
        const float input_h = sbl.input_rect.bottom - sbl.input_rect.top;

        // 比較は scalar → 空になりやすい ime_comp → query の順で短絡させる
        Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
        const bool cache_hit = cached_search_layout_ && cached_search_width_ == input_w && cached_search_caret_pos_ == key_caret_pos && cached_search_ime_comp_ == sb.ime_composition && cached_search_query_ == sb.query;
        if (cache_hit) {
            text_layout = cached_search_layout_;
            display_text = cached_search_text_;
            // 前フレームの下線範囲と異なる可能性があるため、キャッシュ上に下線が残っていれば
            // 常に全体をクリアする。IME 非アクティブ継続時はクリアも発行されない。
            if (cached_search_has_underline_) {
                text_layout->SetUnderline(FALSE, DWRITE_TEXT_RANGE{ 0, static_cast<UINT32>(cached_search_text_.size()) });
                cached_search_has_underline_ = false;
            }
        }
        else {
            // キャッシュミス時のみ display_buf を合成する。
            if (has_comp) {
                display_buf.reserve(sb.query.size() + sb.ime_composition.size());
                display_buf.append(sb.query.data(), static_cast<size_t>(comp_start));
                display_buf.append(sb.ime_composition.data(), sb.ime_composition.size());
                display_buf.append(sb.query.data() + comp_start, sb.query.size() - static_cast<size_t>(comp_start));
                display_text = display_buf;
            }
            backend_.GetDWriteFactory()->CreateTextLayout(
                display_text.data(),
                static_cast<UINT32>(display_text.size()),
                fmt_.search_input.Get(),
                input_w,
                input_h,
                &text_layout);
            if (text_layout) {
                cached_search_layout_ = text_layout;
                cached_search_text_.assign(display_text);
                cached_search_query_.assign(sb.query);
                cached_search_ime_comp_.assign(sb.ime_composition);
                cached_search_caret_pos_ = key_caret_pos;
                cached_search_width_ = input_w;
                cached_search_has_underline_ = false;
            }
        }
        if (text_layout) {
            if (has_comp) {
                const DWRITE_TEXT_RANGE range = {
                    static_cast<UINT32>(comp_start),
                    static_cast<UINT32>(comp_len)
                };
                text_layout->SetUnderline(TRUE, range);
                cached_search_has_underline_ = true;
            }

            // テキストの背面に描画
            const int text_len = static_cast<int>(display_text.size());
            const bool has_selection = !has_comp && sb.selection_start >= 0 && sb.caret_pos >= 0 && sb.selection_start != sb.caret_pos;
            if (has_selection) {
                int sel_min = std::min(sb.selection_start, sb.caret_pos);
                int sel_max = std::max(sb.selection_start, sb.caret_pos);
                sel_min = std::clamp(sel_min, 0, text_len);
                sel_max = std::clamp(sel_max, 0, text_len);
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
                text_layout.Get(),
                Brush(BrushId::SearchInputText));

            // コンポジション中はその末尾、それ以外は通常のキャレット位置
            int effective_pos;
            if (has_comp) {
                effective_pos = comp_start + comp_len;
            }
            else {
                effective_pos = sb.caret_pos;
                if (effective_pos < 0 || effective_pos > text_len) {
                    effective_pos = text_len;
                }
            }
            // layout が同じで effective_pos が一致するフレーム（典型的にはキャレット点滅
            // 直前と同じ入力）は HitTestTextPosition を省く。layout 失効時は cache_hit が
            // false になり、その経路で text_layout を作り直す前に effective_pos キャッシュも
            // 落としておく必要があるため、ここでは layout 一致を直前 update 済み
            // cached_search_layout_ で判定する。
            if (cache_hit && cached_search_effective_pos_ == effective_pos) {
                caret_x = cached_search_caret_x_;
            }
            else {
                FLOAT px, py;
                DWRITE_HIT_TEST_METRICS htm{};
                text_layout->HitTestTextPosition(static_cast<UINT32>(effective_pos), false, &px, &py, &htm);
                caret_x = text_left + px;
                cached_search_effective_pos_ = effective_pos;
                cached_search_caret_x_ = caret_x;
            }
        }
    }

    // コンポジション中はIME側がキャレットを表示するため非表示
    if (sb.caret_visible && !has_comp) {
        caret_x = std::min(caret_x + 1.0f, sbl.input_rect.right - SEARCH_INPUT_TEXT_PAD_RIGHT);
        rt()->DrawLine(
            D2D1::Point2F(caret_x, sbl.input_rect.top + 3.0f),
            D2D1::Point2F(caret_x, sbl.input_rect.bottom - 3.0f),
            Brush(BrushId::SearchInputText),
            1.0f);
    }

    auto drawIconBtn = [&](const D2D1_RECT_F& r, const wchar_t* icon, bool hovered, float alpha = 1.0f) {
        if (hovered) {
            rt()->FillRoundedRectangle(D2D1::RoundedRect(r, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER), Brush(BrushId::TitleBarButtonHover));
        }
        if (fmt_.search_icon) {
            auto* brush = Brush(BrushId::SearchInputText);
            if (brush) {
                mendo::OpacityScope guard{ brush, alpha };
                rt()->DrawText(icon, 1, fmt_.search_icon.Get(), r, brush);
            }
        }
    };

    auto drawToggleBtn = [&](const D2D1_RECT_F& r, const wchar_t* label, UINT32 len, IDWriteTextFormat* fmt, bool checked, bool hovered) {
        if (hovered || checked) {
            rt()->FillRoundedRectangle(
                D2D1::RoundedRect(r, SEARCH_BAR_CORNER, SEARCH_BAR_CORNER),
                Brush(checked ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover));
        }
        if (fmt) {
            auto* brush = Brush(BrushId::SearchInputText);
            if (brush) {
                mendo::OpacityScope guard{ brush, checked ? 1.0f : 0.5f };
                rt()->DrawText(label, len, fmt, r, brush);
            }
        }
    };

    const float nav_alpha = sb.total_matches > 0 ? 1.0f : 0.3f;
    drawIconBtn(sbl.up_btn, L"\uE70E", sb.up_btn_hovered, nav_alpha);
    drawIconBtn(sbl.down_btn, L"\uE70D", sb.down_btn_hovered, nav_alpha);

    if (fmt_.search_count && !sb.query.empty()) {
        // 「N / M」形式で表示。M は実用上 4-5 桁に収まる (md 内 hit 数) ので
        // 32 wchar_t は十分な余裕。format_to_n で先端から書き、長さを返してもらう。
        constexpr size_t kCountBufLen = 32;
        wchar_t count_text[kCountBufLen];
        std::format_to_n_result<wchar_t*> r{};
        if (sb.total_matches == 0) {
            r = std::format_to_n(count_text, kCountBufLen - 1, L"0");
        }
        else {
            r = std::format_to_n(count_text, kCountBufLen - 1, L"{} / {}", sb.current_match + 1, sb.total_matches);
        }
        const auto written = static_cast<UINT32>(r.out - count_text);
        auto* brush = Brush(BrushId::SearchInputText);
        if (brush) {
            mendo::OpacityScope guard{ brush, 0.7f };
            rt()->DrawText(count_text, written, fmt_.search_count.Get(), sbl.count_rect, brush);
        }
    }

    drawToggleBtn(sbl.case_btn, L"Aa", 2, fmt_.search_count.Get(), sb.case_sensitive, sb.case_btn_hovered);
    drawToggleBtn(sbl.highlight_btn, L"\uE7E6", 1, fmt_.search_icon.Get(), sb.highlight_enabled, sb.highlight_btn_hovered);
    drawIconBtn(sbl.close_btn, L"\uE8BB", sb.close_btn_hovered);
}

int Renderer::HitTestSearchInput(std::wstring_view query, float local_x, float max_width) const
{
    if (query.empty() || !fmt_.search_input || !backend_.GetDWriteFactory()) {
        return 0;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    // DrawSearchBar が直前に作成した cached_search_layout_ を再利用できるケース
    // （IME コンポジションが無く、query と表示テキストが一致）を高速パスに。
    // IME 合成中は表示テキスト = query + comp string となり layout のヒット判定対象が
    // ずれるため、cached_search_caret_pos_ != -1 (= 合成中) の場合は安全側でキャッシュを捨てて
    // 再生成する。クリック応答性は損なわない (IME 合成中はキャレット移動を主に DefSubclassProc が処理)。
    const bool cache_hit = cached_search_layout_ && cached_search_width_ == max_width && cached_search_caret_pos_ == -1 && cached_search_query_ == query;
    if (cache_hit) {
        layout = cached_search_layout_;
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
    // HitTestPoint が失敗 (E_FAIL 等) で out 引数未書込みでも未初期化値の読み出しを避ける。
    // hit_test_service.cpp の他 2 箇所と同じ慣習。
    BOOL is_trailing = FALSE;
    BOOL is_inside = FALSE;
    DWRITE_HIT_TEST_METRICS htm{};
    layout->HitTestPoint(local_x, 0.0f, &is_trailing, &is_inside, &htm);
    return static_cast<int>(htm.textPosition) + (is_trailing ? 1 : 0);
}
