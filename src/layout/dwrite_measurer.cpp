#include "dwrite_measurer.h"
#include "layout.h"
#include "parser.h"
#include "profiler.h"
#include "syntax.h"
#include "ui_constants.h"
#include <algorithm>
#include <ranges>

using Microsoft::WRL::ComPtr;

static constexpr float CODE_BLOCK_NO_WRAP_WIDTH = 10000.0f;
static constexpr float LAYOUT_MAX_HEIGHT = 100000.0f;
static constexpr float DEFAULT_COLUMN_WIDTH = 60.0f;
static constexpr float MIN_DIAGRAM_PLACEHOLDER_HEIGHT = 60.0f;
// セル幅がこれ以下の差分なら前回の計測高さを再利用する
static constexpr float CELL_WIDTH_EPSILON = 0.5f;

// 1 行目の高さを取得し entry にキャッシュする。layout 自体は変えない。
static void CacheFirstLineHeight(IDWriteTextLayout* layout, NodeLayoutEntry& entry) noexcept
{
    DWRITE_LINE_METRICS lm{};
    UINT32 lc = 0;
    entry.first_line_height = (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &lc)) && lc > 0) ? lm.height : 0.0f;
}

static HRESULT CreateFormat(IDWriteFactory* factory, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out)
{
    return factory->CreateTextFormat(
        family, nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"ja-jp", out);
}

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

IDWriteTextFormat* DWriteTextMeasurer::GetTextFormat(const Node& node) noexcept
{
    if (node.type == NodeType::CodeBlock) {
        return fmt_code_.Get();
    }
    if (node.type == NodeType::Heading) {
        if (node.heading_level >= 1 && node.heading_level <= 6) {
            return fmt_h_[node.heading_level - 1].Get();
        }
    }
    return fmt_body_.Get();
}

namespace {

// 5 属性を 1 パスでまとめてマージするためのレンジビルダ。
// 隣接ランで属性が連続する間はマージし、切れたら emit する。
struct AttrRangeBuilder {
    uint32_t start = 0;
    uint32_t length = 0;
    bool active = false;
};

template <typename Emit>
inline void UpdateAttr(AttrRangeBuilder& b, bool active_now, const TextRun& run, Emit&& emit) noexcept
{
    if (active_now) {
        if (b.active && run.start == b.start + b.length) {
            b.length += run.length;
        }
        else {
            if (b.active) {
                emit(DWRITE_TEXT_RANGE{ b.start, b.length });
            }
            b.start = run.start;
            b.length = run.length;
            b.active = true;
        }
    }
    else if (b.active) {
        emit(DWRITE_TEXT_RANGE{ b.start, b.length });
        b.active = false;
    }
}

template <typename Emit>
inline void FlushAttr(AttrRangeBuilder& b, Emit&& emit) noexcept
{
    if (b.active) {
        emit(DWRITE_TEXT_RANGE{ b.start, b.length });
        b.active = false;
    }
}

} // namespace

void DWriteTextMeasurer::ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, std::optional<NodeType> node_type)
{
    if (runs.empty()) {
        return;
    }
    MENDO_PROFILE("ApplyRunFormatting");
    const bool apply_code = (!node_type || *node_type != NodeType::CodeBlock);
    const bool apply_code_size = apply_code && (!node_type || *node_type != NodeType::Heading);
    const bool apply_link = !node_type;

    AttrRangeBuilder bold_b, italic_b, code_b, strike_b, link_b;

    const auto emit_bold = [&](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_EXTRA_BOLD, r);
    };
    const auto emit_italic = [&](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
    };
    const auto emit_code = [&](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetFontFamilyName(theme_->monospace_font.c_str(), r);
        if (apply_code_size) {
            layout->SetFontSize(theme_->font_size_code, r);
        }
    };
    const auto emit_strike = [&](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetStrikethrough(TRUE, r);
    };
    const auto emit_link = [&](DWRITE_TEXT_RANGE r) noexcept {
        layout->SetUnderline(TRUE, r);
    };

    for (const auto& r : runs) {
        UpdateAttr(bold_b, r.bold(), r, emit_bold);
        UpdateAttr(italic_b, r.italic(), r, emit_italic);
        if (apply_code) {
            UpdateAttr(code_b, r.code(), r, emit_code);
        }
        UpdateAttr(strike_b, r.strikethrough(), r, emit_strike);
        if (apply_link) {
            UpdateAttr(link_b, r.has_link(), r, emit_link);
        }
    }

    FlushAttr(bold_b, emit_bold);
    FlushAttr(italic_b, emit_italic);
    if (apply_code) {
        FlushAttr(code_b, emit_code);
    }
    FlushAttr(strike_b, emit_strike);
    if (apply_link) {
        FlushAttr(link_b, emit_link);
    }
}

