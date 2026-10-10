#include "selection_html.h"
#include "nav.h"
#include "profiler.h"
#include "small_vector.h"
#include "syntax.h"
#include "theme_palette.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>

namespace {

// 範囲外の node index を飛ばしながら選択範囲内のノードを走査する。
template <class Fn>
void ForEachSelectedNode(const std::pmr::vector<Node>& nodes, const TextSelection& selection, Fn&& fn)
{
    const int first = std::max(selection.start_node, 0);
    const int last = std::min(selection.end_node, static_cast<int>(nodes.size()) - 1);
    for (int i = first; i <= last; ++i) {
        fn(i, nodes[i]);
    }
}

constexpr const theme_palette::SharedColors& PaletteFor(bool dark_mode) noexcept
{
    return dark_mode ? theme_palette::kDark : theme_palette::kLight;
}

constexpr uint32_t ClampEndToText(uint32_t end, size_t text_size) noexcept
{
    return end > text_size ? static_cast<uint32_t>(text_size) : end;
}

constexpr std::string_view HtmlEscapeOf(char c) noexcept
{
    switch (c) {
    case '&':
        return "&amp;";
    case '<':
        return "&lt;";
    case '>':
        return "&gt;";
    case '"':
        return "&quot;";
    case '\'':
        return "&#39;";
    default:
        return {};
    }
}

// 1 byte ずつ push_back すると巨大選択で容量チェックと NUL 書き込みが毎回走るため、
// エスケープ不要な区間をまとめて append する。
constexpr void AppendHtmlEscaped(std::pmr::string& out, std::string_view text)
{
    size_t run_begin = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto escaped = HtmlEscapeOf(text[i]);
        if (escaped.empty()) {
            continue;
        }
        out.append(text.data() + run_begin, i - run_begin);
        out.append(escaped);
        run_begin = i + 1;
    }
    out.append(text.data() + run_begin, text.size() - run_begin);
}

struct InlineState {
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strike = false;
    int16_t link_url_index = -1;
    bool operator==(const InlineState&) const = default;
};

// 開いたインラインタグを入れ子順に管理し、閉じ忘れを防ぐスコープヘルパー。
// 最大深さは <a><strong><em><s><code> の 5 段で、ヒープ割り当てなし。
class InlineTagScope {
public:
    constexpr explicit InlineTagScope(std::pmr::string& out) noexcept : out_(out)
    {}
    InlineTagScope(const InlineTagScope&) = delete;
    InlineTagScope& operator=(const InlineTagScope&) = delete;
    // CloseAll() は out_.append() 経由で bad_alloc を投げ得るが、デストラクタからは例外を出さない。
    // bad_alloc が起きた場合、生成中の HTML は途中で打ち切られるが、selection-copy 経路自体が
    // クリップボードに渡す前に切り詰める耐性を持つ (selection_html_test 参照)。
    ~InlineTagScope() noexcept
    {
        try {
            CloseAll();
        } catch (...) {
            MENDO_TRACE("InlineTagScope::~ - CloseAll bad_alloc");
        }
    }

    // 開く順: <a> → <strong> → <em> → <s> → <code>（code は最内側）。
    // 開いたタグと対の閉じタグをスタックにペアで積むため、フィールドと閉じ文字列のズレが起きない。
    constexpr void Open(const InlineState& s, std::span<const std::pmr::string> link_urls)
    {
        if (s.link_url_index >= 0 && static_cast<size_t>(s.link_url_index) < link_urls.size()) {
            out_.append("<a href=\"");
            AppendHtmlEscaped(out_, link_urls[static_cast<size_t>(s.link_url_index)]);
            out_.append("\">");
            Push("</a>");
        }
        if (s.bold) {
            OpenTag("<strong>", "</strong>");
        }
        if (s.italic) {
            OpenTag("<em>", "</em>");
        }
        if (s.strike) {
            OpenTag("<s>", "</s>");
        }
        if (s.code) {
            OpenTag("<code>", "</code>");
        }
    }

    constexpr void CloseAll()
    {
        while (count_ > 0) {
            out_.append(close_stack_[--count_]);
        }
    }

