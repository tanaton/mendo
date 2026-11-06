#include "dwrite_measurer.h"
#include "brush_id.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "document_utils.h"
#include "inline_math.h"
#include "layout_computer.h"
#include "task_scheduler.h"
#include "profiler.h"
#include "syntax.h"
#include "ui_constants.h"
#include "utf8_codec.h"
#include "utility.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <iterator>
#include <ranges>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace {

// 背景を行の高さから上下に縮める量。行送り 24 DIP で背景の高さと隙間を GitHub と同じ 21 / 3 DIP にする。
constexpr float INLINE_CODE_INSET_Y = 1.5f;
// コード範囲の両端に足す字間で、背景の左右パディングになる。背景はこの字間までに留め、
// GitHub と同じく前後の空白を背景の外にまるごと残す。
constexpr float INLINE_CODE_SPACING = 5.0f;

// セルに列幅を適用して高さを返す。揃えはセルを作るときに付けてある。
// 左揃えで列幅に収まるセルは作ったときの無限幅のまま使う。折り返さないので行組みは同じで、
// 幅を付けると組み直しになる。幅を変えなければ GetMetrics は計測済みの結果を返すだけ。
float ApplyCellWidth(const TableLayoutData& tl, size_t ci, float cw, TableAlign align) noexcept
{
    auto* layout = tl.cell_layouts[ci].Get();
    const bool left = align == TableAlign::Default || align == TableAlign::Left;
    const float layout_width = left && tl.cell_natural_widths[ci] <= cw ? layout::kNoWrapLayoutWidth : cw;
    if (std::abs(layout->GetMaxWidth() - layout_width) >= CELL_WIDTH_EPSILON) {
        layout->SetMaxWidth(layout_width);
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.height;
}

// インラインコードの run の矩形 (字間のパディング込み、レイアウト原点基準) を add に渡す。折り返した run は行ごとに 1 つ。
// 折り返しに依存するので、幅を変えて計測し直すたびに作り直す。
template <class Add>
void ForEachInlineCodeBg(IDWriteTextLayout* layout, std::string_view text, std::span<const TextRun> runs, Add&& add)
{
    thread_local std::vector<DWRITE_HIT_TEST_METRICS> buffer;
    Utf16OffsetCursor cursor{ text };
    for (const auto& run : runs) {
        if (!run.code() || run.length == 0) {
            continue;
        }
        const auto range = cursor.WideRange(run.start, run.length);
        const UINT32 count = FetchHitTestMetrics(layout, range.startPosition, range.length, buffer);
        for (const auto& m : std::span(buffer).first(count)) {
            add(D2D1::RectF(m.left, m.top + INLINE_CODE_INSET_Y, m.left + m.width, m.top + m.height - INLINE_CODE_INSET_Y));
        }
    }
}

// layout を持たないセル (evict 中の行) の高さ。列幅に収まれば初回計測で残した折り返さない高さそのもので、
// 収まらなければ折り返しの行数を見積もる (行を復元したときの実測で直す)。
float EvictedCellHeight(const TableLayoutData& tl, size_t ci, float cw) noexcept
{
    const float natural = tl.cell_natural_widths[ci];
    const float height = tl.cell_nowrap_heights[ci];
    if (natural <= cw || cw <= 0.0f) {
        return height;
    }
    return height * std::ceil(natural / cw);
}

// セルに列幅を適用し、行高さ (上下パディング込み、base_row_height 以上) を求める。
// インラインコードを含むテーブルなら、行の背景も作り直す (layout の無い evict 中の行は背景も持たない)。
float MeasureRowHeight(TableLayoutData& tl, const NodeTableData& tbl, size_t r, float base_row_height)
{
    float height = base_row_height;
    auto* const bgs = tl.row_code_bgs.empty() ? nullptr : &tl.row_code_bgs[r];
    if (bgs) {
        bgs->clear();
    }
    for (size_t c = 0; c < tl.col_count; c++) {
        const size_t ci = tl.CellIndex(r, c);
        auto* const layout = tl.cell_layouts[ci].Get();
        const float cell_height = layout ? ApplyCellWidth(tl, ci, tl.col_widths[c], tbl.ColAlign(c)) : EvictedCellHeight(tl, ci, tl.col_widths[c]);
        height = std::max(height, cell_height + TABLE_CELL_PADDING * 2.0f);
        if (layout && bgs) {
            ForEachInlineCodeBg(layout, tbl.GetCellText(r, c), tbl.GetCellRuns(r, c), [&](const InlineCodeBg& rect) {
                bgs->push_back({ static_cast<uint32_t>(c), rect });
            });
        }
    }
    return height;
}

// 初回計測でセル layout を残す表の大きさの上限。layout は 1 セル 7KB ほどあり、5 万行 6 列の表で 2GB になる。
// これより大きい表は自然幅と高さだけ測って捨て、可視行だけ作り直す。小さい表は可視行を 2 回作らないよう残す。
constexpr size_t kMaxKeptTableCells = 1024;

// 列ごとに、同じ中身 (テキストと装飾) のセルのうち最初のものを代表にし、[セル] = 代表のセルを返す。
// 大きな表は状態や種別のような同じ値が列に並びやすく、代表だけ測れば CreateTextLayout が減る。
// 見出し行 (書式が違う) と空のセル (測らない) は自分自身を代表にする。
// セル数は concat_text 内の uint32_t オフセットで数えられる範囲に収まるので、uint32_t で持って巨大表の一時メモリを半分にする。
std::vector<uint32_t> FindRepresentativeCells(const NodeTableData& tbl, TaskScheduler* scheduler)
{
    std::vector<uint32_t> reps(static_cast<size_t>(tbl.row_count) * tbl.col_count);
    ParallelFor(scheduler, tbl.col_count, 1, [&](size_t col_begin, size_t col_end) {
        for (size_t c = col_begin; c < col_end; ++c) {
            // 列の違う値の数までしか伸びないので、値の少ない列は小さいまま済む (20 万行 20 列で 47 → 39ms)。
            FlatHashMap<uint32_t> first_rows;
            for (size_t r = 0; r < tbl.row_count; ++r) {
                const size_t ci = tbl.CellIndex(r, c);
                reps[ci] = static_cast<uint32_t>(ci);
                const auto text = tbl.GetCellText(r, c);
                if (text.empty() || tbl.IsHeaderRow(r)) {
                    continue;
                }
                const auto [first_row, added] = first_rows.TryEmplace(Fnv1a64(text));
                if (added) {
                    *first_row = static_cast<uint32_t>(r);
                    continue;
                }
                // ハッシュが衝突したセルと、同じテキストで装飾が違うセルは、最初のものを代表にせず自分で測る。
                if (tbl.GetCellText(*first_row, c) == text && std::ranges::equal(tbl.GetCellRuns(r, c), tbl.GetCellRuns(*first_row, c))) {
                    reps[ci] = static_cast<uint32_t>(tbl.CellIndex(*first_row, c));
                }
            }
        }
    });
    return reps;
}

// テーブルを行チャンクで並列処理するときの 1 チャンクの行数。数十セルの小さな表も、1 画面分の行の復元も
// ワーカーに行き渡る細かさにする。32 セルから 8 セルで、test.md を開いて先読みするまでが 2.6 → 1.9ms、
// 2 万行 6 列の表の復元が 2.9 → 2.0ms に縮む。
constexpr size_t TableRowsPerChunk(size_t col_count) noexcept
{
    constexpr size_t kCellsPerChunk = 8;
    return std::max<size_t>(kCellsPerChunk / std::max<size_t>(col_count, 1), 1);
}

// value が大きければ上げる (C++26 の fetch_max の代わり)。
void RaiseTo(std::atomic<float>& a, float value) noexcept
{
    float cur = a.load(std::memory_order_relaxed);
    while (value > cur && !a.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

// セル layout の無い行は高さを見積もるだけで、chunk の取得のほうが重いので粗く分ける。行チャンクのままだと
// 2 万行 6 列の幅の変更が 1.0ms かかる (分けて 0.48ms)。直列にすると表が大きいときに遅れる。
constexpr size_t kEstimatedRowsPerChunk = 256;

// 記号を 1 行目の中央に置くため (GenListBullet)。layout 自体は変えない。
void CacheFirstLineHeight(IDWriteTextLayout* layout, NodeLayoutEntry& entry) noexcept
{
    DWRITE_LINE_METRICS lm{};
    UINT32 lc = 0;
    // 複数行レイアウトでは E_NOT_SUFFICIENT_BUFFER が返るが、先頭 1 行分は lm に書き込まれる。
    const HRESULT hr = layout->GetLineMetrics(&lm, 1, &lc);
    entry.set_first_line_height(((SUCCEEDED(hr) || hr == E_NOT_SUFFICIENT_BUFFER) && lc > 0) ? lm.height : 0.0f);
}

// entry.text_layout の計測結果から高さを返す。インラインコード背景を作り直す
// (描画色と下線は SetMaxWidth だけの再計測でも残る)。
float FinishTextMeasure(const Node& node, NodeLayoutEntry& entry, const DWRITE_TEXT_METRICS& metrics, const Theme& theme)
{
    if (node.type == NodeType::ListItem) {
        CacheFirstLineHeight(entry.text_layout(), entry);
    }
    if (node.type == NodeType::CodeBlock) {
        entry.set_natural_code_width(metrics.widthIncludingTrailingWhitespace + 2.0f * layout::NodeTextXOffset(node, theme));
    }
    entry.clear_inline_code_bgs();
    ForEachInlineCodeBg(entry.text_layout(), node.GetText(), node.runs(), [&entry](const InlineCodeBg& rect) {
        entry.ensure_inline_code_bgs().push_back(rect);
    });
    return metrics.height;
}

// テキストレイアウトを持たないノード (HR / 空テキスト) の高さ。それ以外は nullopt。
std::optional<float> MeasureWithoutTextLayout(const Node& node, const Theme& theme) noexcept
{
    if (node.type == NodeType::HorizontalRule) {
        return layout::HorizontalRuleHeight(theme);
    }
    if (!node.HasText()) {
        return layout::EmptyTextNodeHeight(node, theme);
    }
    return std::nullopt;
}

// 既存の text_layout を SetMaxWidth で再計測する。中身が変わる経路 (ズームの InvalidateAllLayouts、数式の
// InvalidatePendingMathLayouts、読み直しの新しいキャッシュ) は text_layout を捨てるので、
// 現存していればテキスト/runs/フォント幾何は一致している。SetMaxWidth の後の GetMetrics は解析からやり直すので
// 作り直しより 1〜16% 速いだけだが、装飾の付け直しは要らない。
float RemeasureExistingLayout(const Node& node, NodeLayoutEntry& entry, float layout_width, const Theme& theme)
{
    MENDO_PROFILE("MeasureNode.fastpath");
    entry.text_layout()->SetMaxWidth(layout_width);
    DWRITE_TEXT_METRICS metrics{};
    entry.text_layout()->GetMetrics(&metrics);
    return FinishTextMeasure(node, entry, metrics, theme);
}

// 1 属性ぶんのレンジビルダ。隣接ランで属性が連続する間はマージし、切れたら emit する。
// 範囲は UTF-16 textPosition 単位。
template <typename Emit>
class AttrRangeBuilder {
public:
    explicit AttrRangeBuilder(Emit emit) noexcept : emit_(emit)
    {
    }

    void Update(bool active_now, DWRITE_TEXT_RANGE r) noexcept
    {
        if (!active_now) {
            Flush();
            return;
        }
        if (active_ && r.startPosition == range_.startPosition + range_.length) {
            range_.length += r.length;
            return;
        }
        Flush();
        range_ = r;
        active_ = true;
    }

    void Flush() noexcept
    {
        if (active_) {
            emit_(range_);
            active_ = false;
        }
    }

private:
    Emit emit_;
    DWRITE_TEXT_RANGE range_{};
    bool active_ = false;
};

// 描画時に UTF-16 位置へ数え直さないよう、ワーカー上で UTF-16 単位にし、描画時のグリフラン分割を減らす。
// 空白は色が見えないので、トークン間の空白だけの隙間は直前のトークンに含め、同種のトークンはまとめる。
std::vector<SyntaxToken> ToWideMergedTokens(std::string_view text, std::vector<SyntaxToken> tokens)
{
    Utf16OffsetCursor cursor{ text };
    size_t out = 0;
    uint32_t prev_end = 0; // 直前トークンの UTF-8 終端
    for (size_t i = 0; i < tokens.size(); i++) {
        const SyntaxToken t = tokens[i];
        const auto w = cursor.WideRange(t.start, t.length);
        const auto gap = text.substr(prev_end, t.start - prev_end);
        prev_end = t.start + t.length;
        if (out > 0 && gap.find_first_not_of(" \t\r\n") == std::string_view::npos) {
            SyntaxToken& prev = tokens[out - 1];
            prev.length = w.startPosition - prev.start;
            if (prev.type == t.type) {
                prev.length += w.length;
                continue;
            }
        }
        tokens[out++] = SyntaxToken{ w.startPosition, w.length, t.type };
    }
    tokens.resize(out);
    return tokens;
}

} // namespace

bool DWriteTextMeasurer::CreateFormats(const Theme& theme, DWriteContext& ctx)
{
    // 描画側と同じ作り方にし、無効なフォント名でも描画と同じフォールバックで測る。
    IDWriteFactory* const factory = ctx.factory.Get();
    const wchar_t* family = theme.font_family.c_str();
    ctx.body = CreateTextFormat(factory, { .family = family, .size = theme.font_size_body, .locale = theme.text_locale, .line_height_factor = BODY_LINE_HEIGHT_FACTOR });
    for (const auto i : std::views::iota(0, 6)) {
        ctx.h[i] = CreateTextFormat(factory, { .family = family, .weight = DWRITE_FONT_WEIGHT_BOLD, .size = theme.font_size_h[i], .locale = theme.text_locale });
    }
    ctx.code = CreateTextFormat(factory, { .family = theme.monospace_font.c_str(), .size = theme.font_size_code, .locale = theme.text_locale, .no_wrap = true });
    return ctx.body && ctx.code && std::ranges::all_of(ctx.h, [](const auto& f) static noexcept { return f != nullptr; });
}

bool DWriteTextMeasurer::RecreateFormats()
{
    if (!ctx_.factory || !theme_) {
        return false;
    }
    static std::atomic<uint64_t> s_generation{ 0 };
    ctx_.generation = ++s_generation;
    owner_thread_ = std::this_thread::get_id();
    body_fonts_.Reset(ctx_.factory.Get(), theme_->font_family, theme_->text_locale);
    code_fonts_.Reset(ctx_.factory.Get(), theme_->monospace_font, theme_->text_locale);
    return CreateFormats(*theme_, ctx_);
}

const DWriteTextMeasurer::DWriteContext& DWriteTextMeasurer::Context() const
{
    if (std::this_thread::get_id() == owner_thread_) {
        return ctx_;
    }
    thread_local DWriteContext t_ctx;
    if (t_ctx.generation != ctx_.generation) {
        if (!t_ctx.factory) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(t_ctx.factory.GetAddressOf()));
        }
        // 作れなければ共有ファクトリで測る (競合で遅いだけで結果は同じ)。
        if (!t_ctx.factory || !CreateFormats(*theme_, t_ctx)) {
            return ctx_;
        }
        t_ctx.generation = ctx_.generation;
    }
    return t_ctx;
}