void DWriteTextMeasurer::MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width)
{
    MENDO_PROFILE("MeasureNode");
    if (!dwrite_ || !theme_) {
        return;
    }

    if (node.type == NodeType::HorizontalRule) {
        entry.height = theme_->paragraph_spacing + theme_->hr_thickness;
        entry.layout_dirty = false;
        return;
    }

    if (node.type == NodeType::Table) {
        MeasureTable(node, entry, max_width);
        return;
    }

    // ダイアグラム系コードブロック: ビットマップがレンダリングされるまでのプレースホルダー高さ
    if (node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language)) {
        if (entry.height <= 0) {
            entry.height = std::max(MIN_DIAGRAM_PLACEHOLDER_HEIGHT, theme_->font_size_body * 3.0f);
        }
        entry.layout_dirty = false;
        return;
    }

    // 画像ノード: 元画像サイズが設定済みならコンテンツ幅に合わせてスケール、
    // 未設定ならプレースホルダー高さ
    if (node.type == NodeType::Image) {
        if (const auto* img = node.image_data(); img && img->width > 0 && img->height > 0) {
            const float w = img->width;
            float h = img->height;
            if (w > max_width) {
                h *= max_width / w;
            }
            entry.height = h;
        }
        else if (entry.height <= 0) {
            entry.height = std::max(MIN_DIAGRAM_PLACEHOLDER_HEIGHT, theme_->font_size_body * 3.0f);
        }
        entry.layout_dirty = false;
        return;
    }

    const auto& text = node.GetText();
    if (text.empty()) {
        entry.height = theme_->paragraph_spacing;
        entry.layout_dirty = false;
        return;
    }

    IDWriteTextFormat* const fmt = GetTextFormat(node);
    float layout_width = max_width;
    if (node.type == NodeType::CodeBlock) {
        layout_width = CODE_BLOCK_NO_WRAP_WIDTH;
    }

    // line_count ベースで上限高さを動的に決める。100k DIP 固定だと
    // 数万行のコードブロックで切り詰められうるため、フォントサイズ × 行数
    // × 安全係数で十分な余地を確保する。コードブロックは font_size_code 基準で計算しないと
    // body フォントが小さい設定で安全マージンが不足するため type で切り替える。
    const float height_basis = (node.type == NodeType::CodeBlock) ? theme_->font_size_code : theme_->font_size_body;
    const float dynamic_max_height = std::max(
        LAYOUT_MAX_HEIGHT,
        height_basis * static_cast<float>(std::max(1, node.line_count + 1)) * 3.0f);

    // 高速パス: 既存の text_layout が残っていれば SetMaxWidth で再計測する。
    // text_layout は内容変更時に呼び出し側 (LayoutCache::InvalidateAllLayouts /
    // MarkAllDirty / EvictTextLayouts) で必ず Reset される契約のため、現存している
    // 場合はテキスト/runs/フォント幾何が一致している。CreateTextLayout は内部で
    // BiDi 解析と shaping を走らせるためリサイズ時の最大コスト要因で、SetMaxWidth は
    // ラインブレーク再計算のみで済むので大幅に軽い。
    if (entry.text_layout) {
        MENDO_PROFILE("MeasureNode.fastpath");
        HRESULT hr = entry.text_layout->SetMaxWidth(layout_width);
        if (SUCCEEDED(hr)) {
            hr = entry.text_layout->SetMaxHeight(dynamic_max_height);
        }
        if (SUCCEEDED(hr)) {
            DWRITE_TEXT_METRICS metrics{};
            entry.text_layout->GetMetrics(&metrics);
            entry.height = metrics.height;
            entry.layout_dirty = false;
            CacheFirstLineHeight(entry.text_layout.Get(), entry);
            // 折り返し行が変わるためエフェクト位置 / ハイライト矩形は無効化する。
            // text_layout 自体は破棄しない (フォーマット属性は保持される)。
            entry.effects_applied = false;
            entry.clear_inline_code_bgs();
            entry.invalidate_per_frame_hl_caches();
            return;
        }
        // 失敗時はスローパスでフルに作り直す。
        entry.text_layout.Reset();
        entry.first_line_height = 0.0f;
    }

    ComPtr<IDWriteTextLayout> layout;
    const HRESULT hr = [&] {
        MENDO_PROFILE("CreateTextLayout");
        return dwrite_->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            fmt,
            layout_width,
            dynamic_max_height,
            &layout);
    }();
    if (FAILED(hr)) {
        return;
    }

    ApplyRunFormatting(layout.Get(), node.runs, node.type);

    // Alert ノードのアイコン文字のフォントウェイトを設定
    if (node.type == NodeType::BlockQuote && node.alert_type != AlertType::None && node.alert_label_length > 0) {
        const UINT32 icon_len = static_cast<UINT32>(GetAlertIcon(node.alert_type).size());
        const DWRITE_TEXT_RANGE icon_range{ 0, icon_len };
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, icon_range);
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    // コードブロックのシンタックストークン化をレイアウトパスで事前実行する。
    // 描画パス（ApplyNodeEffects）での遅延トークン化を排除し、フレーム落ちを防止する。
    if (node.type == NodeType::CodeBlock && node.syntax_tokens().empty() &&
        node.code_language != SyntaxLanguage::None &&
        !IsDiagramLanguage(node.code_language)) {
        MENDO_PROFILE("Tokenize");
        node.syntax_tokens_mut() = Tokenize(text, node.code_language);
    }

    CacheFirstLineHeight(layout.Get(), entry);

    entry.text_layout = std::move(layout);
    entry.height = metrics.height;
    entry.layout_dirty = false;
    entry.effects_applied = false;
    entry.clear_inline_code_bgs();
    entry.invalidate_per_frame_hl_caches();
}

