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

void RebuildRowCumY(TableLayoutData& tl, size_t row_count, float border_width)
{
    tl.row_cum_y.resize(row_count + 1);
    float ry = 0.0f;
    for (size_t r = 0; r < row_count; r++) {
        tl.row_cum_y[r] = ry;
        ry += tl.row_heights[r] + border_width;
    }
    tl.row_cum_y[row_count] = ry;
}

void RebuildColCumX(TableLayoutData& tl, size_t col_count, float border_width)
{
    tl.col_cum_x.resize(col_count + 1);
    float cx = border_width;
    for (size_t c = 0; c < col_count; c++) {
        tl.col_cum_x[c] = cx;
        cx += tl.col_widths[c] + TABLE_CELL_PADDING * 2.0f + border_width;
    }
    tl.col_cum_x[col_count] = cx;
}

// 1 行目の高さを取得し entry にキャッシュする。layout 自体は変えない。
void CacheFirstLineHeight(IDWriteTextLayout* layout, NodeLayoutEntry& entry) noexcept
{
    DWRITE_LINE_METRICS lm{};
    UINT32 lc = 0;
    // 複数行レイアウトでは E_NOT_SUFFICIENT_BUFFER が返るが、先頭 1 行分は lm に書き込まれる。
    const HRESULT hr = layout->GetLineMetrics(&lm, 1, &lc);
    entry.first_line_height = ((SUCCEEDED(hr) || hr == E_NOT_SUFFICIENT_BUFFER) && lc > 0) ? lm.height : 0.0f;
}

// entry.text_layout の計測結果を確定する。折り返し行が変わるためハイライト矩形は無効化する。
// reused_layout (SetMaxWidth のみ) では SetDrawingEffect/SetUnderline の範囲が残るので、
// 折り返しに依存するインラインコード背景だけを作り直させる。
void FinishTextMeasure(const Node& node, NodeLayoutEntry& entry, const DWRITE_TEXT_METRICS& metrics, bool reused_layout) noexcept
{
    entry.height = metrics.height;
    entry.layout_dirty = false;
    CacheFirstLineHeight(entry.text_layout.Get(), entry);
    entry.invalidate_per_frame_hl_caches();
    if (node.type == NodeType::CodeBlock) {
        entry.natural_code_width = metrics.widthIncludingTrailingWhitespace;
        // NO_WRAP なので再利用時は行も変わらず、インラインコード背景も持たない。
        if (reused_layout) {
            return;
        }
    }
    entry.clear_inline_code_bgs();
    if (reused_layout) {
        entry.inline_code_bgs_stale = true;
    }
    else {
        entry.effects_applied = false;
    }
}