DWriteTextMeasurer::TextStyle DWriteTextMeasurer::NodeStyle(const DWriteContext& ctx, const Node& node) const noexcept
{
    if (node.type == NodeType::CodeBlock) {
        return { ctx.code.Get(), code_fonts_ };
    }
    if (node.type == NodeType::Heading) {
        const int8_t lv = node.heading_level;
        if (lv >= 1 && lv <= 6) {
            return { ctx.h[lv - 1].Get(), body_fonts_ };
        }
    }
    return { ctx.body.Get(), body_fonts_ };
}

DWriteTextMeasurer::TextStyle DWriteTextMeasurer::TableRowStyle(const DWriteContext& ctx, const NodeTableData& tbl, size_t r) const noexcept
{
    return { tbl.IsHeaderRow(r) ? ctx.h[Theme::kTableHeaderLevel - 1].Get() : ctx.body.Get(), body_fonts_ };
}

bool DWriteTextMeasurer::ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text, bool apply_code_size, float math_font_size, float fit_width) const
{
    if (runs.empty()) {
        return false;
    }

    AttrRangeBuilder bold{ [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_EXTRA_BOLD, r);
    } };
    AttrRangeBuilder italic{ [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
    } };
    // 背景の左右パディングとして、コード範囲の両端に字間を足す。
    // 末尾の字間は最後の文字の位置に置くので、サロゲートペアなら 2 単位ぶん指定する。
    ComPtr<IDWriteTextLayout1> layout1;
    layout->QueryInterface(IID_PPV_ARGS(&layout1));
    UINT32 code_tail_units = 1;
    AttrRangeBuilder code{ [this, layout, apply_code_size, layout1 = layout1.Get(), &code_tail_units](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontFamilyName(theme_->monospace_font.c_str(), r);
        if (apply_code_size) {
            layout->SetFontSize(theme_->font_size_code, r);
        }
        if (!layout1 || r.length == 0) {
            return;
        }
        // 1 文字だけの範囲は 2 回目の指定が 1 回目を上書きするので、先頭の字間も 2 回目に含める。
        const UINT32 tail = code_tail_units;
        layout1->SetCharacterSpacing(INLINE_CODE_SPACING, 0.0f, 0.0f, { r.startPosition, 1 });
        layout1->SetCharacterSpacing(tail == r.length ? INLINE_CODE_SPACING : 0.0f, INLINE_CODE_SPACING, 0.0f, { r.startPosition + r.length - tail, tail });
    } };
    AttrRangeBuilder strike{ [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetStrikethrough(TRUE, r);
    } };
    AttrRangeBuilder link{ [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetUnderline(TRUE, r);
        layout->SetDrawingEffect(BrushEffect::For(BrushId::Link), r);
    } };

    // 数式ごとにインラインオブジェクトを作る。数式 1 個あたり約 135ns 以下で、数式 12000 個の文書を組む時間
    // (直列 113ms) の約 1.4% にとどまる (BM_InlineMathLayout)。オブジェクトは表に持たせて使い回さない。同じオブジェクトを
    // 隣り合う範囲 (*$x$*$x$ など) に掛けると、DirectWrite が 1 つの範囲にまとめて数式 1 個分の幅にする。
    // 同じ理由で、AttrRangeBuilder でまとめずに run ごとに掛ける。
    bool math_pending = false;
    // runs は start 昇順・非重複なので、境界の UTF-16 位置は前方への 1 回の走査で求まる。
    Utf16OffsetCursor cursor{ text };
    for (const auto& r : runs) {
        const DWRITE_TEXT_RANGE wr = cursor.WideRange(r.start, r.length);
        bold.Update(r.bold(), wr);
        italic.Update(r.italic(), wr);
        code.Update(r.code(), wr);
        if (r.code() && r.length > 0) {
            code_tail_units = utf8_codec::Utf16Units(utf8_codec::DecodePrev(text, r.start + r.length).cp);
        }
        strike.Update(r.strikethrough(), wr);
        link.Update(r.has_link(), wr);
        if (!inline_math_ || !r.math()) {
            continue;
        }
        const auto tex = InlineMathTex(text, r);
        if (!tex) {
            continue;
        }
        const uint64_t key = InlineMathKey(*tex, math_font_size);
        if (const auto metrics = inline_math_->FindOrRequest(key)) {
            if (metrics->ok()) {
                const float scale = fit_width > 0.0f && metrics->width > fit_width ? fit_width / metrics->width : 1.0f;
                layout->SetInlineObject(MakeInlineMathObject(key, *metrics, scale).Get(), wr);
            }
        }
        else {
            math_pending = true;
        }
    }

    bold.Flush();
    italic.Flush();
    code.Flush();
    strike.Flush();
    link.Flush();
    return math_pending;
}