void DWriteTextMeasurer::MeasureTableCells(Node& node, NodeLayoutEntry& entry, std::pmr::vector<float>& natural_widths)
{
    MENDO_PROFILE("MeasureTableCells");
    IDWriteTextFormat* const fmt = fmt_body_.Get();
    IDWriteTextFormat* const fmt_bold = fmt_h_[3].Get();
    auto& rows = node.table_rows();
    const auto row_count = rows.size();
    auto& tl = *entry.table_layout;

    for (size_t r = 0; r < row_count; r++) {
        auto& row = rows[r];
        const auto col_count = row.cells.size();
        for (size_t c = 0; c < col_count; c++) {
            auto& cell = row.cells[c];
            if (cell.text.empty()) {
                continue;
            }

            IDWriteTextFormat* const cell_fmt = cell.is_header ? fmt_bold : fmt;
            const size_t ci = tl.CellIndex(r, c);
            {
                MENDO_PROFILE("CreateTextLayout.cell");
                dwrite_->CreateTextLayout(
                    cell.text.c_str(), static_cast<UINT32>(cell.text.size()),
                    cell_fmt, CODE_BLOCK_NO_WRAP_WIDTH, LAYOUT_MAX_HEIGHT,
                    &tl.cell_layouts[ci]);
            }

            if (tl.cell_layouts[ci]) {
                ApplyRunFormatting(tl.cell_layouts[ci].Get(), cell.runs, std::nullopt);
                DWRITE_TEXT_METRICS metrics{};
                tl.cell_layouts[ci]->GetMetrics(&metrics);
                natural_widths[c] = std::max(natural_widths[c], metrics.width);
            }
        }
    }
}

void DWriteTextMeasurer::RestoreNullCellLayouts(Node& node, NodeLayoutEntry& entry)
{
    // EvictInvisibleTableRows で Reset された null セルを再生成する。
    MENDO_PROFILE("RestoreNullCellLayouts");
    IDWriteTextFormat* const fmt = fmt_body_.Get();
    IDWriteTextFormat* const fmt_bold = fmt_h_[3].Get();
    auto& rows = node.table_rows();
    auto& tl = *entry.table_layout;
    const auto row_count = rows.size();

    for (size_t r = 0; r < row_count; r++) {
        auto& row = rows[r];
        const auto col_count = row.cells.size();
        for (size_t c = 0; c < col_count; c++) {
            const size_t ci = tl.CellIndex(r, c);
            if (ci >= tl.cell_layouts.size() || tl.cell_layouts[ci]) {
                continue;
            }
            auto& cell = row.cells[c];
            if (cell.text.empty()) {
                continue;
            }
            IDWriteTextFormat* const cell_fmt = cell.is_header ? fmt_bold : fmt;
            {
                MENDO_PROFILE("CreateTextLayout.cell.restore");
                dwrite_->CreateTextLayout(
                    cell.text.c_str(), static_cast<UINT32>(cell.text.size()),
                    cell_fmt, CODE_BLOCK_NO_WRAP_WIDTH, LAYOUT_MAX_HEIGHT,
                    &tl.cell_layouts[ci]);
            }
            if (tl.cell_layouts[ci]) {
                ApplyRunFormatting(tl.cell_layouts[ci].Get(), cell.runs, std::nullopt);
            }
        }
    }
}

