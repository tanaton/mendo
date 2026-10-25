#include "selection_html.h"
#include "ascii_util.h"
#include "document_utils.h"
#include "nav.h"
#include "profiler.h"
#include "syntax.h"
#include "theme_palette.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory_resource>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>

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

// i と次のノードの間に改行を置くか。区切ったテキストの続きとは、区切るときに改行を取り除いたときだけ置く。
constexpr bool LineBreakAfter(const std::pmr::vector<Node>& nodes, int i) noexcept
{
    const auto next = static_cast<size_t>(i) + 1;
    return next >= nodes.size() || !nodes[next].continues_prev() || nodes[next].newline_before_chunk();
}

constexpr std::string_view kHtmlEscapeChars = "&<>\"'";

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

// エスケープ不要な区間をまとめて append する。数文字の find_first_of は STL がベクトル化している。
constexpr void AppendHtmlEscaped(std::pmr::string& out, std::string_view text)
{
    for (size_t p; (p = text.find_first_of(kHtmlEscapeChars)) != std::string_view::npos; text.remove_prefix(p + 1)) {
        out.append(text.substr(0, p));
        out.append(HtmlEscapeOf(text[p]));
    }
    out.append(text);
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
    // CloseAll() は bad_alloc を投げ得るが、例外の巻き戻し中に投げると terminate するので握りつぶす。
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
    constexpr void Open(const InlineState& s, std::span<const std::string_view> link_urls)
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

// 貼り付け先で読み込まれても外部アクセスの経路にならない src か。http(s) とスキームの無い相対パスだけ通す。
// javascript: / file: / C:\ などのスキームと、認証付きで取りに行かれる //host・\\host (UNC) は外す。
// ブラウザは解析前に先頭の制御文字・空白と途中のタブ・改行を捨てるので、それに合わせて判定する。
bool IsSafeImageSrc(std::string_view src)
{
    src.remove_prefix(std::ranges::find_if(src, [](char c) static { return static_cast<unsigned char>(c) > 0x20; }) - src.begin());
    std::string s;
    std::ranges::copy_if(src, std::back_inserter(s), [](char c) static { return c != '\t' && c != '\n' && c != '\r'; });
    if (ascii_util::istarts_with(s, "http://") || ascii_util::istarts_with(s, "https://")) {
        return true;
    }
    const auto is_slash = [](char c) static { return c == '/' || c == '\\'; };
    if (s.empty() || (s.size() >= 2 && is_slash(s[0]) && is_slash(s[1]))) {
        return false;
    }
    // スキームの ':' はパス・クエリ・フラグメントより前にしか現れない。
    const size_t p = s.find_first_of(":/\\?#");
    return p == std::string::npos || s[p] != ':';
}

// 危険なスキームのリンクは外す。
InlineState InlineStateOf(const TextRun& r, std::span<const std::string_view> link_urls)
{
    InlineState s{ r.bold(), r.italic(), r.code(), r.strikethrough(), r.link_url_index };
    if (s.link_url_index < 0 || static_cast<size_t>(s.link_url_index) >= link_urls.size()) {
        return s;
    }
    if (!IsSafeUrlScheme(link_urls[static_cast<size_t>(s.link_url_index)])) {
        s.link_url_index = -1;
    }
    return s;
}

// runs は start 昇順・非重複で並ぶ前提。run 境界単位で処理することで
// 同一 state 区間の比較を run あたり 1 回に抑える。
constexpr void AppendInlineHtml(
    std::pmr::string& out,
    std::string_view text,
    std::span<const TextRun> runs,
    std::span<const std::string_view> link_urls,
    uint32_t start, uint32_t end)
{
    assert(end <= text.size());
    if (start >= end) {
        return;
    }

    InlineTagScope scope(out);
    InlineState current;
    size_t run_idx = 0;
    uint32_t pos = start;

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
            s = InlineStateOf(r, link_urls);
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
    AppendInlineHtml(out, node.GetText(), node.runs(), node.view_link_urls(), start, end);
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
    case SyntaxTokenType::Inserted:
        return palette.syntax_inserted;
    case SyntaxTokenType::Deleted:
        return palette.syntax_deleted;
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

constexpr std::string_view kCodeBlockClose = "</code></pre>";

void AppendCodeBlockOpen(std::pmr::string& out, bool dark_mode)
{
    constexpr std::string_view kStyleTail =
        ";padding:12px;border-radius:4px;overflow:auto;font-family:'Cascadia Mono',Consolas,'Courier New',monospace;font-size:13px;line-height:1.45;\"><code>";
    const auto& palette = PaletteFor(dark_mode);
    out.append("<pre style=\"background-color:");
    AppendHexColor(out, palette.code_bg);
    // ダーク時のみテキスト色を明示する（ライトは呼び出し側の親要素の色を継承）。
    if (dark_mode) {
        out.append(";color:");
        AppendHexColor(out, palette.code_text);
    }
    out.append(kStyleTail);
}

constexpr void AppendTableCellStyle(std::pmr::string& out, TableAlign align, bool dark_mode)
{
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
        out.append("<pre>");
        if (start < end) {
            AppendHtmlEscaped(out, node.GetText().substr(start, end - start));
        }
        out.append("</pre>");
        return;
    }

    // [start, end) は呼び出し側で concat_text (= GetText) の長さにクランプ済み。
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
        return std::ranges::any_of(std::views::iota(size_t{ 0 }, col_count), [&](size_t c) {
            return std::max(start, tbl->CellTextStart(r, c)) < std::min(end, tbl->CellTextEnd(r, c));
        });
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

    // セルの開きタグは列と見出し行かどうかだけで決まるので、表ごとに 1 回だけ組み立てる。
    std::pmr::vector<std::pmr::string> open_tags(col_count * 2);
    for (size_t c = 0; c < col_count; c++) {
        for (const bool header : { false, true }) {
            auto& tag = open_tags[c * 2 + header];
            tag = header ? "<th" : "<td";
            AppendTableCellStyle(tag, tbl->ColAlign(c), dark_mode);
            tag += '>';
        }
    }

    out.append("<table style=\"border-collapse:collapse;\">");
    std::string_view section_close;
    for (size_t r = first_row; r < end_row; r++) {
        const bool header_row = tbl->IsHeaderRow(r);
        if (const std::string_view close = header_row ? "</thead>" : "</tbody>"; close != section_close) {
            out.append(section_close);
            out.append(header_row ? "<thead>" : "<tbody>");
            section_close = close;
        }

        const std::string_view close_tag = header_row ? "</th>" : "</td>";
        out.append("<tr>");
        for (size_t c = 0; c < col_count; c++) {
            out.append(open_tags[c * 2 + header_row]);
            const uint32_t cell_start = tbl->CellTextStart(r, c);
            const uint32_t cell_end = tbl->CellTextEnd(r, c);
            AppendInlineHtml(out, tbl->GetCellText(r, c), tbl->GetCellRuns(r, c), link_urls,
                             std::clamp(start, cell_start, cell_end) - cell_start, std::clamp(end, cell_start, cell_end) - cell_start);
            out.append(close_tag);
        }
        out.append("</tr>");
    }
    out.append(section_close);
    out.append("</table>");
}

// 開いているリストを外側から積む。各段の <li> は次の項目かその段を出るまで開いたままにし、
// loose list の本文や項目内のコードブロック・入れ子のリスト (LI の後ろに別ノードで並ぶ) を同じ <li> に入れる。
class HtmlListStack {
public:
    explicit HtmlListStack(std::pmr::string& out) noexcept : out_(out)
    {}

    // node を含まない段を閉じ、node が項目なら <li> を開く (同じリストの続きなら前の </li> を閉じる)。
    void Enter(const Node& node)
    {
        while (!levels_.empty() && !InOpenItem(levels_.back(), node)) {
            CloseInnermost();
        }
        if (!IsListItem(node)) {
            return;
        }
        const bool ordered = node.list_ordered();
        const int32_t number = node.list_number();
        const bool same_level = !levels_.empty() && levels_.back().indent == node.indent_level && levels_.back().quote_depth == node.quote_depth;
        // 番号が続かない番号付きは別のリスト ("1. a" の後の "1) b" 等)。
        if (same_level && levels_.back().ordered == ordered && (!ordered || levels_.back().next_number == number)) {
            out_.append("</li>");
            levels_.back().next_number = number + 1;
            return;
        }
        if (same_level) {
            CloseInnermost();
        }
        if (!ordered) {
            out_.append("<ul>");
        }
        else if (number == 1) {
            out_.append("<ol>");
        }
        else {
            std::format_to(std::back_inserter(out_), "<ol start=\"{}\">", number);
        }
        levels_.push_back({ node.indent_level, node.quote_depth, ordered, number + 1 });
    }

    void CloseAll()
    {
        while (!levels_.empty()) {
            CloseInnermost();
        }
    }

private:
    struct Level {
        int indent;
        int quote_depth;
        bool ordered;
        int32_t next_number;
    };

    // 同じ引用の深さなら段以上のインデントが項目の中。引用の深さが変わるノードは、その引用が項目の中で
    // 始まったとき (段より深いインデント) だけ含める。リストを出た直後の引用もインデントが上がるため。
    static constexpr bool InOpenItem(const Level& l, const Node& node) noexcept
    {
        return node.quote_depth == l.quote_depth ? node.indent_level >= l.indent
                                                 : node.quote_depth > l.quote_depth && node.indent_level > l.indent;
    }

    void CloseInnermost()
    {
        out_.append(levels_.back().ordered ? "</li></ol>" : "</li></ul>");
        levels_.pop_back();
    }

    std::pmr::string& out_;
    std::pmr::vector<Level> levels_;
};

// src は Markdown 記述値をそのまま使う (相対パスは貼付先で解決不能だが仕様上の限界)。
// alt は選択範囲内のノード線形化テキスト。[start, end) は呼び出し側でテキスト長にクランプ済み。
void AppendImageHtml(std::pmr::string& out, const Node& node, uint32_t start, uint32_t end)
{
    out.append("<img");
    if (const auto* img = node.image_data(); img && IsSafeImageSrc(img->src)) {
        out.append(" src=\"");
        AppendHtmlEscaped(out, img->src);
        out.append("\"");
    }
    out.append(" alt=\"");
    if (start < end) {
        AppendHtmlEscaped(out, node.GetText().substr(start, end - start));
    }
    out.append("\">");
}

std::optional<LinkHit> FindLinkInRuns(std::span<const TextRun> runs, std::span<const std::string_view> link_urls, uint32_t pos)
{
    // runs は start 昇順で重ならないので、pos を含みうるのは start <= pos の最後の run だけ。
    const auto it = std::ranges::upper_bound(runs, pos, {}, &TextRun::start);
    if (it == runs.begin()) {
        return std::nullopt;
    }
    const TextRun& run = *std::prev(it);
    if (!run.has_link() || pos >= run.start + run.length || static_cast<size_t>(run.link_url_index) >= link_urls.size()) {
        return std::nullopt;
    }
    const auto index = static_cast<uint32_t>(run.link_url_index);
    return LinkHit{ link_urls[index], index };
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
    const size_t idx = tbl->CellIndexAt(text_pos);
    const size_t col_count = tbl->col_count;
    const size_t r = idx / col_count;
    const size_t c = idx % col_count;
    if (r >= tbl->row_count) {
        return {};
    }
    // 末尾区切りを除いたセル長で in-range 判定。padding セルは長さ 0 で必ず外れる。
    if (text_pos >= tbl->CellTextEnd(r, c)) {
        return {};
    }
    return { tbl->GetCellRuns(r, c), text_pos - tbl->CellTextStart(r, c) };
}

} // namespace