// テキストレイアウトを持たないノード (HR / ダイアグラム / 画像 / 空テキスト) の高さを確定する。
// 戻り値: 処理した場合 true。
bool MeasureWithoutTextLayout(const Node& node, NodeLayoutEntry& entry, float max_width, const Theme& theme) noexcept
{
    if (node.type == NodeType::HorizontalRule) {
        entry.height = theme.paragraph_spacing + theme.hr_thickness;
    }
    // ダイアグラム: ビットマップがレンダリングされるまではプレースホルダー高さ
    else if (IsDiagramCodeBlock(node)) {
        if (entry.height <= 0) {
            entry.height = mendo::layout::PlaceholderHeight(theme);
        }
    }
    else if (node.type == NodeType::Image) {
        if (const auto* img = node.image_data(); img && img->width > 0 && img->height > 0) {
            entry.height = mendo::layout::ImageDisplayHeight(img->width, img->height, max_width);
        }
        else if (entry.height <= 0) {
            entry.height = mendo::layout::PlaceholderHeight(theme);
        }
    }
    else if (node.GetText().empty()) {
        // loose LI で paragraph_spacing を入れると bullet と直下 P の文字 Y が分離する (issue#237)。
        entry.height = IsEmptyListItemContainer(node) ? 0.0f : theme.paragraph_spacing;
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
    HRESULT hr = entry.text_layout->SetMaxWidth(layout_width);
    if (SUCCEEDED(hr)) {
        hr = entry.text_layout->SetMaxHeight(LAYOUT_INFINITY);
    }
    if (SUCCEEDED(hr)) {
        DWRITE_TEXT_METRICS metrics{};
        entry.text_layout->GetMetrics(&metrics);
        FinishTextMeasure(node, entry, metrics, true);
        return true;
    }
    entry.text_layout.Reset();
    entry.first_line_height = 0.0f;
    entry.natural_code_width = 0.0f;
    return false;
}

// 描画パス (ApplyNodeEffects) での遅延トークン化によるフレーム落ちを避けるため、レイアウトパスで行う。
void TokenizeCodeBlock(Node& node, std::string_view text, std::pmr::vector<SyntaxToken>* tokens_out)
{
    const auto lang = node.code_language();
    if (lang == SyntaxLanguage::None || !node.syntax_tokens().empty()) {
        return;
    }
    if (tokens_out != nullptr) {
        *tokens_out = Tokenize(text, lang);
    }
    else {
        node.syntax_tokens_mut() = Tokenize(text, lang);
    }
}

HRESULT CreateFormat(IDWriteFactory* factory, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out)
{
    return factory->CreateTextFormat(
        family, nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"ja-jp", out);
}

// 1 属性ぶんのレンジビルダ。隣接ランで属性が連続する間はマージし、切れたら emit する。
// start/length は UTF-8 byte 単位で、emit 時に UTF-16 textPosition に変換する。
template <typename Emit>
class AttrRangeBuilder {
public:
    AttrRangeBuilder(const mendo::WideViewForDWrite& wv, Emit emit) noexcept
        : wv_(wv), emit_(emit)
    {
    }

    void Update(bool active_now, const TextRun& run) noexcept
    {
        if (!active_now) {
            Flush();
            return;
        }
        if (active_ && run.start == start_ + length_) {
            length_ += run.length;
            return;
        }
        Flush();
        start_ = run.start;
        length_ = run.length;
        active_ = true;
    }

    void Flush() noexcept
    {
        if (active_) {
            emit_(wv_.WideRange(start_, length_));
            active_ = false;
        }
    }

private:
    const mendo::WideViewForDWrite& wv_;
    Emit emit_;
    uint32_t start_ = 0;
    uint32_t length_ = 0;
    bool active_ = false;
};

} // namespace

bool DWriteTextMeasurer::CreateAllFormats()
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

bool DWriteTextMeasurer::Init(const Theme& theme)
{
    theme_ = &theme;
    return CreateAllFormats();
}

bool DWriteTextMeasurer::RecreateFormats()
{
    return CreateAllFormats();
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

void DWriteTextMeasurer::ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, const mendo::WideViewForDWrite& wv, RunFormatScope scope) const
{
    if (runs.empty()) {
        return;
    }

    AttrRangeBuilder bold{ wv, [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_EXTRA_BOLD, r);
    } };
    AttrRangeBuilder italic{ wv, [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
    } };
    AttrRangeBuilder code{ wv, [this, layout, apply_code_size = scope.apply_code_size](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontFamilyName(theme_->monospace_font.c_str(), r);
        if (apply_code_size) {
            layout->SetFontSize(theme_->font_size_code, r);
        }
    } };
    AttrRangeBuilder strike{ wv, [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetStrikethrough(TRUE, r);
    } };
    AttrRangeBuilder link{ wv, [layout](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetUnderline(TRUE, r);
    } };

    for (const auto& r : runs) {
        bold.Update(r.bold(), r);
        italic.Update(r.italic(), r);
        if (scope.apply_code) {
            code.Update(r.code(), r);
        }
        strike.Update(r.strikethrough(), r);
        if (scope.apply_link) {
            link.Update(r.has_link(), r);
        }
    }

    bold.Flush();
    italic.Flush();
    code.Flush();
    strike.Flush();
    link.Flush();
}