void DWriteTextMeasurer::FinalizeTableLayout(Node& node, NodeLayoutEntry& entry, float max_width,
                                             size_t col_count, std::pmr::vector<float>& natural_widths)
{
    MENDO_PROFILE("FinalizeTableLayout");
    const float cell_padding = TABLE_CELL_PADDING;
    const float border_width = TABLE_BORDER_WIDTH;
    auto& tl = *entry.table_layout;

    const float available = max_width - (static_cast<float>(col_count) + 1.0f) * border_width - static_cast<float>(col_count) * cell_padding * 2.0f;
    ComputeColumnWidths(tl.col_widths, natural_widths, available, col_count);

    // 適用幅/高さキャッシュ。幅不変なら GetMetrics を省ける。
    const size_t cell_total = tl.cell_layouts.size();
    if (tl.cell_heights.size() != cell_total) {
        tl.cell_heights.assign(cell_total, 0.0f);
    }
    if (tl.cell_applied_widths.size() != cell_total) {
        tl.cell_applied_widths.assign(cell_total, -1.0f);
    }

    float total_height = border_width;
    auto& rows = node.table_rows();
    const auto row_count = rows.size();
    for (size_t r = 0; r < row_count; r++) {
        auto& row = rows[r];
        float row_height = theme_->font_size_body * 1.4f;
        const auto cell_count = row.cells.size();
        for (size_t c = 0; c < cell_count; c++) {
            auto& cell = row.cells[c];
            const float cw = (c < tl.col_widths.size()) ? tl.col_widths[c] : DEFAULT_COLUMN_WIDTH;

            const size_t ci = tl.CellIndex(r, c);
            if (tl.cell_layouts[ci]) {
                const bool width_unchanged = std::abs(tl.cell_applied_widths[ci] - cw) < CELL_WIDTH_EPSILON && tl.cell_heights[ci] > 0.0f;
                if (!width_unchanged) {
                    tl.cell_layouts[ci]->SetMaxWidth(cw);
                    if (cell.align == TableAlign::Center) {
                        tl.cell_layouts[ci]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    }
                    else if (cell.align == TableAlign::Right) {
                        tl.cell_layouts[ci]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    }
                    DWRITE_TEXT_METRICS metrics{};
                    tl.cell_layouts[ci]->GetMetrics(&metrics);
                    tl.cell_heights[ci] = metrics.height;
                    tl.cell_applied_widths[ci] = cw;
                }
                row_height = std::max(row_height, tl.cell_heights[ci] + cell_padding * 2.0f);
            }
        }
        tl.row_heights[r] = row_height;
        total_height += row_height + border_width;
    }

    if (!node.HasText()) {
        // line_count は Table 描画では未参照なので 0 で改行走査を回避。
        node.SetTextWithLineCount(BuildLinearizedTableText(node.table_rows()), 0);
    }

    // ヒットテスト高速化用に行Y累積と列X累積を事前計算
    tl.row_cum_y.resize(row_count + 1);
    {
        float ry = 0.0f;
        for (size_t r = 0; r < row_count; r++) {
            tl.row_cum_y[r] = ry;
            ry += tl.row_heights[r] + border_width;
        }
        tl.row_cum_y[row_count] = ry;
    }
    tl.col_cum_x.resize(col_count + 1);
    {
        float cx = border_width;
        for (size_t c = 0; c < col_count; c++) {
            tl.col_cum_x[c] = cx;
            cx += tl.col_widths[c] + cell_padding * 2.0f + border_width;
        }
        tl.col_cum_x[col_count] = cx;
    }

    // ヒットテスト高速化用に行ごとのフラットテキストオフセットをプリコンピュート
    tl.row_flat_offsets.resize(row_count);
    uint32_t flat_offset = 0;
    for (size_t r = 0; r < row_count; r++) {
        tl.row_flat_offsets[r] = flat_offset;
        TableLayoutData::AdvanceFlatOffsetInRow(rows[r], 0, rows[r].cells.size(), flat_offset);
        if (r + 1 < row_count) {
            flat_offset++; // 改行区切り
        }
    }

    // col_cum_x の末尾は border_width + Σ(col_w + 2*pad + border) と一致するため再計算しない。
    tl.cached_table_width = tl.col_cum_x.back();

    entry.height = total_height;
    entry.layout_dirty = false;
    tl.last_applied_max_width = max_width;
}

