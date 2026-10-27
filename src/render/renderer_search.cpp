#include "renderer.h"
#include "ui_constants.h"
#include <algorithm>
#include <wrl/client.h>

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

overlay::SearchInputText Renderer::MeasureSearchInput(const SearchBarRenderState& sb, const SearchBarLayout& sbl)
{
    const bool has_comp = !sb.ime_composition.empty();
    if (!fmt_.search_input || (sb.query.empty() && !has_comp) || !backend_.GetDWriteFactory()) {
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

    // レイアウトを1回だけ作成し、描画とキャレット計測で共用する。
    IDWriteTextLayout* const text_layout = AcquireSearchInputLayout(
        sb, comp_start, sbl.text_width(), sbl.input_rect.bottom - sbl.input_rect.top);
    if (!text_layout) {
        return {};
    }
    overlay::SearchInputText input{ .layout = text_layout };

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
                0.0f, 0.0f,
                &htm_sel, 1, &actual);
            if (actual > 0) {
                input.selection_x = htm_sel.left;
                input.selection_width = htm_sel.width;
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
    text_layout->HitTestTextPosition(static_cast<UINT32>(effective_pos), false, &px, &py, &htm);
    input.caret_x = px;
    return input;
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
