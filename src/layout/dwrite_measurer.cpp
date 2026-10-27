#include "dwrite_measurer.h"
#include "brush_id.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "document_utils.h"
#include "layout_computer.h"
#include "parallel_for.h"
#include "profiler.h"
#include "syntax.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "utf8_codec.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <latch>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <string_view>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace {

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
    thread_local std::pmr::vector<DWRITE_HIT_TEST_METRICS> buffer;
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
        if (!layout) {
            height = std::max(height, EvictedCellHeight(tl, ci, tl.col_widths[c]) + TABLE_CELL_PADDING * 2.0f);
            continue;
        }
        height = std::max(height, ApplyCellWidth(tl, ci, tl.col_widths[c], tbl.ColAlign(c)) + TABLE_CELL_PADDING * 2.0f);
        if (bgs) {
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
std::pmr::vector<size_t> FindRepresentativeCells(const NodeTableData& tbl, TaskScheduler* scheduler)
{
    std::pmr::vector<size_t> reps(static_cast<size_t>(tbl.row_count) * tbl.col_count);
    ParallelFor(scheduler, tbl.col_count, 1, [&](size_t col_begin, size_t col_end) {
        for (size_t c = col_begin; c < col_end; ++c) {
            std::pmr::monotonic_buffer_resource arena;
            std::pmr::unordered_map<std::string_view, size_t> first_rows(&arena);
            // arena は伸長で手放したバケットを返さないので、先に広げておく。
            first_rows.reserve(tbl.row_count);
            for (size_t r = 0; r < tbl.row_count; ++r) {
                const size_t ci = tbl.CellIndex(r, c);
                reps[ci] = ci;
                const auto text = tbl.GetCellText(r, c);
                if (text.empty() || tbl.IsHeaderRow(r)) {
                    continue;
                }
                // 同じテキストで装飾が違うセルは、最初のものだけを代表にして自分で測る。
                const auto [it, inserted] = first_rows.try_emplace(text, r);
                if (!inserted && std::ranges::equal(tbl.GetCellRuns(r, c), tbl.GetCellRuns(it->second, c))) {
                    reps[ci] = tbl.CellIndex(it->second, c);
                }
            }
        }
    });
    return reps;
}

// 巨大テーブルを行チャンクで並列処理するときの 1 チャンクの行数。1 画面分の行の復元 (数百セル) も
// ワーカーに行き渡る細かさにする (2 万行 6 列で 256 セルだと復元 4.7ms、32 セルで 1.2ms。初回計測は変わらない)。
constexpr size_t TableRowsPerChunk(size_t col_count) noexcept
{
    constexpr size_t kCellsPerChunk = 32;
    return std::max<size_t>(1, kCellsPerChunk / std::max<size_t>(col_count, 1));
}

// 1 行目の高さを取得し entry にキャッシュする。layout 自体は変えない。
void CacheFirstLineHeight(IDWriteTextLayout* layout, NodeLayoutEntry& entry) noexcept
{
    DWRITE_LINE_METRICS lm{};
    UINT32 lc = 0;
    // 複数行レイアウトでは E_NOT_SUFFICIENT_BUFFER が返るが、先頭 1 行分は lm に書き込まれる。
    const HRESULT hr = layout->GetLineMetrics(&lm, 1, &lc);
    entry.set_first_line_height(((SUCCEEDED(hr) || hr == E_NOT_SUFFICIENT_BUFFER) && lc > 0) ? lm.height : 0.0f);
}

// entry.text_layout の計測結果から高さを返す。折り返し行が変わるためハイライト矩形は無効化し、
// インラインコード背景を作り直す (描画色と下線は SetMaxWidth だけの再計測でも残る)。
float FinishTextMeasure(const Node& node, NodeLayoutEntry& entry, const DWRITE_TEXT_METRICS& metrics, const Theme& theme)
{
    CacheFirstLineHeight(entry.text_layout(), entry);
    entry.invalidate_per_frame_hl_caches();
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
    if (node.GetText().empty()) {
        return layout::EmptyTextNodeHeight(node, theme);
    }
    return std::nullopt;
}

// 既存の text_layout を SetMaxWidth で再計測する。text_layout は内容変更時に呼び出し側
// (LayoutCache::InvalidateAllLayouts / EvictTextLayouts) で必ず Reset される契約のため、
// 現存していればテキスト/runs/フォント幾何は一致している。SetMaxWidth の後の GetMetrics は解析からやり直すので
// 作り直しより 1〜16% 速いだけだが、装飾の付け直しは要らない。
// 戻り値: 成功したら高さ。失敗時は layout を捨ててスローパスに委ねる。
std::optional<float> RemeasureExistingLayout(const Node& node, NodeLayoutEntry& entry, float layout_width, const Theme& theme)
{
    MENDO_PROFILE("MeasureNode.fastpath");
    if (SUCCEEDED(entry.text_layout()->SetMaxWidth(layout_width))) {
        DWRITE_TEXT_METRICS metrics{};
        entry.text_layout()->GetMetrics(&metrics);
        return FinishTextMeasure(node, entry, metrics, theme);
    }
    entry.release_layout_state();
    return std::nullopt;
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
std::pmr::vector<SyntaxToken> ToWideMergedTokens(std::string_view text, std::pmr::vector<SyntaxToken> tokens)
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

void DWriteTextMeasurer::WarmUp(TaskScheduler& scheduler) const
{
    if (!ctx_.body) {
        return;
    }
    // 各書式で日本語を含む小さなレイアウトを測り、フォントの読み込みとフォールバックの解決を済ませる。
    // 見出しはサイズしか違わず、フォントの読み込みとフォールバックの解決は 1 つで済む。
    static constexpr auto warm = [](const DWriteContext& ctx) {
        MENDO_PROFILE("DWriteTextMeasurer::WarmUp");
        constexpr std::wstring_view kSample = L"Aa1 日本語のテキスト";
        for (IDWriteTextFormat* fmt : { ctx.body.Get(), ctx.code.Get(), ctx.h[0].Get() }) {
            ComPtr<IDWriteTextLayout> layout;
            if (fmt && SUCCEEDED(ctx.factory->CreateTextLayout(kSample.data(), static_cast<UINT32>(kSample.size()), fmt, layout::kNoWrapLayoutWidth, layout::kNoWrapLayoutWidth, &layout))) {
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
            }
        }
    };
    // ワーカーの組はタスクを取ったワーカーで作られる。作り終えたワーカーは全員がそろうまで待ち、1 本に 1 つずつ行き渡らせる。
    // 計測と違って UI スレッドを待たせないので、その間にズームなどで書き換わるテーマと ctx_ は読まず、今の値を渡す。
    const auto workers = static_cast<std::ptrdiff_t>(scheduler.WorkerCount());
    const auto arrived = std::make_shared<std::latch>(workers);
    const auto theme = std::make_shared<const Theme>(*theme_);
    for (std::ptrdiff_t i = 0; i < workers; ++i) {
        if (!scheduler.Post([theme, generation = ctx_.generation, arrived] {
            if (const auto* ctx = ThreadContext(*theme, generation)) {
                warm(*ctx);
            }
            arrived->arrive_and_wait();
        })) {
            arrived->count_down();
        }
    }
    // 共有ファクトリ (UI スレッドの計測用) は書式の参照を持って渡し、UI スレッドが作り直しても途中で解放されないようにする。
    scheduler.Post([ctx = ctx_] { warm(ctx); });
}

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
    return ctx.body && ctx.code && std::ranges::all_of(ctx.h, [](const auto& f) static { return f != nullptr; });
}

bool DWriteTextMeasurer::RecreateFormats()
{
    if (!ctx_.factory || !theme_) {
        return false;
    }
    static std::atomic<uint64_t> s_generation{ 0 };
    ctx_.generation = ++s_generation;
    owner_thread_ = std::this_thread::get_id();
    return CreateFormats(*theme_, ctx_);
}

const DWriteTextMeasurer::DWriteContext* DWriteTextMeasurer::ThreadContext(const Theme& theme, uint64_t generation)
{
    thread_local DWriteContext t_ctx;
    if (t_ctx.generation != generation) {
        if (!t_ctx.factory) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(t_ctx.factory.GetAddressOf()));
        }
        if (!t_ctx.factory || !CreateFormats(theme, t_ctx)) {
            return nullptr;
        }
        t_ctx.generation = generation;
    }
    return &t_ctx;
}