void DWriteTextMeasurer::MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width)
{
    MENDO_PROFILE("MeasureTable");
    if (!dwrite_ || !theme_) {
        return;
    }

    if (node.table_rows().empty()) {
        entry.height = 0;
        entry.layout_dirty = false;
        return;
    }

    auto& rows = node.table_rows();
    const auto row_count = rows.size();
    // パーサが NodeTableData::col_count に最大列数を保持済みなので全行走査は不要。
    const size_t col_count = node.table_data()->col_count;
    if (col_count == 0) {
        entry.layout_dirty = false;
        return;
    }

    // 既存レイアウトの互換性判定。row*col の積だけだと (旧6×4) と (新8×3) のように積が一致するだけで
    // ストライド (col_count) が違うケースを取りこぼすため、col_count も明示的に比較する。
    auto* tl_existing = entry.table_layout.get();
    const bool has_compatible_layouts =
        tl_existing && tl_existing->col_count == col_count && !tl_existing->cell_layouts.empty() && tl_existing->cell_layouts.size() == row_count * col_count;

    // 超高速パス: 前回と max_width がほぼ一致しキャッシュ済みレイアウトが揃っていれば、
    // セル幅・行高さ・累積位置・行オフセットすべて変化しないため、layout_dirty を倒すだけで終える。
    // 検索ハイライト矩形・effects・inline_code_bgs もテキスト位置に依存するので保持できる。
    if (has_compatible_layouts && tl_existing->last_applied_max_width >= 0.0f && std::abs(tl_existing->last_applied_max_width - max_width) < CELL_WIDTH_EPSILON) {
        entry.layout_dirty = false;
        return;
    }

    // セル layout の再作成 or SetMaxWidth で metrics が変わるため、ハイライト矩形の
    // キャッシュは捨てる。選択ハイライトキャッシュは本文ノード専用だが、ノード型変更等で
    // 残っている可能性に備えて落としておく。
    entry.invalidate_per_frame_hl_caches();

    entry.effects_applied = false;
    auto& tl = entry.ensure_table_layout();
    tl.cell_inline_code_bgs.clear();
    tl.row_heights.resize(row_count);

    // セルレイアウトが既に存在し、かつストライドが現在の列数と一致する場合のみ
    // 第1パス（テキストレイアウト作成）をスキップして列幅再計算だけ行う。
    const bool has_existing_layouts = has_compatible_layouts;
    if (has_existing_layouts) {
        RestoreNullCellLayouts(node, entry);
        if (tl.natural_col_widths.size() == col_count) {
            // キャッシュ済み自然幅を使用し、DirectWrite呼び出しを回避
            FinalizeTableLayout(node, entry, max_width, col_count, tl.natural_col_widths);
        }
        else {
            // 既存レイアウトから自然幅を再取得
            std::pmr::vector<float> natural_widths(col_count, 0.0f);
            for (size_t r = 0; r < row_count; r++) {
                const auto cell_count = rows[r].cells.size();
                for (size_t c = 0; c < cell_count && c < col_count; c++) {
                    const size_t ci = tl.CellIndex(r, c);
                    if (tl.cell_layouts[ci]) {
                        tl.cell_layouts[ci]->SetMaxWidth(CODE_BLOCK_NO_WRAP_WIDTH);
                        DWRITE_TEXT_METRICS metrics{};
                        tl.cell_layouts[ci]->GetMetrics(&metrics);
                        natural_widths[c] = std::max(natural_widths[c], metrics.width);
                    }
                }
            }
            tl.natural_col_widths = natural_widths;
            FinalizeTableLayout(node, entry, max_width, col_count, natural_widths);
        }
    }
    else {
        tl.col_count = col_count;
        tl.cell_layouts.assign(row_count * col_count, {});

        // 第1パス: テキストレイアウトを作成し、自然な幅を計測
        std::pmr::vector<float> natural_widths(col_count, 0.0f);
        MeasureTableCells(node, entry, natural_widths);

        // リサイズ高速パス用に自然幅をキャッシュ
        tl.natural_col_widths = std::move(natural_widths);

        // 第2パス: 列幅を設定し、行の高さを計測
        FinalizeTableLayout(node, entry, max_width, col_count, tl.natural_col_widths);
    }
}
