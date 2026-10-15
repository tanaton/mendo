#include "selection_html.h"
#include "ascii_util.h"
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

enum class UrlSafety : uint8_t {
    Unchecked,
    Safe,
    Unsafe,
};

// URL 数は典型的に少数なので、その範囲ではスタック上の領域を使い heap 確保しない。
class UrlSafetyTable {
public:
    explicit UrlSafetyTable(size_t url_count) : table_(url_count, UrlSafety::Unchecked, &resource_) {}

    std::span<UrlSafety> Span() noexcept
    {
        return table_;
    }

private:
    std::array<std::byte, 32> buffer_;
    std::pmr::monotonic_buffer_resource resource_{ buffer_.data(), buffer_.size() };
    std::pmr::vector<UrlSafety> table_;
};

// 貼り付け先で読み込まれても外部アクセスの経路にならない src か。http(s) とスキームの無い相対パスだけ通す。
// javascript: / file: / C:\ などのスキームと、認証付きで取りに行かれる //host・\\host (UNC) は外す。
// ブラウザは解析前に先頭の制御文字・空白と途中のタブ・改行を捨てるので、それに合わせて判定する。
bool IsSafeImageSrc(std::string_view src)
{
    src.remove_prefix(std::ranges::find_if(src, [](char c) { return static_cast<unsigned char>(c) > 0x20; }) - src.begin());
    std::string s;
    std::ranges::copy_if(src, std::back_inserter(s), [](char c) { return c != '\t' && c != '\n' && c != '\r'; });
    if (ascii_util::istarts_with(s, "http://") || ascii_util::istarts_with(s, "https://")) {
        return true;
    }
    const auto is_slash = [](char c) { return c == '/' || c == '\\'; };
    if (s.empty() || (s.size() >= 2 && is_slash(s[0]) && is_slash(s[1]))) {
        return false;
    }
    // スキームの ':' はパス・クエリ・フラグメントより前にしか現れない。
    const size_t p = s.find_first_of(":/\\?#");
    return p == std::string::npos || s[p] != ':';
}

// 危険なスキームのリンクは外す。IsSafeUrlScheme の結果は URL ごとに url_safety へキャッシュする。
InlineState InlineStateOf(const TextRun& r, std::span<const std::string_view> link_urls, std::span<UrlSafety> url_safety)
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
// 同一 state 区間の比較を run あたり 1 回に抑える。
// url_safety は link_urls と同じ長さで、テーブルではセル間で共有して IsSafeUrlScheme を URL あたり 1 回にする。
constexpr void AppendInlineHtml(
    std::pmr::string& out,
    std::string_view text,
    std::span<const TextRun> runs,
    std::span<const std::string_view> link_urls,
    std::span<UrlSafety> url_safety,
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
    const auto link_urls = node.view_link_urls();
    UrlSafetyTable url_safety{ link_urls.size() };
    AppendInlineHtml(out, node.GetText(), node.runs(), link_urls, url_safety.Span(), start, end);
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
        AppendHighlightedCode(out, text, Tokenize(text, node.code_language()), start, end, palette);
    }
    out.append(kClose);
}

// <thead>/<tbody> や <ol>/<ul> のように、同時に 1 つだけ開いている要素の切替を管理する RAII スコープ。
// 別の要素に切り替わるとき前の要素を自動で閉じ、スコープ終了時に最後の要素も閉じる。
class ExclusiveTagScope {
public:
    constexpr explicit ExclusiveTagScope(std::pmr::string& out) noexcept : out_(out)
    {}
    // Close() は bad_alloc を投げ得るが、例外の巻き戻し中に投げると terminate するので握りつぶす。
    ~ExclusiveTagScope() noexcept
    {
        try {
            Close();
        } catch (...) {
            MENDO_TRACE("ExclusiveTagScope::~ - Close bad_alloc");
        }
    }
    ExclusiveTagScope(const ExclusiveTagScope&) = delete;
    ExclusiveTagScope& operator=(const ExclusiveTagScope&) = delete;