    constexpr bool IsApplied() const noexcept
    {
        return count_ != 0;
    }

private:
    constexpr void OpenTag(std::string_view open_tag, std::string_view close_tag)
    {
        out_.append(open_tag);
        Push(close_tag);
    }

    constexpr void Push(std::string_view close_tag) noexcept
    {
        assert(count_ < close_stack_.size());
        close_stack_[count_++] = close_tag;
    }

    std::pmr::string& out_;
    std::array<std::string_view, 5> close_stack_{};
    size_t count_ = 0;
};

enum class UrlSafety : uint8_t {
    Unchecked,
    Safe,
    Unsafe,
};

// 危険なスキームのリンクは外す。IsSafeUrlScheme の結果は URL ごとに url_safety へキャッシュする。
InlineState InlineStateOf(const TextRun& r, std::span<const std::pmr::string> link_urls, std::span<UrlSafety> url_safety)
{
    InlineState s{ r.bold(), r.italic(), r.code(), r.strikethrough(), r.link_url_index };
    if (s.link_url_index < 0 || static_cast<size_t>(s.link_url_index) >= link_urls.size()) {
        return s;
    }
    auto& safety = url_safety[static_cast<size_t>(s.link_url_index)];
    if (safety == UrlSafety::Unchecked) {
        safety = IsSafeUrlScheme(link_urls[static_cast<size_t>(s.link_url_index)]) ? UrlSafety::Safe : UrlSafety::Unsafe;
    }
    if (safety == UrlSafety::Unsafe) {
        s.link_url_index = -1;
    }
    return s;
}

// runs は start 昇順・非重複で並ぶ前提。run 境界単位で処理することで
// 同一 state 区間の比較と IsSafeUrlScheme を run あたり 1 回に抑える。
constexpr void AppendInlineHtml(
    std::pmr::string& out,
    std::string_view text,
    std::span<const TextRun> runs,
    std::span<const std::pmr::string> link_urls,
    uint32_t start, uint32_t end)
{
    end = ClampEndToText(end, text.size());
    if (start >= end) {
        return;
    }

    InlineTagScope scope(out);
    InlineState current;
    size_t run_idx = 0;
    uint32_t pos = start;

    // URL 数は典型的に少数のためスタック上にインライン格納する。
    mendo::small_vector<UrlSafety, 4> url_safety;
    url_safety.assign(link_urls.size(), UrlSafety::Unchecked);

    while (pos < end) {
        while (run_idx < runs.size() && runs[run_idx].start + runs[run_idx].length <= pos) {
            ++run_idx;
        }

        InlineState s;
        uint32_t segment_end;
        if (run_idx >= runs.size() || pos < runs[run_idx].start) {
            segment_end = (run_idx < runs.size()) ? std::min(end, runs[run_idx].start) : end;
        }
        else {
            const auto& r = runs[run_idx];
            s = InlineStateOf(r, link_urls, url_safety);
            segment_end = std::min(end, r.start + r.length);
        }

        if (s != current) {
            scope.CloseAll();
            current = s;
            scope.Open(current, link_urls);
        }

        // LF の位置で区切り、連続する非 LF 区間をまとめて AppendHtmlEscaped に渡す。
        uint32_t batch = pos;
        while (batch < segment_end) {
            const auto* lf_ptr = static_cast<const char*>(
                std::memchr(text.data() + batch, '\n', segment_end - batch));
            const uint32_t lf_pos = lf_ptr ? static_cast<uint32_t>(lf_ptr - text.data()) : segment_end;

            if (batch < lf_pos) {
                if (!scope.IsApplied()) {
                    scope.Open(current, link_urls);
                }
                AppendHtmlEscaped(out, text.substr(batch, lf_pos - batch));
            }

            if (lf_ptr) {
                scope.CloseAll();
                out.append("<br>");
                batch = lf_pos + 1;
            }
            else {
                batch = segment_end;
            }
        }
        pos = segment_end;
    }
}

void AppendNodeInlineHtml(std::pmr::string& out, const Node& node, uint32_t start, uint32_t end)
{
    AppendInlineHtml(out, node.GetText(), node.runs, node.view_link_urls(), start, end);
}