void DWriteTextMeasurer::MeasureNode(
    Node& node, NodeLayoutEntry& entry, float max_width,
    std::pmr::vector<SyntaxToken>* tokens_out,
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
    if (MeasureWithoutTextLayout(node, entry, max_width, *theme_)) {
        return;
    }

    // CodeBlock は fmt_code_ が NO_WRAP なので layout_width は折り返しに使われない。
    const float layout_width = (node.type == NodeType::CodeBlock) ? LAYOUT_INFINITY : max_width;
    if (entry.text_layout && RemeasureExistingLayout(node, entry, layout_width)) {
        return;
    }

    // 1 ノードにつき WideViewForDWrite を 1 回だけ構築し、CreateTextLayout と ApplyRunFormatting で共有する
    // (per-node の二重 UTF-8→UTF-16 decode を回避)。
    const auto& text = node.GetText();
    const mendo::WideViewForDWrite wv{ text };

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(mendo::CreateDocTextLayout(dwrite_, wv, GetTextFormat(node), layout_width, LAYOUT_INFINITY, &layout))) {
        return;
    }

    ApplyRunFormatting(layout.Get(), node.runs, wv, RunFormatScope::ForNode(node.type));

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    if (node.type == NodeType::CodeBlock) {
        TokenizeCodeBlock(node, text, tokens_out);
    }

    entry.text_layout = std::move(layout);
    FinishTextMeasure(node, entry, metrics, false);
}