    constexpr void Enter(std::string_view open_tag, std::string_view close_tag)
    {
        if (close_tag_ == close_tag) {
            return;
        }
        Close();
        out_.append(open_tag);
        close_tag_ = close_tag;
    }
    constexpr bool IsOpen() const noexcept
    {
        return !close_tag_.empty();
    }
    // out_.append() は bad_alloc を投げうるため noexcept にはしない。
    constexpr void Close()
    {
        if (!close_tag_.empty()) {
            out_.append(close_tag_);
            close_tag_ = {};
        }
    }

private:
    std::pmr::string& out_;
    std::string_view close_tag_{};
};

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

    UrlSafetyTable url_safety{ link_urls.size() };
    out.append("<table style=\"border-collapse:collapse;\">");
    {
        ExclusiveTagScope section(out);
        for (size_t r = first_row; r < end_row; r++) {
            const bool header_row = tbl->IsHeaderRow(r);
            if (header_row) {
                section.Enter("<thead>", "</thead>");
            }
            else {
                section.Enter("<tbody>", "</tbody>");
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
                AppendInlineHtml(out, tbl->GetCellText(r, c), tbl->GetCellRuns(r, c), link_urls, url_safety.Span(),
                                 std::clamp(start, cell_start, cell_end) - cell_start, std::clamp(end, cell_start, cell_end) - cell_start);
                out.append(close_tag);
            }
            out.append("</tr>");
        }
    }
    out.append("</table>");
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
    // CF_UNICODETEXT は CRLF が慣例で、LF だけでは改行しないアプリがあるため、
    // ノード内の改行 (コード行・BR・表の行区切り) も CRLF にする。本文は読み込み時に LF へ正規化済み。
    static constexpr std::string_view kCrlf = "\r\n";
    const auto for_each_selected = [&](auto&& fn) {
        ForEachSelectedNode(nodes, selection, [&](int i, const Node& node) {
            const std::string_view text = node.LinearizedText();
            const auto [start, end] = selection.ClampedRange(i, text.size());
            fn(start < end ? text.substr(start, end - start) : std::string_view{}, i < selection.end_node);
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
        const auto [start, end] = selection.ClampedRange(i, node.LinearizedText().size());
        if (start < end) {
            estimated += end - start;
        }
    });
    // シンタックスハイライトの span やテーブルの style 属性でタグのオーバーヘッドが増える。
    // 3 倍の予約は 100MB 選択で 300MB を確保するため 2 倍に留め、超過分は成長に任せる。
    out.reserve(estimated * 2 + 128);

    ExclusiveTagScope list(out);
    // loose list の本文や項目内のコードブロックは LI の後ろに別ノードで並ぶので、
    // </li> は次の項目かリストを出るまで遅らせ、LI 以上の深さのノードを同じ <li> に入れる
    // (リストが開いている間は常に <li> も開いている)。
    // リストを出た直後の引用も indent_level が上がるため、引用の深さが変わるノードは
    // その引用が項目の中で始まったとき (最外側の引用が項目より深い) だけ項目に含める。
    int list_indent = 0;
    int list_quote_depth = 0;
    const auto in_open_li = [&](const Node& node) {
        return list.IsOpen() && node.indent_level >= list_indent &&
               (node.quote_depth == list_quote_depth || node.quote_outer_indent > list_indent);
    };
    const auto close_li = [&] {
        if (list.IsOpen()) {
            out.append("</li>");
        }
    };

    ForEachSelectedNode(nodes, selection, [&](int i, const Node& node) {
        const auto text = node.LinearizedText();
        const auto [start, end] = selection.ClampedRange(i, text.size());

        if (IsListItem(node)) {
            close_li();
            if (!node.list_ordered()) {
                list.Enter("<ul>", "</ul>");
            }
            else if (node.list_number() == 1) {
                list.Enter("<ol>", "</ol>");
            }
            else {
                char open_tag[32];
                const auto open_end = std::format_to(open_tag, "<ol start=\"{}\">", node.list_number());
                list.Enter({ open_tag, open_end }, "</ol>");
            }
            list_indent = node.indent_level;
            list_quote_depth = node.quote_depth;
        }
        else if (!in_open_li(node)) {
            close_li();
            list.Close();
        }

        const auto wrap_inline = [&](std::string_view open, std::string_view close) {
            out.append(open);
            AppendNodeInlineHtml(out, node, start, end);
            out.append(close);
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
            AppendCodeBlockHtml(out, node, start, end, dark_mode);
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
            // src は Markdown 記述値をそのまま使う (相対パスは貼付先で解決不能だが仕様上の限界)。
            // alt は選択範囲内のノード線形化テキスト。
            out.append("<img");
            if (const auto* img = node.image_data(); img && IsSafeImageSrc(img->src)) {
                out.append(" src=\"");
                AppendHtmlEscaped(out, img->src);
                out.append("\"");
            }
            out.append(" alt=\"");
            if (start < end) {
                AppendHtmlEscaped(out, text.substr(start, end - start));
            }
            out.append("\">");
            break;
        }
    });
    // out を返す前に閉じる (デストラクタでは戻り値のコピー後になりうる)。
    close_li();
    list.Close();
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

std::optional<LinkHit> FindLinkHitAtPosition(const Node& node, uint32_t text_pos)
{
    if (node.type == NodeType::Table) {
        const auto [runs, local_pos] = FindTableCellRuns(node, text_pos);
        return runs.empty() ? std::nullopt : FindLinkInRuns(runs, node.view_link_urls(), local_pos);
    }
    return FindLinkInRuns(node.runs(), node.view_link_urls(), text_pos);
}