constexpr void AppendHexColor(std::pmr::string& out, uint32_t rgb)
{
    constexpr char kDigits[] = "0123456789abcdef";
    out.push_back('#');
    for (int shift = 20; shift >= 0; shift -= 4) {
        out.push_back(kDigits[(rgb >> shift) & 0xF]);
    }
}

constexpr std::optional<uint32_t> SyntaxTokenColor(SyntaxTokenType type, const theme_palette::SharedColors& palette) noexcept
{
    switch (type) {
    case SyntaxTokenType::Plain:
        return std::nullopt;
    case SyntaxTokenType::Keyword:
        return palette.syntax_keyword;
    case SyntaxTokenType::Type:
        return palette.syntax_type;
    case SyntaxTokenType::String:
        return palette.syntax_string;
    case SyntaxTokenType::Number:
        return palette.syntax_number;
    case SyntaxTokenType::Comment:
        return palette.syntax_comment;
    case SyntaxTokenType::Preprocessor:
        return palette.syntax_preprocessor;
    case SyntaxTokenType::Function:
        return palette.syntax_function;
    }
    std::unreachable();
}

// Plain は span を付けずにエスケープのみ行う。
constexpr void AppendSyntaxHighlightedSpan(
    std::pmr::string& out, std::string_view chunk, SyntaxTokenType type, const theme_palette::SharedColors& palette)
{
    const auto color = SyntaxTokenColor(type, palette);
    if (!color) {
        AppendHtmlEscaped(out, chunk);
        return;
    }
    out.append("<span style=\"color:");
    AppendHexColor(out, *color);
    out.append("\">");
    AppendHtmlEscaped(out, chunk);
    out.append("</span>");
}

// tokens は start 昇順・非重複の前提。トークンの隙間は Plain として出力する。
void AppendHighlightedCode(
    std::pmr::string& out,
    std::string_view text,
    std::span<const SyntaxToken> tokens,
    uint32_t start, uint32_t end,
    const theme_palette::SharedColors& palette)
{
    uint32_t pos = start;
    for (const auto& tok : tokens) {
        const uint32_t tok_end = tok.start + tok.length;
        if (tok_end <= pos) {
            continue;
        }
        if (tok.start >= end) {
            break;
        }
        if (tok.start > pos) {
            AppendHtmlEscaped(out, text.substr(pos, tok.start - pos));
            pos = tok.start;
        }
        const uint32_t seg_end = std::min(tok_end, end);
        AppendSyntaxHighlightedSpan(out, text.substr(pos, seg_end - pos), tok.type, palette);
        pos = seg_end;
        if (pos >= end) {
            break;
        }
    }
    if (pos < end) {
        AppendHtmlEscaped(out, text.substr(pos, end - pos));
    }
}

void AppendCodeBlockHtml(std::pmr::string& out, const Node& node, uint32_t start, uint32_t end, bool dark_mode)
{
    constexpr std::string_view kStyleTail =
        ";padding:12px;border-radius:4px;overflow:auto;font-family:Consolas,'Courier New',monospace;font-size:13px;line-height:1.45;\"><code>";
    constexpr std::string_view kClose = "</code></pre>";

    const auto& palette = PaletteFor(dark_mode);
    out.append("<pre style=\"background-color:");
    AppendHexColor(out, palette.code_bg);
    // ダーク時のみテキスト色を明示する（ライトは呼び出し側の親要素の色を継承）。
    if (dark_mode) {
        out.append(";color:");
        AppendHexColor(out, palette.code_text);
    }
    out.append(kStyleTail);
    const std::string_view text = node.GetText();
    end = ClampEndToText(end, text.size());
    if (start < end) {
        AppendHighlightedCode(out, text, node.syntax_tokens(), start, end, palette);
    }
    out.append(kClose);
}

// <thead> と <tbody> の排他的切替を管理する RAII スコープ。
// 行が header / data に切り替わるとき前セクションを自動で閉じ、スコープ終了時に
// 最後のセクションも閉じるため、in_thead/in_tbody フラグを持ち回す必要がない。
class TableSectionScope {
public:
    constexpr explicit TableSectionScope(std::pmr::string& out) noexcept : out_(out)
    {}
    // Close() は out_.append() 経由で bad_alloc を投げ得るが、デストラクタからは例外を出さない。
    // bad_alloc が起きた場合、生成中の HTML は途中で打ち切られるが、selection-copy 経路自体が
    // クリップボードに渡す前に切り詰める耐性を持つ。
    ~TableSectionScope() noexcept
    {
        try {
            Close();
        } catch (...) {
            MENDO_TRACE("TableSectionScope::~ - Close bad_alloc");
        }
    }
    TableSectionScope(const TableSectionScope&) = delete;
    TableSectionScope& operator=(const TableSectionScope&) = delete;