void DWriteTextMeasurer::BuildCellLayout(const NodeTableData& tbl, size_t r, size_t c, IDWriteTextFormat* row_fmt, TableLayoutData& tl) const
{
    const auto text = tbl.GetCellText(r, c);
    if (text.empty()) {
        return;
    }
    auto& cell_layout = tl.cell_layouts[tl.CellIndex(r, c)];
    const mendo::WideViewForDWrite wv{ text };
    mendo::CreateDocTextLayout(dwrite_, wv, row_fmt, LAYOUT_INFINITY, LAYOUT_INFINITY, &cell_layout);
    if (cell_layout) {
        ApplyRunFormatting(cell_layout.Get(), tbl.GetCellRuns(r, c), wv, RunFormatScope::ForCell());
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
    constexpr size_t kCellsPerChunk = 256;
    const size_t rows_per_chunk = std::max<size_t>(1, kCellsPerChunk / std::max<size_t>(col_count, 1));
    std::mutex merge_mutex;
    ParallelFor(scheduler_, tbl.row_count, rows_per_chunk, [&](size_t row_begin, size_t row_end) {
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
    auto& tl = *entry.table_layout;
    const size_t col_count = tl.col_count;
    const auto& natural_widths = tl.natural_col_widths;
    const float borders_width = (static_cast<float>(col_count) + 1.0f) * border_width;
    const float paddings_width = static_cast<float>(col_count) * cell_padding * 2.0f;

    mendo::layout::ComputeColumnWidths(tl.col_widths, natural_widths, max_width - borders_width - paddings_width, col_count);

    // 適用幅/高さキャッシュ。幅不変なら GetMetrics を省ける。
    const size_t cell_total = tl.cell_layouts.size();
    if (tl.cell_heights.size() != cell_total) {
        tl.cell_heights.assign(cell_total, 0.0f);
    }
    if (tl.cell_applied_widths.size() != cell_total) {
        tl.cell_applied_widths.assign(cell_total, -1.0f);
    }

    float total_height = border_width;
    const float base_row_height = theme_->font_size_body * TABLE_ROW_HEIGHT_FACTOR;
    for (size_t r = 0; r < tbl.row_count; r++) {
        const auto [row_height, has_unbuilt_cell] = MeasureRowHeight(tl, tbl, r, base_row_height);
        // 部分復元時は null セルがある行 = 範囲外 evict 行。再計算で行高さを既定値に戻すと
        // 累積位置がずれるため、既存の row_heights[r] を保持する (復元時に実測で補正される)。
        if (has_unbuilt_cell && r < tl.row_heights.size() && tl.row_heights[r] > 0.0f) {
            total_height += tl.row_heights[r] + border_width;
        }
        else {
            tl.row_heights[r] = row_height;
            total_height += row_height + border_width;
        }
    }

    // ヒットテスト高速化用に行Y累積と列X累積を事前計算
    RebuildRowCumY(tl, tbl.row_count, border_width);
    RebuildColCumX(tl, col_count, border_width);

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

TableRestoreResult DWriteTextMeasurer::RestoreEvictedTableRows(Node& node, NodeLayoutEntry& entry, float max_width, MeasureViewportRange viewport) const
{
    MENDO_PROFILE("RestoreEvictedTableRows");
    TableRestoreResult result;
    auto* tl_ptr = entry.table_layout.get();
    const auto* tbl = node.table_data();
    if (!dwrite_ || !theme_ || !tl_ptr || !tbl || !tl_ptr->HasEvictedRows()) {
        return result;
    }
    auto& tl = *tl_ptr;
    const size_t row_count = tbl->row_count;
    const size_t col_count = tl.col_count;
    const bool geometry_ok = tl.row_evicted.size() == row_count && tl.row_cum_y.size() == row_count + 1 &&
                             tl.row_heights.size() == row_count && tl.col_widths.size() == col_count &&
                             tl.cell_layouts.size() == row_count * col_count &&
                             tl.cell_heights.size() == tl.cell_layouts.size() &&
                             tl.cell_applied_widths.size() == tl.cell_layouts.size();
    // 幅が変わっていれば dirty 経由の MeasureTable (Finalize) が担当する。
    if (!geometry_ok || std::abs(tl.last_applied_max_width - max_width) >= CELL_WIDTH_EPSILON) {
        return result;
    }

    const auto [r_begin, r_end] = tl.RowsInViewport(row_count, viewport.top, viewport.bottom);
    const float base_row_height = theme_->font_size_body * TABLE_ROW_HEIGHT_FACTOR;
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
            result.height_changed = true;
        }
    }

    if (result.height_changed) {
        const float border_width = TABLE_BORDER_WIDTH;
        RebuildRowCumY(tl, row_count, border_width);
        entry.height = border_width + tl.row_cum_y[row_count];
        entry.cached_height = entry.height;
    }
    return result;
}

void DWriteTextMeasurer::MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width,
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
    const auto* tl_existing = entry.table_layout.get();
    const bool has_compatible_layouts = tl_existing && tl_existing->col_count == col_count && !tl_existing->cell_layouts.empty() && tl_existing->cell_layouts.size() == row_count * col_count;

    // 超高速パス: 前回と max_width がほぼ一致しキャッシュ済みレイアウトが揃っていれば、
    // セル幅・行高さ・累積位置・行オフセットすべて変化しないため、layout_dirty を倒すだけで終える。
    // 検索ハイライト矩形・effects・inline_code_bgs もテキスト位置に依存するので保持できる。
    // evict 済み行のセルは RestoreEvictedTableRows が可視になった時点で再生成する。
    // col_widths が空 (EstimateInvisibleNodeHeight が幾何を破棄済み) のまま通すと
    // GenTable が空テーブルを描画し続ける。
    if (has_compatible_layouts) {
        const bool same_width = tl_existing->last_applied_max_width >= 0.0f &&
                                std::abs(tl_existing->last_applied_max_width - max_width) < CELL_WIDTH_EPSILON;
        if (same_width && tl_existing->row_evicted.size() == row_count && !tl_existing->col_widths.empty()) {
            entry.layout_dirty = false;
            return;
        }
    }

    // セル layout の再作成 or SetMaxWidth で metrics が変わるため、ハイライト矩形の
    // キャッシュは捨てる。選択ハイライトキャッシュは本文ノード専用だが、ノード型変更等で
    // 残っている可能性に備えて落としておく。
    entry.invalidate_per_frame_hl_caches();

    entry.effects_applied = false;
    auto& tl = entry.ensure_table_layout();
    tl.cell_inline_code_bgs.clear();
    // 計算済みフラグを残すと、幅変更時に画面外だった行の inline code 背景が
    // 再計算されずに欠落する (ApplyTableEffects の resize は既存要素を保持するため)。
    tl.row_bgs_computed.clear();
    tl.row_heights.resize(row_count);

    // セルレイアウトが既に存在し、かつストライドが現在の列数と一致する場合のみ
    // 第1パス（テキストレイアウト作成）をスキップして列幅再計算だけ行う。
    // natural_col_widths は cell_layouts と同時にしか破棄されない (ResetTableLayoutGeometry) ため、
    // 互換レイアウトがあればキャッシュ済み自然幅をそのまま使える。
    if (has_compatible_layouts) {
        if (tl.row_evicted.size() != row_count) {
            tl.ResetRowEviction(row_count);
        }
        RestoreNullCellLayouts(*tbl, tl, viewport);
    }
    else {
        tl.col_count = col_count;
        tl.cell_layouts.assign(row_count * col_count, {});
        // 初回構築は常に全行を作る (列幅判定に全行の自然幅が必要なため)。
        MeasureTableCells(*tbl, tl);
        tl.ResetRowEviction(row_count);
    }
    FinalizeTableLayout(*tbl, entry, max_width);
}