std::pmr::string ExtractSelectedText(const std::pmr::vector<Node>& nodes, const TextSelection& selection)
{
    if (!selection.active) {
        return {};
    }

    // 全選択 (100MB 級) で倍々成長の再確保と旧新バッファの同時保持が起きないよう、
    // 先に正確な長さを数えて 1 回で確保する。
    // CF_UNICODETEXT は CRLF が慣例で、LF だけでは改行しないアプリがあるため、
    // ノード内の改行 (コード行・BR・表の行区切り) も CRLF にする。本文は読み込み時に LF へ正規化済み。
    static constexpr std::string_view kCrlf = "\r\n";
    const auto for_each_selected = [&](auto&& fn) {
        ForEachSelectedNode(nodes, selection, [&](int i, const Node& node) {
            const std::string_view text = node.GetText();
            const auto [start, end] = selection.ClampedRange(i, text.size());
            const bool separated = i < selection.end_node && LineBreakAfter(nodes, i);
            fn(start < end ? text.substr(start, end - start) : std::string_view{}, separated);
        });
    };
    // 数えるだけなら行に分けず、LF 1 個につき CR 1 個増えるとして足す (count はベクトル化される)。
    size_t total = 0;
    for_each_selected([&total](std::string_view s, bool separated) {
        total += s.size() + static_cast<size_t>(std::ranges::count(s, '\n')) + (separated ? kCrlf.size() : 0);
    });
    std::pmr::string result;
    // 短い行が数百万続くと append ごとの容量確認が効くので、確保済みの領域へ直接書く。
    result.resize_and_overwrite(total, [&](char* const out, size_t) {
        char* p = out;
        const auto put = [&p](std::string_view s) {
            p = std::ranges::copy(s, p).out;
        };
        for_each_selected([&](std::string_view s, bool separated) {
            for (size_t lf = s.find('\n'); lf != std::string_view::npos; lf = s.find('\n')) {
                put(s.substr(0, lf));
                put(kCrlf);
                s.remove_prefix(lf + 1);
            }
            put(s);
            if (separated) {
                put(kCrlf);
            }
        });
        assert(static_cast<size_t>(p - out) == total);
        return static_cast<size_t>(p - out);
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
        const auto [start, end] = selection.ClampedRange(i, node.GetText().size());
        if (start < end) {
            estimated += end - start;
        }
    });
    // シンタックスハイライトの span やテーブルの style 属性でタグのオーバーヘッドが増える。
    // 3 倍の予約は 100MB 選択で 300MB を確保するため 2 倍に留め、超過分は成長に任せる。
    out.reserve(estimated * 2 + 128);

    HtmlListStack lists(out);
    ForEachSelectedNode(nodes, selection, [&](int i, const Node& node) {
        const auto [start, end] = selection.ClampedRange(i, node.GetText().size());
        lists.Enter(node);
        // 区切ったテキストの並びは 1 つの要素にまとめ、間に元の改行を戻す。
        const bool opens = !node.continues_prev() || i == selection.start_node;
        const bool closes = !node.continues_next() || i == selection.end_node;
        const bool newline_before = !opens && node.newline_before_chunk();

        const auto wrap_inline = [&](std::string_view open, std::string_view close) {
            if (opens) {
                out.append(open);
            }
            else if (newline_before) {
                out.append("<br>");
            }
            AppendNodeInlineHtml(out, node, start, end);
            if (closes) {
                out.append(close);
            }
        };
        switch (node.type) {
        case NodeType::Heading: {
            const int level = std::clamp(static_cast<int>(node.heading_level()), 1, 6);
            std::format_to(std::back_inserter(out), "<h{}>", level);
            AppendNodeInlineHtml(out, node, start, end);
            std::format_to(std::back_inserter(out), "</h{}>", level);
            break;
        }
        case NodeType::Paragraph:
            wrap_inline("<p>", "</p>");
            break;
        case NodeType::CodeBlock:
        case NodeType::Diagram:
            if (opens) {
                AppendCodeBlockOpen(out, dark_mode);
            }
            else if (newline_before) {
                out.push_back('\n');
            }
            if (start < end) {
                AppendHighlightedCode(out, node.GetText(), TokenizeCodeNode(node), start, end, PaletteFor(dark_mode));
            }
            if (closes) {
                out.append(kCodeBlockClose);
            }
            break;
        case NodeType::BlockQuote:
            wrap_inline("<blockquote>", "</blockquote>");
            break;
        case NodeType::ListItem:
            wrap_inline("<li>", "");
            break;
        case NodeType::TaskListItem:
            wrap_inline(node.task_checked() ? "<li><input type=\"checkbox\" checked disabled> " : "<li><input type=\"checkbox\" disabled> ", "");
            break;
        case NodeType::HorizontalRule:
            out.append("<hr>");
            break;
        case NodeType::Table:
            AppendTableHtml(out, node, start, end, dark_mode);
            break;
        case NodeType::Image:
            AppendImageHtml(out, node, start, end);
            break;
        }
    });
    lists.CloseAll();
    return out;
}

std::optional<LinkHit> FindLinkHitAtPosition(const Node& node, uint32_t text_pos)
{
    if (node.type == NodeType::Table) {
        const auto [runs, local_pos] = FindTableCellRuns(node, text_pos);
        return runs.empty() ? std::nullopt : FindLinkInRuns(runs, node.view_link_urls(), local_pos);
    }
    return FindLinkInRuns(node.runs(), node.view_link_urls(), text_pos);
}