    constexpr void EnterThead()
    {
        Enter("<thead>", "</thead>");
    }
    constexpr void EnterTbody()
    {
        Enter("<tbody>", "</tbody>");
    }

private:
    constexpr void Enter(std::string_view open_tag, std::string_view close_tag)
    {
        if (close_tag_ == close_tag) {
            return;
        }
        Close();
        out_.append(open_tag);
        close_tag_ = close_tag;
    }
    // out_.append() は bad_alloc を投げうるため noexcept にはしない。
    constexpr void Close()
    {
        if (!close_tag_.empty()) {
            out_.append(close_tag_);
            close_tag_ = {};
        }
    }

    std::pmr::string& out_;
    std::string_view close_tag_{};
};

constexpr void AppendTableCellStyle(std::pmr::string& out, TableAlign align, bool dark_mode)
{
    // 共通の border+padding を先に出し、align 指定があれば追加して閉じる。
    out.append(" style=\"border:1px solid ");
    AppendHexColor(out, PaletteFor(dark_mode).table_border);
    out.append(";padding:6px 13px");
    switch (align) {
    case TableAlign::Center:
        out.append(";text-align:center");
        break;
    case TableAlign::Right:
        out.append(";text-align:right");
        break;
    default:
        break;
    }
    out.append(";\"");
}

// start / end は concat_text の offset。プレーンテキスト版と同じく読み順の範囲で切り取り、
// 範囲にかかる最初の行から最後の行までを出す。範囲外のセルは表の形を保つため空セルにする。
void AppendTableHtml(std::pmr::string& out, const Node& node, uint32_t start, uint32_t end, bool dark_mode)
{
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0 || tbl->col_count == 0) {
        const std::string_view text = node.GetText();
        end = ClampEndToText(end, text.size());
        out.append("<pre>");
        if (start < end) {
            AppendHtmlEscaped(out, text.substr(start, end - start));
        }
        out.append("</pre>");
        return;
    }

    // [start, end) は呼び出し側で LinearizedText (= concat_text) の長さにクランプ済み。
    if (start >= end) {
        return;
    }
    const auto col_count = static_cast<size_t>(tbl->col_count);
    const auto link_urls = node.view_link_urls();

    // 行のセル文字範囲 [行頭セルの開始, 行末セルの終端] は読み順で単調なので、範囲にかかりうる行帯を二分探索で求める。
    const auto row_begin = [&](size_t r) {
        return tbl->CellTextStart(r, 0);
    };
    const auto row_end = [&](size_t r) {
        return tbl->CellTextEnd(r, col_count - 1);
    };
    const auto rows = std::views::iota(size_t{ 0 }, static_cast<size_t>(tbl->row_count));
    size_t first_row = static_cast<size_t>(std::ranges::partition_point(rows, [&](size_t r) { return row_end(r) < start; }) - rows.begin());
    size_t end_row = static_cast<size_t>(std::ranges::partition_point(rows, [&](size_t r) { return row_begin(r) <= end; }) - rows.begin());
    // 端の行は、選択文字を持つセルがあるか、行全体が選択範囲に含まれるときだけ出す。
    // 区切り文字だけにかかる行は出さず、全選択では空セルだけの行 (空のヘッダ行など) も落とさない。
    const auto row_selected = [&](size_t r) {
        if (start <= row_begin(r) && row_end(r) <= end) {
            return true;
        }
        for (size_t c = 0; c < col_count; c++) {
            if (std::max(start, tbl->CellTextStart(r, c)) < std::min(end, tbl->CellTextEnd(r, c))) {
                return true;
            }
        }
        return false;
    };
    while (first_row < end_row && !row_selected(first_row)) {
        ++first_row;
    }
    while (end_row > first_row && !row_selected(end_row - 1)) {
        --end_row;
    }
    if (first_row >= end_row) {
        return;
    }

    out.append("<table style=\"border-collapse:collapse;\">");
    {
        TableSectionScope section(out);
        for (size_t r = first_row; r < end_row; r++) {
            const bool header_row = tbl->IsHeaderRow(r);
            if (header_row) {
                section.EnterThead();
            }
            else {
                section.EnterTbody();
            }

            const std::string_view open_tag = header_row ? "<th" : "<td";
            const std::string_view close_tag = header_row ? "</th>" : "</td>";
            out.append("<tr>");
            for (size_t c = 0; c < col_count; c++) {
                out.append(open_tag);
                AppendTableCellStyle(out, tbl->ColAlign(c), dark_mode);
                out.append(">");
                const uint32_t cell_start = tbl->CellTextStart(r, c);
                const uint32_t cell_end = tbl->CellTextEnd(r, c);
                AppendInlineHtml(out, tbl->GetCellText(r, c), tbl->GetCellRuns(r, c), link_urls,
                                 std::clamp(start, cell_start, cell_end) - cell_start, std::clamp(end, cell_start, cell_end) - cell_start);
                out.append(close_tag);
            }
            out.append("</tr>");
        }
    }
    out.append("</table>");
}