const DWriteTextMeasurer::DWriteContext& DWriteTextMeasurer::Context() const
{
    if (std::this_thread::get_id() == owner_thread_) {
        return ctx_;
    }
    // 作れなければ共有ファクトリで測る (競合で遅いだけで結果は同じ)。
    const auto* ctx = ThreadContext(*theme_, ctx_.generation);
    return ctx ? *ctx : ctx_;
}

IDWriteTextFormat* DWriteTextMeasurer::GetTextFormat(const DWriteContext& ctx, const Node& node) noexcept
{
    if (node.type == NodeType::CodeBlock) {
        return ctx.code.Get();
    }
    if (node.type == NodeType::Heading) {
        const int8_t lv = node.heading_level();
        if (lv >= 1 && lv <= 6) {
            return ctx.h[lv - 1].Get();
        }
    }
    return ctx.body.Get();
}

IDWriteTextFormat* DWriteTextMeasurer::GetTableRowFormat(const DWriteContext& ctx, const NodeTableData& tbl, size_t r) noexcept
{
    return tbl.IsHeaderRow(r) ? ctx.h[3].Get() : ctx.body.Get();
}

void DWriteTextMeasurer::ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text, bool apply_code_size) const
{
    if (runs.empty()) {
        return;
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

    // runs は start 昇順・非重複なので、境界の UTF-16 位置は前方への 1 回の走査で求まる。
    Utf16OffsetCursor cursor{ text };
    for (const auto& r : runs) {
        const DWRITE_TEXT_RANGE wr = cursor.WideRange(r.start, r.length);
        bold.Update(r.bold(), wr);
        italic.Update(r.italic(), wr);
        code.Update(r.code(), wr);
        if (r.code() && r.length > 0) {
            code_tail_units = utf8_codec::DecodePrev(text, r.start + r.length).cp > 0xFFFF ? 2 : 1;
        }
        strike.Update(r.strikethrough(), wr);
        link.Update(r.has_link(), wr);
    }

    bold.Flush();
    italic.Flush();
    code.Flush();
    strike.Flush();
    link.Flush();
}

std::optional<float> DWriteTextMeasurer::MeasureNode(
    const Node& node, NodeLayoutEntry& entry, float max_width,
    YRange viewport) const
{
    MENDO_PROFILE("MeasureNode");
    if (!ctx_.factory || !theme_) {
        return std::nullopt;
    }
    if (node.type == NodeType::Table) {
        return MeasureTable(node, entry, max_width, viewport);
    }
    if (const auto height = MeasureWithoutTextLayout(node, *theme_)) {
        return height;
    }

    const float layout_width = layout::NodeLayoutWidth(node, max_width);
    // 同じ幅で組んだ text_layout が残っていれば、高さも行もハイライト矩形も変わらない。
    if (entry.text_layout() && entry.cached_height > 0.0f && entry.cached_width == layout_width) {
        return entry.cached_height;
    }
    if (entry.text_layout()) {
        // 改行でしか折れておらず最も長い行が新しい幅に収まるなら、組み直しても行は同じ。左揃えなので幅は付け替えない
        // (SetMaxWidth の後の GetMetrics は作り直しとほぼ同じだけかかる)。GetMetrics は前回の結果を返すだけ。
        // 幅を変えたときの全ノードの計測し直しが test.md で 174ms → 143ms、README で 6.0ms → 3.1ms。
        DWRITE_TEXT_METRICS metrics{};
        entry.text_layout()->GetMetrics(&metrics);
        if (metrics.lineCount == static_cast<UINT32>(node.line_count) + 1 && metrics.width <= layout_width) {
            return metrics.height;
        }
        if (const auto height = RemeasureExistingLayout(node, entry, layout_width, *theme_)) {
            return height;
        }
    }

    const auto& text = node.GetText();
    const auto& ctx = Context();
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateDocTextLayout(ctx.factory.Get(), text, GetTextFormat(ctx, node), layout_width, layout::kNoWrapLayoutWidth, &layout))) {
        return std::nullopt;
    }

    ApplyRunFormatting(layout.Get(), node.runs(), text, /*apply_code_size=*/node.type != NodeType::Heading);
    if (node.type == NodeType::BlockQuote && node.alert_label_length() > 0) {
        if (const auto idx = AlertColorIndex(node.alert_type); idx < ALERT_TYPE_COUNT) {
            Utf16OffsetCursor cursor{ text };
            layout->SetDrawingEffect(BrushEffect::For(AlertBrushIdFromIndex(idx)), cursor.WideRange(0, node.alert_label_length()));
        }
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    // 描画パスでのトークン化によるフレーム落ちを避けるため、ここで作る。
    // layout と一緒に捨てるので、画面外のコードブロックのトークンは残らない。
    if (node.type == NodeType::CodeBlock && node.code_language() != SyntaxLanguage::None) {
        entry.set_syntax_tokens(ToWideMergedTokens(text, TokenizeCodeNode(node)));
    }

    entry.set_text_layout(std::move(layout));
    return FinishTextMeasure(node, entry, metrics, *theme_);
}

