#include "dwrite_measurer.h"
#include "doc_dwrite_bridge.h"
#include "layout_computer.h"
#include "parallel_for.h"
#include "profiler.h"
#include "syntax.h"
#include "ui_constants.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <ranges>

using Microsoft::WRL::ComPtr;

namespace {

// CodeBlock は SetWordWrapping(NO_WRAP) のため max_width は折り返し計算に使われない。
// MaxHeight も「事実上の上限」で十分なので、両方ともこの単一定数で運用する。
// 1e7 は float の整数精度限界 (2^24 ≈ 1.67e7) より少し下で、DirectWrite 内部の
// 幾何計算でも丸め誤差が乗らない安全な値。10MDIP ≈ 数十万行のコードブロックを許容する。
constexpr float LAYOUT_INFINITY = 1.0e7f;
// セル幅がこれ以下の差分なら前回の計測高さを再利用する
constexpr float CELL_WIDTH_EPSILON = 0.5f;

// セルに列幅と揃えを適用して高さを返す。幅不変ならキャッシュ済みの高さを使う。
float ApplyCellWidth(TableLayoutData& tl, size_t ci, float cw, TableAlign align) noexcept
{
    const bool width_unchanged = std::abs(tl.cell_applied_widths[ci] - cw) < CELL_WIDTH_EPSILON && tl.cell_heights[ci] > 0.0f;
    if (!width_unchanged) {
        auto* layout = tl.cell_layouts[ci].Get();
        layout->SetMaxWidth(cw);
        if (align == TableAlign::Center) {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        else if (align == TableAlign::Right) {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        tl.cell_heights[ci] = metrics.height;
        tl.cell_applied_widths[ci] = cw;
    }
    return tl.cell_heights[ci];
}

struct RowHeightResult {
    float height;
    // 本文があるのに layout が無いセル (= 範囲外で evict されたまま) を含む
    bool has_unbuilt_cell;
};

// 構築済みセルに列幅を適用し、行高さ (上下パディング込み、base_row_height 以上) を求める。
RowHeightResult MeasureRowHeight(TableLayoutData& tl, const NodeTableData& tbl, size_t r, float base_row_height) noexcept
{
    RowHeightResult result{ base_row_height, false };
    for (size_t c = 0; c < tl.col_count; c++) {
        const size_t ci = tl.CellIndex(r, c);
        if (tl.cell_layouts[ci]) {
            result.height = std::max(result.height, ApplyCellWidth(tl, ci, tl.col_widths[c], tbl.ColAlign(c)) + TABLE_CELL_PADDING * 2.0f);
        }
        else if (!tbl.GetCellText(r, c).empty()) {
            result.has_unbuilt_cell = true;
        }
    }
    return result;
}

// 巨大テーブルを行チャンクで並列処理するときの 1 チャンクの行数。
constexpr size_t TableRowsPerChunk(size_t col_count) noexcept
{
    constexpr size_t kCellsPerChunk = 256;
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

// entry.text_layout の計測結果を確定する。折り返し行が変わるためハイライト矩形は無効化する。
// reused_layout (SetMaxWidth のみ) では SetDrawingEffect/SetUnderline の範囲が残るので、
// 折り返しに依存するインラインコード背景だけを作り直させる。
void FinishTextMeasure(const Node& node, NodeLayoutEntry& entry, const DWRITE_TEXT_METRICS& metrics, bool reused_layout) noexcept
{
    entry.height = metrics.height;
    entry.layout_dirty = false;
    CacheFirstLineHeight(entry.text_layout(), entry);
    entry.invalidate_per_frame_hl_caches();
    if (node.type == NodeType::CodeBlock) {
        entry.set_natural_code_width(metrics.widthIncludingTrailingWhitespace);
        // NO_WRAP なので再利用時は行も変わらず、インラインコード背景も持たない。
        if (reused_layout) {
            return;
        }
    }
    entry.clear_inline_code_bgs();
    if (reused_layout) {
        entry.set_inline_code_bgs_stale(true);
    }
    else {
        entry.set_effects_applied(false);
    }
}

// テキストレイアウトを持たないノード (HR / ダイアグラム / 画像 / 空テキスト) の高さを確定する。
// 戻り値: 処理した場合 true。
bool MeasureWithoutTextLayout(const Node& node, NodeLayoutEntry& entry, const Theme& theme) noexcept
{
    if (node.type == NodeType::HorizontalRule) {
        entry.height = theme.paragraph_spacing + theme.hr_thickness;
    }
    // ダイアグラム / 画像: 高さはビットマップ側で決まる (画像は MeasureEntry が寸法から求める)。
    // それまではプレースホルダー高さ。
    else if (IsBitmapDrawNode(node)) {
        if (entry.height <= 0) {
            entry.height = layout::PlaceholderHeight(theme);
        }
    }
    else if (node.GetText().empty()) {
        // loose LI で paragraph_spacing を入れると bullet と直下 P の文字 Y が分離する (issue#237)。
        entry.height = IsEmptyListItemContainer(node) ? 0.0f
            : IsListItem(node)                     ? layout::EmptyListItemHeight(theme)
                                                   : theme.paragraph_spacing;
    }
    else {
        return false;
    }
    entry.layout_dirty = false;
    return true;
}

// 既存の text_layout を SetMaxWidth で再計測する。text_layout は内容変更時に呼び出し側
// (LayoutCache::InvalidateAllLayouts / EvictTextLayouts) で必ず Reset される契約のため、
// 現存していればテキスト/runs/フォント幾何は一致している。CreateTextLayout は BiDi 解析と
// shaping を走らせるためリサイズ時の最大コスト要因で、SetMaxWidth はラインブレーク再計算のみで済む。
// 戻り値: 成功したら true。失敗時は layout を捨ててスローパスに委ねる。
bool RemeasureExistingLayout(const Node& node, NodeLayoutEntry& entry, float layout_width) noexcept
{
    MENDO_PROFILE("MeasureNode.fastpath");
    if (SUCCEEDED(entry.text_layout()->SetMaxWidth(layout_width))) {
        DWRITE_TEXT_METRICS metrics{};
        entry.text_layout()->GetMetrics(&metrics);
        FinishTextMeasure(node, entry, metrics, true);
        return true;
    }
    entry.reset_text_layout();
    return false;
}

HRESULT CreateFormat(IDWriteFactory* factory, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out)
{
    return factory->CreateTextFormat(
        family, nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"ja-jp", out);
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

} // namespace

bool DWriteTextMeasurer::RecreateFormats()
{
    if (!dwrite_ || !theme_) {
        return false;
    }

    fmt_body_.Reset();
    for (auto& fmt : fmt_h_) {
        fmt.Reset();
    }
    fmt_code_.Reset();

    const auto W = DWRITE_FONT_WEIGHT_NORMAL;
    const auto B = DWRITE_FONT_WEIGHT_BOLD;

    if (FAILED(CreateFormat(dwrite_, theme_->font_family.c_str(), theme_->font_size_body, W, &fmt_body_))) {
        return false;
    }
    for (const auto i : std::views::iota(0, 6)) {
        if (FAILED(CreateFormat(dwrite_, theme_->font_family.c_str(), theme_->font_size_h[i], B, &fmt_h_[i]))) {
            return false;
        }
    }
    if (FAILED(CreateFormat(dwrite_, theme_->monospace_font.c_str(), theme_->font_size_code, W, &fmt_code_))) {
        return false;
    }

    fmt_body_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    for (auto& fmt : fmt_h_) {
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    fmt_code_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

IDWriteTextFormat* DWriteTextMeasurer::GetTextFormat(const Node& node) const noexcept
{
    if (node.type == NodeType::CodeBlock) {
        return fmt_code_.Get();
    }
    if (node.type == NodeType::Heading) {
        const int8_t lv = node.heading_level();
        if (lv >= 1 && lv <= 6) {
            return fmt_h_[lv - 1].Get();
        }
    }
    return fmt_body_.Get();
}

IDWriteTextFormat* DWriteTextMeasurer::GetTableRowFormat(const NodeTableData& tbl, size_t r) const noexcept
{
    return tbl.IsHeaderRow(r) ? fmt_h_[3].Get() : fmt_body_.Get();
}

void DWriteTextMeasurer::ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::string_view text, RunFormatScope scope) const
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
    AttrRangeBuilder code{ [this, layout, apply_code_size = scope.apply_code_size](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontFamilyName(theme_->monospace_font.c_str(), r);
        if (apply_code_size) {
            layout->SetFontSize(theme_->font_size_code, r);
        }
    } };
    AttrRangeBuilder strike{ [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetStrikethrough(TRUE, r);
    } };
    AttrRangeBuilder link{ [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetUnderline(TRUE, r);
    } };

    // runs は start 昇順・非重複なので、境界の UTF-16 位置は前方への 1 回の走査で求まる。
    Utf16OffsetCursor cursor{ text };
    for (const auto& r : runs) {
        const DWRITE_TEXT_RANGE wr = cursor.WideRange(r.start, r.length);
        bold.Update(r.bold(), wr);
        italic.Update(r.italic(), wr);
        if (scope.apply_code) {
            code.Update(r.code(), wr);
        }
        strike.Update(r.strikethrough(), wr);
        if (scope.apply_link) {
            link.Update(r.has_link(), wr);
        }
    }

    bold.Flush();
    italic.Flush();
    code.Flush();
    strike.Flush();
    link.Flush();
}

// 描画時に UTF-16 位置へ数え直さないよう、ワーカー上で UTF-16 単位にし、描画時のグリフラン分割を減らす。
// 空白は色が見えないので、トークン間の空白だけの隙間は直前のトークンに含め、同種のトークンはまとめる。
static std::pmr::vector<SyntaxToken> ToWideMergedTokens(std::string_view text, std::pmr::vector<SyntaxToken> tokens)
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
    tokens.shrink_to_fit();
    return tokens;
}

void DWriteTextMeasurer::MeasureNode(
    const Node& node, NodeLayoutEntry& entry, float max_width,
    MeasureViewportRange viewport) const
{
    MENDO_PROFILE("MeasureNode");
    if (!dwrite_ || !theme_) {
        return;
    }
    if (node.type == NodeType::Table) {
        MeasureTable(node, entry, max_width, viewport);
        return;
    }
    if (MeasureWithoutTextLayout(node, entry, *theme_)) {
        return;
    }

    // CodeBlock は fmt_code_ が NO_WRAP なので、高さも行もハイライト矩形も幅に依存しない。
    if (node.type == NodeType::CodeBlock && entry.text_layout() && entry.cached_height > 0.0f) {
        entry.height = entry.cached_height;
        entry.layout_dirty = false;
        return;
    }
    const float layout_width = (node.type == NodeType::CodeBlock) ? LAYOUT_INFINITY : max_width;
    if (entry.text_layout() && RemeasureExistingLayout(node, entry, layout_width)) {
        return;
    }

    const auto& text = node.GetText();
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(CreateDocTextLayout(dwrite_, text, GetTextFormat(node), layout_width, LAYOUT_INFINITY, &layout))) {
        return;
    }

    ApplyRunFormatting(layout.Get(), node.runs(), text, RunFormatScope::ForNode(node.type));

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    // 描画パスでのトークン化によるフレーム落ちを避けるため、ここで作る。
    // layout と一緒に捨てるので、画面外のコードブロックのトークンは残らない。
    if (node.type == NodeType::CodeBlock && node.code_language() != SyntaxLanguage::None) {
        entry.set_syntax_tokens(ToWideMergedTokens(text, Tokenize(text, node.code_language())));
    }

    entry.set_text_layout(std::move(layout));
    FinishTextMeasure(node, entry, metrics, false);
}

void DWriteTextMeasurer::BuildCellLayout(const NodeTableData& tbl, size_t r, size_t c, IDWriteTextFormat* row_fmt, TableLayoutData& tl) const
{
    const auto text = tbl.GetCellText(r, c);
    if (text.empty()) {
        return;
    }
    auto& cell_layout = tl.cell_layouts[tl.CellIndex(r, c)];
    CreateDocTextLayout(dwrite_, text, row_fmt, LAYOUT_INFINITY, LAYOUT_INFINITY, &cell_layout);
    if (cell_layout) {
        ApplyRunFormatting(cell_layout.Get(), tbl.GetCellRuns(r, c), text, RunFormatScope::ForCell());
    }
}

void DWriteTextMeasurer::MeasureTableCells(const NodeTableData& tbl, TableLayoutData& tl) const
{
    MENDO_PROFILE("MeasureTableCells");
    const size_t col_count = tbl.col_count;
    auto& natural_widths = tl.natural_col_widths;
    natural_widths.assign(col_count, 0.0f);

    // 巨大テーブルは全セルの CreateTextLayout が秒単位になるため行チャンクで並列化する。
    // セルは chunk ごとに別 index へ書くので排他不要で、自然幅だけ chunk 末尾でマージする。
    std::mutex merge_mutex;
    ParallelFor(scheduler_, tbl.row_count, TableRowsPerChunk(col_count), [&](size_t row_begin, size_t row_end) {
        std::pmr::vector<float> local_widths(col_count, 0.0f);
        for (size_t r = row_begin; r < row_end; r++) {
            IDWriteTextFormat* const row_fmt = GetTableRowFormat(tbl, r);
            for (size_t c = 0; c < col_count; c++) {
                BuildCellLayout(tbl, r, c, row_fmt, tl);
                if (const auto& cell_layout = tl.cell_layouts[tl.CellIndex(r, c)]) {
                    DWRITE_TEXT_METRICS metrics{};
                    cell_layout->GetMetrics(&metrics);
                    local_widths[c] = std::max(local_widths[c], metrics.width);
                }
            }
        }
        const std::lock_guard lock(merge_mutex);
        for (size_t c = 0; c < col_count; c++) {
            natural_widths[c] = std::max(natural_widths[c], local_widths[c]);
        }
    });
}

void DWriteTextMeasurer::RestoreNullCellLayouts(const NodeTableData& tbl, TableLayoutData& tl, MeasureViewportRange viewport) const
{
    // EvictInvisibleTableRows で Reset された null セルを再生成する。
    // viewport が部分範囲なら、その範囲外の行はスキップして CreateTextLayout を回避する。
    MENDO_PROFILE("RestoreNullCellLayouts");
    const auto [r_begin, r_end] = tl.RowsInViewport(tbl.row_count, viewport.top, viewport.bottom);
    for (size_t r = r_begin; r < r_end; r++) {
        if (tl.row_evicted[r]) {
            RestoreRowCells(tbl, tl, r);
        }
    }
}

void DWriteTextMeasurer::RestoreRowCells(const NodeTableData& tbl, TableLayoutData& tl, size_t r) const
{
    IDWriteTextFormat* const row_fmt = GetTableRowFormat(tbl, r);
    for (size_t c = 0; c < tbl.col_count; c++) {
        const size_t ci = tl.CellIndex(r, c);
        if (ci < tl.cell_layouts.size() && !tl.cell_layouts[ci]) {
            BuildCellLayout(tbl, r, c, row_fmt, tl);
        }
    }
    tl.MarkRowRestored(r);
}

void DWriteTextMeasurer::FinalizeTableLayout(const NodeTableData& tbl, NodeLayoutEntry& entry, float max_width) const
{
    MENDO_PROFILE("FinalizeTableLayout");
    const float cell_padding = TABLE_CELL_PADDING;
    const float border_width = TABLE_BORDER_WIDTH;
    auto& tl = *entry.table_layout();
    const size_t col_count = tl.col_count;
    const auto& natural_widths = tl.natural_col_widths;
    const float borders_width = (static_cast<float>(col_count) + 1.0f) * border_width;
    const float paddings_width = static_cast<float>(col_count) * cell_padding * 2.0f;

    layout::ComputeColumnWidths(tl.col_widths, natural_widths, max_width - borders_width - paddings_width, col_count);

    // 幅の適用 (SetMaxWidth + GetMetrics) は全セル分かかる。行ごとの書き込み先
    // (row_heights[r] と行内セルの幅・高さキャッシュ) は重ならないので行チャンクで並列化する。
    const float base_row_height = layout::TableBaseRowHeight(*theme_);
    ParallelFor(scheduler_, tbl.row_count, TableRowsPerChunk(col_count), [&](size_t row_begin, size_t row_end) {
        for (size_t r = row_begin; r < row_end; r++) {
            const auto [row_height, has_unbuilt_cell] = MeasureRowHeight(tl, tbl, r, base_row_height);
            // 部分復元時は null セルがある行 = 範囲外 evict 行。再計算で行高さを既定値に戻すと
            // 累積位置がずれるため、既存の row_heights[r] を保持する (復元時に実測で補正される)。
            if (!has_unbuilt_cell || tl.row_heights[r] <= 0.0f) {
                tl.row_heights[r] = row_height;
            }
        }
    });

    // ヒットテスト高速化用に行Y累積と列X累積を事前計算
    tl.RebuildRowCumY(tbl.row_count);
    tl.RebuildColCumX();
    const float total_height = tl.row_cum_y[tbl.row_count] + border_width;

    // col_cum_x の末尾は border_width + Σ(col_w + 2*pad + border) と一致するため再計算しない。
    tl.cached_table_width = tl.col_cum_x.back();

    // 圧縮分岐に入った場合 cached_table_width は自然総幅と乖離する。
    // 横スクロールのクランプ計算は natural_total_width を基準にする。
    float natural_total = borders_width + paddings_width;
    for (const float w : natural_widths | std::views::take(col_count)) {
        natural_total += w;
    }
    tl.natural_total_width = natural_total;

    entry.height = total_height;
    tl.last_applied_max_width = max_width;
    // evict 行が残っていても dirty にはしない。dirty のままだと可視中は毎フレーム全行の
    // Finalize と effects 破棄が走る。残りは RestoreEvictedTableRows が可視行だけ埋める。
    entry.layout_dirty = false;
}

TableRestoreResult DWriteTextMeasurer::RestoreEvictedTableRows(const Node& node, NodeLayoutEntry& entry, float max_width, MeasureViewportRange viewport) const
{
    MENDO_PROFILE("RestoreEvictedTableRows");
    TableRestoreResult result;
    auto* tl_ptr = entry.table_layout();
    const auto* tbl = node.table_data();
    if (!dwrite_ || !theme_ || !tl_ptr || !tbl || !tl_ptr->HasEvictedRows()) {
        return result;
    }
    auto& tl = *tl_ptr;
    const size_t row_count = tbl->row_count;
    // evict 済みの行があるならセル単位の配列と row_evicted は揃っている (行追跡済みのときだけ evict する)。
    // 幾何が無効 (InvalidateGeometry 後) か幅が変わっていれば dirty 経由の MeasureTable (Finalize) が担当する。
    if (!tl.HasGeometry(row_count, tl.col_count) || std::abs(tl.last_applied_max_width - max_width) >= CELL_WIDTH_EPSILON) {
        return result;
    }

    const auto [r_begin, r_end] = tl.RowsInViewport(row_count, viewport.top, viewport.bottom);
    const float base_row_height = layout::TableBaseRowHeight(*theme_);
    size_t first_changed_row = row_count;
    for (size_t r = r_begin; r < r_end; r++) {
        if (!tl.row_evicted[r]) {
            continue;
        }
        RestoreRowCells(*tbl, tl, r);
        const float row_height = MeasureRowHeight(tl, *tbl, r, base_row_height).height;
        result.restored = true;
        // 幅変更時に evict 中だった行は旧幅の行高さのまま残っているため、ここで実測に揃える。
        if (row_height != tl.row_heights[r]) {
            tl.row_heights[r] = row_height;
            first_changed_row = std::min(first_changed_row, r);
        }
    }

    result.height_changed = first_changed_row < row_count;
    if (result.height_changed) {
        const float border_width = TABLE_BORDER_WIDTH;
        tl.RebuildRowCumY(row_count, first_changed_row);
        entry.height = border_width + tl.row_cum_y[row_count];
        entry.cached_height = entry.height;
    }
    return result;
}

void DWriteTextMeasurer::MeasureTable(const Node& node, NodeLayoutEntry& entry, float max_width,
                                      MeasureViewportRange viewport) const
{
    MENDO_PROFILE("MeasureTable");
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0) {
        entry.height = 0;
        entry.layout_dirty = false;
        return;
    }

    const size_t row_count = tbl->row_count;
    // パーサが NodeTableData::col_count に最大列数を保持済みなので全行走査は不要。
    const size_t col_count = tbl->col_count;
    if (col_count == 0) {
        entry.layout_dirty = false;
        return;
    }

    // 既存レイアウトの互換性判定。row*col の積だけだと (旧6×4) と (新8×3) のように積が一致するだけで
    // ストライド (col_count) が違うケースを取りこぼすため、col_count も明示的に比較する。
    const auto* tl_existing = entry.table_layout();
    const bool has_compatible_layouts = tl_existing && tl_existing->col_count == col_count && !tl_existing->cell_layouts.empty() && tl_existing->cell_layouts.size() == row_count * col_count;

    // 超高速パス: 前回と max_width がほぼ一致しキャッシュ済みレイアウトが揃っていれば、
    // セル幅・行高さ・累積位置・行オフセットすべて変化しないため、layout_dirty を倒すだけで終える。
    // 検索ハイライト矩形・effects・inline_code_bgs もテキスト位置に依存するので保持できる。
    // evict 済み行のセルは RestoreEvictedTableRows が可視になった時点で再生成する。
    // 幾何が無効 (EstimateInvisibleNodeHeight が InvalidateGeometry 済み) のまま通すと
    // GenTable が空テーブルを描画し続ける。
    if (has_compatible_layouts) {
        const bool same_width = tl_existing->last_applied_max_width >= 0.0f &&
                                std::abs(tl_existing->last_applied_max_width - max_width) < CELL_WIDTH_EPSILON;
        if (same_width && tl_existing->HasGeometry(row_count, col_count)) {
            entry.layout_dirty = false;
            return;
        }
    }

    // セル layout の再作成 or SetMaxWidth で metrics が変わるため、ハイライト矩形の
    // キャッシュは捨てる。選択ハイライトキャッシュは本文ノード専用だが、ノード型変更等で
    // 残っている可能性に備えて落としておく。
    entry.invalidate_per_frame_hl_caches();

    // 倒すと ApplyTableEffects が inline code 背景と行フラグを作り直す。
    entry.set_effects_applied(false);
    auto& tl = entry.ensure_table_layout();
    tl.row_heights.resize(row_count);

    // セルレイアウトが既に存在し、かつストライドが現在の列数と一致する場合のみ
    // 第1パス（テキストレイアウト作成）をスキップして列幅再計算だけ行う。
    // natural_col_widths は cell_layouts と同時にしか破棄されない (ResetTableCellCaches) ため、
    // 互換レイアウトがあればキャッシュ済み自然幅をそのまま使える。
    if (has_compatible_layouts) {
        RestoreNullCellLayouts(*tbl, tl, viewport);
    }
    else {
        tl.AllocateCells(row_count, col_count);
        // 初回構築は常に全行を作る (列幅判定に全行の自然幅が必要なため)。
        MeasureTableCells(*tbl, tl);
    }
    FinalizeTableLayout(*tbl, entry, max_width);
}