std::optional<std::string_view> FindLinkInRuns(std::span<const TextRun> runs, std::span<const std::pmr::string> link_urls, uint32_t pos)
{
    const auto it = std::ranges::find_if(runs, [pos](const TextRun& run) noexcept {
        return run.has_link() && (pos >= run.start) && (pos < run.start + run.length);
    });
    if (it == runs.end() || static_cast<size_t>(it->link_url_index) >= link_urls.size()) {
        return std::nullopt;
    }
    return link_urls[static_cast<size_t>(it->link_url_index)];
}

struct CellRunsHit {
    std::span<const TextRun> runs;
    uint32_t local_pos = 0;
};

// text_pos は linearized 形式 (concat_text) の offset。セル外 (区切り文字・padding) なら runs は空。
CellRunsHit FindTableCellRuns(const Node& node, uint32_t text_pos)
{
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0 || tbl->col_count == 0) {
        return {};
    }
    // cell_text_starts は単調非減少 (= 行頭 / セル間で進む)。upper_bound で text_pos を超える
    // 最初の境界を取り、その直前を flat cell index にする。
    const auto& starts = tbl->cell_text_starts;
    const auto it = std::ranges::upper_bound(starts, text_pos);
    if (it == starts.begin() || it == starts.end()) {
        return {};
    }
    const size_t idx = static_cast<size_t>(it - starts.begin()) - 1;
    const size_t col_count = tbl->col_count;
    const size_t r = idx / col_count;
    const size_t c = idx % col_count;
    if (r >= tbl->row_count) {
        return {};
    }
    const uint32_t start = starts[idx];
    // 末尾区切りを除いたセル長で in-range 判定。padding セルは長さ 0 で必ず外れる。
    if (text_pos >= start + tbl->GetCellText(r, c).size()) {
        return {};
    }
    return { tbl->GetCellRuns(r, c), text_pos - start };
}

} // namespace

std::pmr::string ExtractSelectedText(const std::pmr::vector<Node>& nodes, const TextSelection& selection)
{
    if (!selection.active) {
        return {};
    }

    // 全選択 (100MB 級) で倍々成長の再確保と旧新バッファの同時保持が起きないよう、
    // 先に正確な長さを数えて 1 回で確保する。
    const auto for_each_piece = [&](auto&& sink) {
        ForEachSelectedNode(nodes, selection, [&](int i, const Node& node) {
            const std::string_view text = node.LinearizedText();
            const auto [start, end] = selection.ClampedRange(i, text.size());
            if (start < end) {
                sink(text.substr(start, end - start));
            }
            if (i < selection.end_node) {
                sink(std::string_view{ "\r\n" });
            }
        });
    };
    size_t total = 0;
    for_each_piece([&total](std::string_view s) {
        total += s.size();
    });
    std::pmr::string result;
    result.reserve(total);
    for_each_piece([&result](std::string_view s) {
        result.append(s);
    });
    return result;
}