ComPtr<IDWriteTextLayout> DWriteTextMeasurer::CreateCellLayout(const DWriteContext& ctx, const NodeTableData& tbl, size_t r, size_t c) const
{
    ComPtr<IDWriteTextLayout> cell_layout;
    const auto text = tbl.GetCellText(r, c);
    if (!text.empty() && SUCCEEDED(CreateDocTextLayout(ctx.factory.Get(), text, GetTableRowFormat(ctx, tbl, r), layout::kNoWrapLayoutWidth, layout::kNoWrapLayoutWidth, &cell_layout))) {
        ApplyRunFormatting(cell_layout.Get(), tbl.GetCellRuns(r, c), text, /*apply_code_size=*/true);
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

void DWriteTextMeasurer::MeasureTableCells(const NodeTableData& tbl, TableLayoutData& tl) const
{
    MENDO_PROFILE("MeasureTableCells");
    const size_t col_count = tbl.col_count;
    auto& natural_widths = tl.natural_col_widths;
    natural_widths.assign(col_count, 0.0f);
    const bool keep_layouts = tl.cell_layouts.size() <= kMaxKeptTableCells;
    // layout を残す小さい表は、全セルの layout が要るので代表を探さない (空のまま)。
    const auto reps = keep_layouts ? std::pmr::vector<size_t>{} : FindRepresentativeCells(tbl, scheduler_);

    // 巨大テーブルは全セルの CreateTextLayout が秒単位になるため行チャンクで並列化する。
    // セルは chunk ごとに別 index へ書くので排他不要。残さない layout は worker 上で解放する。
    ParallelFor(scheduler_, tbl.row_count, TableRowsPerChunk(col_count), [&](size_t row_begin, size_t row_end) {
        const auto& ctx = Context();
        for (size_t r = row_begin; r < row_end; r++) {
            for (size_t c = 0; c < col_count; c++) {
                const size_t ci = tl.CellIndex(r, c);
                if (!reps.empty() && reps[ci] != ci) {
                    continue;
                }
                auto cell_layout = CreateCellLayout(ctx, tbl, r, c);
                if (!cell_layout) {
                    continue;
                }
                DWRITE_TEXT_METRICS metrics{};
                cell_layout->GetMetrics(&metrics);
                tl.cell_natural_widths[ci] = metrics.width;
                tl.cell_nowrap_heights[ci] = metrics.height;
                if (keep_layouts) {
                    tl.cell_layouts[ci] = std::move(cell_layout);
                }
            }
        }
    });
    // 代表と同じ中身のセルは代表の値を写す (代表は別の chunk にあるので測り終えてから)。
    // 列の自然幅もここで集める。測れなかったセルと空のセルは確保したときの 0 のままなので効かない。
    for (size_t r = 0; r < tbl.row_count; r++) {
        for (size_t c = 0; c < col_count; c++) {
            const size_t ci = tl.CellIndex(r, c);
            if (!reps.empty()) {
                tl.cell_natural_widths[ci] = tl.cell_natural_widths[reps[ci]];
                tl.cell_nowrap_heights[ci] = tl.cell_nowrap_heights[reps[ci]];
            }
            natural_widths[c] = std::max(natural_widths[c], tl.cell_natural_widths[ci]);
        }
    }
    if (!keep_layouts) {
        tl.MarkAllRowsEvicted();
    }
}

std::pmr::vector<size_t> DWriteTextMeasurer::RestoreRowsInViewport(
    const NodeTableData& tbl, TableLayoutData& tl, YRange viewport, std::pmr::vector<float>* row_heights) const
{
    MENDO_PROFILE("RestoreRowsInViewport");
    const auto [r_begin, r_end] = tl.VisibleRowRange(viewport.top, viewport.bottom);
    std::pmr::vector<size_t> rows;
    for (size_t r = r_begin; r < r_end; r++) {
        if (tl.row_evicted[r]) {
            rows.push_back(r);
        }
    }
    if (rows.empty()) {
        return rows;
    }
    // 戻りスクロールで 1 画面分の行がまとめて戻るので、MeasureTableCells と同じく行チャンクで並列化する。
    // 行ごとの書き込み先は重ならない。evict の管理情報 (MarkRowRestored) は後で直列に更新する。
    const float base_row_height = layout::TableBaseRowHeight(*theme_);
    if (row_heights) {
        row_heights->resize(rows.size());
    }
    ParallelFor(scheduler_, rows.size(), TableRowsPerChunk(tl.col_count), [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; k++) {
            BuildRowCells(tbl, tl, rows[k]);
            if (row_heights) {
                (*row_heights)[k] = MeasureRowHeight(tl, tbl, rows[k], base_row_height);
            }
        }
    });
    for (const size_t r : rows) {
        tl.MarkRowRestored(r);
    }
    return rows;
}

void DWriteTextMeasurer::BuildRowCells(const NodeTableData& tbl, TableLayoutData& tl, size_t r) const
{
    const auto& ctx = Context();
    for (size_t c = 0; c < tbl.col_count; c++) {
        const size_t ci = tl.CellIndex(r, c);
        if (!tl.cell_layouts[ci]) {
            tl.cell_layouts[ci] = CreateCellLayout(ctx, tbl, r, c);
        }
    }
}

float DWriteTextMeasurer::FinalizeTableLayout(const NodeTableData& tbl, TableLayoutData& tl, float max_width) const
{
    MENDO_PROFILE("FinalizeTableLayout");
    const float cell_padding = TABLE_CELL_PADDING;
    const float border_width = TABLE_BORDER_WIDTH;
    const size_t col_count = tl.col_count;
    const float borders_width = (static_cast<float>(col_count) + 1.0f) * border_width;
    const float paddings_width = static_cast<float>(col_count) * cell_padding * 2.0f;

    layout::ComputeColumnWidths(tl.col_widths, tl.natural_col_widths, max_width - borders_width - paddings_width);

    // 幅の適用 (SetMaxWidth + GetMetrics) は全セル分かかる。行ごとの書き込み先
    // (row_heights[r] と行内セルの幅・高さキャッシュ) は重ならないので行チャンクで並列化する。
    const float base_row_height = layout::TableBaseRowHeight(*theme_);
    ParallelFor(scheduler_, tbl.row_count, TableRowsPerChunk(col_count), [&](size_t row_begin, size_t row_end) {
        for (size_t r = row_begin; r < row_end; r++) {
            tl.row_heights[r] = MeasureRowHeight(tl, tbl, r, base_row_height);
        }
    });

    // evict 行が残っていても確定させる (dirty に残さない)。dirty のままだと可視中は毎フレーム全行の
    // Finalize が走る。残りは RestoreEvictedTableRows が可視行だけ埋める。
    tl.CommitGeometry(max_width);
    return tl.TotalHeight();
}

TableRestoreResult DWriteTextMeasurer::RestoreEvictedTableRows(const Node& node, NodeLayoutEntry& entry, float max_width, YRange viewport) const
{
    MENDO_PROFILE("RestoreEvictedTableRows");
    TableRestoreResult result;
    auto* tl_ptr = entry.table_layout();
    const auto* tbl = node.table_data();
    if (!ctx_.factory || !theme_ || !tl_ptr || !tbl || !tl_ptr->HasEvictedRows()) {
        return result;
    }
    auto& tl = *tl_ptr;
    const size_t row_count = tbl->row_count;
    // 幾何が無効 (InvalidateGeometry 後) か幅が変わっていれば dirty 経由の MeasureTable (Finalize) が担当する。
    if (!tl.GeometryValidAt(max_width)) {
        return result;
    }

    std::pmr::vector<float> row_heights;
    const auto rows = RestoreRowsInViewport(*tbl, tl, viewport, &row_heights);
    if (rows.empty()) {
        return result;
    }
    result.restored = true;

    size_t first_changed_row = row_count;
    for (size_t k = 0; k < rows.size(); k++) {
        const size_t r = rows[k];
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

std::optional<float> DWriteTextMeasurer::MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width,
                                                      YRange viewport) const
{
    MENDO_PROFILE("MeasureTable");
    const auto* tbl = node.table_data();
    // パーサが NodeTableData::col_count に最大列数を保持済みなので全行走査は不要。
    if (!tbl || tbl->row_count == 0 || tbl->col_count == 0) {
        return 0.0f;
    }

    const size_t row_count = tbl->row_count;
    const size_t col_count = tbl->col_count;

    // 超高速パス: 前回と max_width がほぼ一致し幾何が揃っていれば、
    // セル幅・行高さ・累積位置・行オフセットすべて変化しないため、今の高さのまま確定させる。
    // 検索ハイライト矩形・inline_code_bgs もテキスト位置に依存するので保持できる。
    // evict 済み行のセルは RestoreEvictedTableRows が可視になった時点で再生成する。
    // 幾何が無効 (EstimateInvisibleNodeHeight が InvalidateGeometry 済み) のまま通すと
    // GenTable が空テーブルを描画し続ける。
    auto* tl_existing = entry.table_layout();
    if (tl_existing && tl_existing->GeometryValidAt(max_width)) {
        return entry.height;
    }

    // セル layout の再作成 or SetMaxWidth で metrics が変わるため、ハイライト矩形の
    // キャッシュは捨てる。選択ハイライトキャッシュは本文ノード専用だが、ノード型変更等で
    // 残っている可能性に備えて落としておく。
    entry.invalidate_per_frame_hl_caches();

    // インラインコードの背景は行の計測 (MeasureRowHeight) で作る。持たないテーブルには確保しない。
    auto& tl = tl_existing ? *tl_existing
                           : entry.ensure_table_layout(row_count, col_count, std::ranges::any_of(tbl->all_runs, [](const TextRun& r) static {
                                 return r.code() && r.length > 0;
                             }));
    assert(tl.row_count == row_count && tl.col_count == col_count);

    // 全セルを測り終えていれば、テキストレイアウトの作成をスキップして列幅の再計算だけ行う。
    // 自然幅はセルのテキストだけで決まり、evict しても残る。
    if (tl.HasRowGeometry()) {
        // 行高さは FinalizeTableLayout が新しい列幅で全行測る。
        RestoreRowsInViewport(*tbl, tl, viewport, nullptr);
    }
    else {
        // 列幅判定に全行の自然幅が要るので、初回は全セルを測る。
        MeasureTableCells(*tbl, tl);
    }
    return FinalizeTableLayout(*tbl, tl, max_width);
}