std::optional<float> DWriteTextMeasurer::MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const
{
    MENDO_PROFILE("MeasureNode");
    if (node.type == NodeType::Table) {
        return MeasureTable(node, entry, max_width);
    }
    if (const auto height = MeasureWithoutTextLayout(node, *theme_)) {
        return height;
    }

    const float layout_width = layout::NodeLayoutWidth(node, max_width);
    // ブロック数式は図と同じく、幅に収まらなければ縮めて中央に置く (GitHub と同じ)。
    const bool block_math = IsBlockMath(node);
    if (entry.text_layout()) {
        // GetMetrics は前回の結果を返すだけ。
        DWRITE_TEXT_METRICS metrics{};
        entry.text_layout()->GetMetrics(&metrics);
        // 同じ幅で組んだ text_layout が残っていれば、高さも行もインラインコードの背景も変わらない。
        if (entry.cached_width == layout_width) {
            return metrics.height;
        }
        // 改行でしか折れておらず最も長い行が新しい幅に収まるなら、組み直しても行は同じ。左揃えなら幅は付け替えない
        // (SetMaxWidth の後の GetMetrics は作り直しとほぼ同じだけかかる)。
        // 幅を変えたときの全ノードの計測し直しが test.md で 174ms → 143ms、README で 6.0ms → 3.1ms。
        if (metrics.lineCount == static_cast<UINT32>(node.line_count) + 1 && metrics.width <= layout_width &&
            entry.text_layout()->GetTextAlignment() == DWRITE_TEXT_ALIGNMENT_LEADING) {
            return metrics.height;
        }
        // 縮める倍率は幅で変わるので、縮めて組んだか新しい幅に収まらないブロック数式は組み直す。
        const bool rescale = block_math && !(metrics.width < entry.text_layout()->GetMaxWidth() && metrics.width <= layout_width);
        if (!rescale) {
            return RemeasureExistingLayout(node, entry, layout_width, *theme_);
        }
    }

    const auto& text = node.GetText();
    const auto& ctx = Context();
    ComPtr<IDWriteTextLayout> layout;
    const TextStyle style = NodeStyle(ctx, node);
    if (FAILED(CreateDocTextLayout(ctx.factory.Get(), text, style.format, layout_width, layout::kNoWrapLayoutWidth, style.fonts, &layout))) {
        return std::nullopt;
    }

    entry.pending_math = ApplyRunFormatting(layout.Get(), node.runs(), text, /*apply_code_size=*/node.type != NodeType::Heading,
                                            MathFontSize(*theme_, node), block_math ? layout_width : 0.0f);
    if (block_math) {
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (const uint32_t label_length = node.alert_label_length(); label_length > 0) {
        Utf16OffsetCursor cursor{ text };
        layout->SetDrawingEffect(BrushEffect::For(AlertBrushIdFromIndex(AlertColorIndex(node.alert_type))), cursor.WideRange(0, label_length));
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    // 描画パスでのトークン化によるフレーム落ちを避けるため、ここで作る。
    // layout と一緒に捨てるので、画面外のコードブロックのトークンは残らない。
    if (node.type == NodeType::CodeBlock && node.code_language != SyntaxLanguage::None) {
        entry.set_syntax_tokens(ToWideMergedTokens(text, TokenizeCodeNode(node)));
    }

    entry.set_text_layout(std::move(layout));
    return FinishTextMeasure(node, entry, metrics, *theme_);
}

ComPtr<IDWriteTextLayout> DWriteTextMeasurer::CreateCellLayout(const DWriteContext& ctx, const NodeTableData& tbl, bool& pending_math, size_t r, size_t c) const
{
    ComPtr<IDWriteTextLayout> cell_layout;
    const auto text = tbl.GetCellText(r, c);
    const TextStyle style = TableRowStyle(ctx, tbl, r);
    if (!text.empty() && SUCCEEDED(CreateDocTextLayout(ctx.factory.Get(), text, style.format, layout::kNoWrapLayoutWidth, layout::kNoWrapLayoutWidth, style.fonts, &cell_layout))) {
        if (ApplyRunFormatting(cell_layout.Get(), tbl.GetCellRuns(r, c), text, /*apply_code_size=*/true, MathFontSize(*theme_, tbl, r), /*fit_width=*/0.0f)) {
            std::atomic_ref{ pending_math }.store(true, std::memory_order_relaxed);
        }
        // 揃えは折り返さない幅に影響しないので、自然幅の計測前に付けてよい。
        if (const TableAlign align = tbl.ColAlign(c); align == TableAlign::Center) {
            cell_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        else if (align == TableAlign::Right) {
            cell_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
    return cell_layout;
}

void DWriteTextMeasurer::MeasureTableCells(const NodeTableData& tbl, TableLayoutData& tl, bool& pending_math) const
{
    MENDO_PROFILE("MeasureTableCells");
    const size_t col_count = tbl.col_count;
    auto& natural_widths = tl.natural_col_widths;
    const bool keep_layouts = tl.cell_layouts.size() <= kMaxKeptTableCells;
    // layout を残す小さい表は、全セルの layout が要るので代表を探さない (空のまま)。
    const auto reps = keep_layouts ? std::vector<uint32_t>{} : FindRepresentativeCells(tbl, scheduler_);

    // 巨大テーブルは全セルの CreateTextLayout が秒単位になるため行チャンクで並列化する。
    // セルは chunk ごとに別 index へ書くので排他不要。残さない layout は worker 上で解放する。
    // 列の最小幅 (最も長い単語) はセルごとには持たず (1 セル 4 byte)、列ごとの最大だけを chunk をまたいで重ねる。chunk は
    // 8 セルと細かいので、chunk ごとの確保とロックは 20 万行の表で計測より重い。自然幅が今の最大以下のセルは最小幅も
    // 効かないので DetermineMinWidth を呼ばない。代表と同じ中身のセルは最小幅も同じ。
    std::vector<std::atomic<float>> min_widths(col_count);
    ParallelFor(scheduler_, tbl.row_count, TableRowsPerChunk(col_count), [&](size_t row_begin, size_t row_end) {
        const auto& ctx = Context();
        for (size_t r = row_begin; r < row_end; r++) {
            for (size_t c = 0; c < col_count; c++) {
                const size_t ci = tl.CellIndex(r, c);
                if (!reps.empty() && reps[ci] != ci) {
                    continue;
                }
                auto cell_layout = CreateCellLayout(ctx, tbl, pending_math, r, c);
                if (!cell_layout) {
                    continue;
                }
                DWRITE_TEXT_METRICS metrics{};
                cell_layout->GetMetrics(&metrics);
                tl.cell_natural_widths[ci] = metrics.width;
                tl.cell_nowrap_heights[ci] = metrics.height;
                if (auto& col_min = min_widths[c]; metrics.width > col_min.load(std::memory_order_relaxed)) {
                    if (FLOAT w = 0.0f; SUCCEEDED(cell_layout->DetermineMinWidth(&w))) {
                        RaiseTo(col_min, std::min(w, metrics.width));
                    }
                }
                if (keep_layouts) {
                    tl.cell_layouts[ci] = std::move(cell_layout);
                }
            }
        }
    });
    std::ranges::transform(min_widths, tl.min_col_widths.begin(), [](const std::atomic<float>& w) noexcept { return w.load(std::memory_order_relaxed); });
    // 代表と同じ中身のセルは代表の値を写す (代表は別の chunk にあるので測り終えてから)。書き込み先は行ごとに重ならない。
    if (!reps.empty()) {
        ParallelFor(scheduler_, tbl.row_count, kEstimatedRowsPerChunk, [&](size_t row_begin, size_t row_end) {
            for (size_t ci = row_begin * col_count; ci < row_end * col_count; ++ci) {
                tl.cell_natural_widths[ci] = tl.cell_natural_widths[reps[ci]];
                tl.cell_nowrap_heights[ci] = tl.cell_nowrap_heights[reps[ci]];
            }
        });
    }
    // 列の自然幅を集める。測れなかったセルと空のセルは確保したときの 0 のままなので効かない。
    for (size_t r = 0; r < tbl.row_count; r++) {
        for (size_t c = 0; c < col_count; c++) {
            natural_widths[c] = std::max(natural_widths[c], tl.cell_natural_widths[tl.CellIndex(r, c)]);
        }
    }
    if (!keep_layouts) {
        tl.MarkAllRowsEvicted();
    }
}

float DWriteTextMeasurer::FinalizeTableLayout(const NodeTableData& tbl, TableLayoutData& tl, float max_width) const
{
    MENDO_PROFILE("FinalizeTableLayout");
    const size_t col_count = tl.col_count;
    const float borders_width = (static_cast<float>(col_count) + 1.0f) * TABLE_BORDER_WIDTH;
    const float paddings_width = static_cast<float>(col_count) * TABLE_CELL_PADDING * 2.0f;

    layout::ComputeColumnWidths(tl.col_widths, tl.natural_col_widths, tl.min_col_widths, max_width - borders_width - paddings_width);

    // 幅の適用 (SetMaxWidth + GetMetrics) は layout のあるセル分かかる。行ごとの書き込み先
    // (row_heights[r] と行内セルの幅・高さキャッシュ) は重ならないので行チャンクで並列化する。
    // layout のある行と見積もるだけの行は 1 行の重さが桁で違うので、別の細かさで分ける。
    const float base_row_height = layout::TableBaseRowHeight(*theme_);
    const auto [live_begin, live_end] = tl.LiveRows();
    const size_t live_count = live_end - live_begin;
    ParallelFor(scheduler_, live_count, TableRowsPerChunk(col_count), [&](size_t begin, size_t end) {
        for (size_t r = live_begin + begin; r < live_begin + end; r++) {
            tl.row_heights[r] = MeasureRowHeight(tl, tbl, r, base_row_height);
        }
    });
    ParallelFor(scheduler_, tbl.row_count - live_count, kEstimatedRowsPerChunk, [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; k++) {
            const size_t r = k < live_begin ? k : k + live_count;
            tl.row_heights[r] = MeasureRowHeight(tl, tbl, r, base_row_height);
        }
    });

    // evict 行が残っていても確定させる (dirty に残さない)。dirty のままだと可視中は毎フレーム全行の
    // Finalize が走る。残りは RestoreEvictedTableRows が可視行だけ埋める。
    tl.CommitGeometry(max_width);
    return tl.TotalHeight();
}

TableRestoreResult DWriteTextMeasurer::RestoreEvictedTableRows(const Node& node, NodeLayoutEntry& entry, YRange viewport) const
{
    MENDO_PROFILE("RestoreEvictedTableRows");
    TableRestoreResult result;
    const auto& tbl = *node.table_data();
    auto& tl = *entry.table_layout();
    const auto [r_begin, r_end] = tl.VisibleRowRange(viewport.top, viewport.bottom);
    std::vector<size_t> rows;
    std::ranges::copy_if(std::views::iota(r_begin, r_end), std::back_inserter(rows), [&](size_t r) { return tl.IsRowEvicted(r); });
    if (rows.empty()) {
        return result;
    }
    result.restored = true;

    // 戻りスクロールで 1 画面分の行がまとめて戻るので、MeasureTableCells と同じく行チャンクで並列化する。
    // 行ごとの書き込み先は重ならない。evict の管理情報 (MarkRowRestored) は後で直列に更新する。
    const float base_row_height = layout::TableBaseRowHeight(*theme_);
    std::vector<float> row_heights(rows.size());
    ParallelFor(scheduler_, rows.size(), TableRowsPerChunk(tl.col_count), [&](size_t begin, size_t end) {
        const auto& ctx = Context();
        for (size_t k = begin; k < end; k++) {
            const size_t r = rows[k];
            for (size_t c = 0; c < tbl.col_count; c++) {
                tl.cell_layouts[tl.CellIndex(r, c)] = CreateCellLayout(ctx, tbl, entry.pending_math, r, c);
            }
            row_heights[k] = MeasureRowHeight(tl, tbl, r, base_row_height);
        }
    });

    const size_t row_count = tbl.row_count;
    size_t first_changed_row = row_count;
    for (size_t k = 0; k < rows.size(); k++) {
        const size_t r = rows[k];
        tl.MarkRowRestored(r);
        // evict 中の行の高さは折り返しの行数を見積もったものなので、ここで実測に揃える。
        if (row_heights[k] != tl.row_heights[r]) {
            tl.row_heights[r] = row_heights[k];
            first_changed_row = std::min(first_changed_row, r);
        }
    }

    if (first_changed_row < row_count) {
        tl.RebuildRowCumY(first_changed_row);
        result.height = tl.TotalHeight();
    }
    return result;
}

std::optional<float> DWriteTextMeasurer::MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width) const
{
    MENDO_PROFILE("MeasureTable");
    const auto* tbl = node.table_data();

    // 全セルを測り終えている。前回と max_width がほぼ一致し幾何が揃っていれば、セル幅・行高さ・行の位置は
    // すべて変化しないため、幾何の高さのまま確定させる。entry.height は返さない。別の幅で推定が伸ばした高さが残っている
    // ことがある (A → B → A と幅を戻したとき)。行ごとの inline_code_bgs も幅で決まるので保持できる。
    // evict 済み行のセルは RestoreEvictedTableRows が可視になった時点で再生成する。
    // 幅が変わったら、テキストレイアウトの作成をスキップして列幅の再計算だけ行う。自然幅はセルのテキストだけで決まり、
    // evict しても残る。行の高さは FinalizeTableLayout が新しい列幅で全行測り、見えている evict 行は次の計測パス
    // (RestoreEvictedTableRows) が戻す。
    if (auto* tl = entry.table_layout()) {
        return tl->GeometryValidAt(max_width) ? tl->TotalHeight() : FinalizeTableLayout(*tbl, *tl, max_width);
    }

    // インラインコードの背景は行の計測 (MeasureRowHeight) で作る。持たないテーブルには確保しない。
    const bool has_inline_code = std::ranges::any_of(tbl->all_runs, [](const TextRun& r) static noexcept {
        return r.code() && r.length > 0;
    });
    auto tl = std::make_unique<TableLayoutData>(tbl->row_count, tbl->col_count, has_inline_code);
    // 列幅判定に全行の自然幅が要るので、初回は全セルを測る。行の復元は次の計測パスに任せる。
    MeasureTableCells(*tbl, *tl, entry.pending_math);
    const float height = FinalizeTableLayout(*tbl, *tl, max_width);
    entry.set_table_layout(std::move(tl));
    return height;
}