std::pmr::string ExtractSelectedTextAsHtml(const std::pmr::vector<Node>& nodes, const TextSelection& selection, bool dark_mode)
{
    if (!selection.active) {
        return {};
    }

    std::pmr::string out;
    size_t estimated = 0;
    // ノード全体ではなく選択範囲で見積もる (巨大な表の数セルだけのコピーでノード全体分を確保しない)。
    ForEachSelectedNode(nodes, selection, [&](int i, const Node& node) {
        const auto [start, end] = selection.ClampedRange(i, node.LinearizedText().size());
        if (start < end) {
            estimated += end - start;
        }
    });
    // シンタックスハイライトの span やテーブルの style 属性でタグのオーバーヘッドが増える。
    // 3 倍の予約は 100MB 選択で 300MB を確保するため 2 倍に留め、超過分は成長に任せる。
    out.reserve(estimated * 2 + 128);

    std::string_view list_close_tag;
    const auto close_list = [&]() {
        if (!list_close_tag.empty()) {
            out.append(list_close_tag);
            list_close_tag = {};
        }
    };

    ForEachSelectedNode(nodes, selection, [&](int i, const Node& node) {
        const auto text = node.LinearizedText();
        const auto [start, end] = selection.ClampedRange(i, text.size());

        if (IsListItem(node)) {
            const bool ordered = node.list_ordered();
            const std::string_view want_close = ordered ? "</ol>" : "</ul>";
            if (list_close_tag != want_close) {
                close_list();
                out.append(ordered ? "<ol>" : "<ul>");
                list_close_tag = want_close;
            }
        }
        else {
            close_list();
        }

        switch (node.type) {
        case NodeType::Heading: {
            const int level = std::clamp(static_cast<int>(node.heading_level()), 1, 6);
            std::format_to(std::back_inserter(out), "<h{}>", level);
            AppendNodeInlineHtml(out, node, start, end);
            std::format_to(std::back_inserter(out), "</h{}>", level);
            break;
        }
        case NodeType::Paragraph:
            out.append("<p>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append("</p>");
            break;
        case NodeType::CodeBlock:
            AppendCodeBlockHtml(out, node, start, end, dark_mode);
            break;
        case NodeType::BlockQuote:
            out.append("<blockquote>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append("</blockquote>");
            break;
        case NodeType::ListItem:
            out.append("<li>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append("</li>");
            break;
        case NodeType::TaskListItem:
            out.append(node.task_checked() ? "<li><input type=\"checkbox\" checked disabled> " : "<li><input type=\"checkbox\" disabled> ");
            AppendNodeInlineHtml(out, node, start, end);
            out.append("</li>");
            break;
        case NodeType::HorizontalRule:
            out.append("<hr>");
            break;
        case NodeType::Table:
            AppendTableHtml(out, node, start, end, dark_mode);
            break;
        case NodeType::Image:
            // src は Markdown 記述値をそのまま使う (相対パスは貼付先で解決不能だが仕様上の限界)。
            // alt は選択範囲内のノード線形化テキスト。
            out.append("<img src=\"");
            if (const auto* img = node.image_data()) {
                AppendHtmlEscaped(out, img->src);
            }
            out.append("\" alt=\"");
            if (start < end) {
                AppendHtmlEscaped(out, text.substr(start, end - start));
            }
            out.append("\">");
            break;
        }
    });
    close_list();
    return out;
}

std::pmr::string BuildCodeBlockHtmlFragment(const Node& node, bool dark_mode)
{
    std::pmr::string out;
    if (node.type != NodeType::CodeBlock) {
        return out;
    }
    const std::string_view text = node.GetText();
    out.reserve(text.size() * 3 + 128);
    AppendCodeBlockHtml(out, node, 0, static_cast<uint32_t>(text.size()), dark_mode);
    return out;
}

std::optional<std::string_view> FindLinkAtPosition(const Node& node, uint32_t text_pos)
{
    if (node.type == NodeType::Table) {
        const auto [runs, local_pos] = FindTableCellRuns(node, text_pos);
        return runs.empty() ? std::nullopt : FindLinkInRuns(runs, node.view_link_urls(), local_pos);
    }
    return FindLinkInRuns(node.runs, node.view_link_urls(), text_pos);
}
