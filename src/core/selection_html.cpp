#include "selection_html.h"
#include "nav.h"
#include "syntax.h"
#include "theme.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>

std::pmr::wstring ExtractSelectedText(const std::pmr::vector<Node>& nodes, const TextSelection& selection)
{
    if (!selection.active) {
        return {};
    }

    std::pmr::wstring result;
    for (int i = selection.start_node; i <= selection.end_node; i++) {
        if (i < 0 || i >= static_cast<int>(nodes.size())) {
            continue;
        }
        const auto& text = nodes[i].GetText();

        uint32_t start = 0;
        uint32_t end = static_cast<uint32_t>(text.size());
        if (i == selection.start_node) {
            start = selection.start_pos;
        }
        if (i == selection.end_node) {
            end = selection.end_pos;
        }

        if (start < end && start < text.size()) {
            if (end > text.size()) {
                end = static_cast<uint32_t>(text.size());
            }
            result.append(text.data() + start, end - start);
        }
        if (i < selection.end_node) {
            result += L"\r\n";
        }
    }
    return result;
}

namespace {

constexpr void AppendHtmlEscaped(std::pmr::wstring& out, std::wstring_view text)
{
    for (const wchar_t c : text) {
        switch (c) {
        case L'&':
            out.append(L"&amp;");
            break;
        case L'<':
            out.append(L"&lt;");
            break;
        case L'>':
            out.append(L"&gt;");
            break;
        case L'"':
            out.append(L"&quot;");
            break;
        case L'\'':
            out.append(L"&#39;");
            break;
        default:
            out.push_back(c);
            break;
        }
    }
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
    constexpr explicit InlineTagScope(std::pmr::wstring& out) noexcept : out_(out)
    {}
    InlineTagScope(const InlineTagScope&) = delete;
    InlineTagScope& operator=(const InlineTagScope&) = delete;
    // CloseAll() は out_.append() 経由で bad_alloc を投げ得るが、デストラクタからは例外を出さない。
    ~InlineTagScope() noexcept
    {
        try {
            CloseAll();
        } catch (...) {
        }
    }

    // 開く順: <a> → <strong> → <em> → <s> → <code>（code は最内側）。
    // 開いたタグと対の閉じタグをスタックにペアで積むため、フィールドと閉じ文字列のズレが起きない。
    constexpr void Open(const InlineState& s, std::span<const std::pmr::wstring> link_urls)
    {
        applied_ = true;
        if (s.link_url_index >= 0 && static_cast<size_t>(s.link_url_index) < link_urls.size()) {
            out_.append(L"<a href=\"");
            AppendHtmlEscaped(out_, link_urls[static_cast<size_t>(s.link_url_index)]);
            out_.append(L"\">");
            Push(L"</a>");
        }
        if (s.bold) {
            out_.append(L"<strong>");
            Push(L"</strong>");
        }
        if (s.italic) {
            out_.append(L"<em>");
            Push(L"</em>");
        }
        if (s.strike) {
            out_.append(L"<s>");
            Push(L"</s>");
        }
        if (s.code) {
            out_.append(L"<code>");
            Push(L"</code>");
        }
    }

    constexpr void CloseAll()
    {
        applied_ = false;
        while (count_ > 0) {
            out_.append(close_stack_[--count_]);
        }
    }

    constexpr bool IsApplied() const noexcept
    {
        return applied_;
    }

private:
    constexpr void Push(std::wstring_view close_tag) noexcept
    {
        assert(count_ < close_stack_.size());
        close_stack_[count_++] = close_tag;
    }

    std::pmr::wstring& out_;
    std::array<std::wstring_view, 5> close_stack_{};
    size_t count_ = 0;
    bool applied_ = false;
};

// runs は start 昇順・非重複で並ぶ前提。run 境界単位で処理することで
// 同一 state 区間の比較と IsSafeUrlScheme を run あたり 1 回に抑える。
constexpr void AppendInlineHtml(
    std::pmr::wstring& out,
    std::wstring_view text,
    std::span<const TextRun> runs,
    std::span<const std::pmr::wstring> link_urls,
    uint32_t start, uint32_t end)
{
    if (end > text.size()) {
        end = static_cast<uint32_t>(text.size());
    }
    if (start >= end) {
        return;
    }

    InlineTagScope scope(out);
    InlineState current;
    size_t run_idx = 0;
    uint32_t pos = start;

    enum class UrlSafety : uint8_t {
        Unchecked = 0,
        Safe = 1,
        Unsafe = 2
    };
    std::pmr::vector<UrlSafety> url_safety(out.get_allocator().resource());
    if (!link_urls.empty()) {
        url_safety.assign(link_urls.size(), UrlSafety::Unchecked);
    }

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
            s.bold = r.bold();
            s.italic = r.italic();
            s.code = r.code();
            s.strike = r.strikethrough();
            s.link_url_index = r.link_url_index;
            if (s.link_url_index >= 0 && static_cast<size_t>(s.link_url_index) < link_urls.size()) {
                const size_t ui = static_cast<size_t>(s.link_url_index);
                if (url_safety[ui] == UrlSafety::Unchecked) {
                    url_safety[ui] = IsSafeUrlScheme(link_urls[ui]) ? UrlSafety::Safe : UrlSafety::Unsafe;
                }
                if (url_safety[ui] == UrlSafety::Unsafe) {
                    s.link_url_index = -1;
                }
            }
            segment_end = std::min(end, r.start + r.length);
        }

        if (!(s == current)) {
            scope.CloseAll();
            current = s;
            scope.Open(current, link_urls);
        }

        for (uint32_t i = pos; i < segment_end; ++i) {
            const wchar_t c = text[i];
            if (c == L'\n') {
                scope.CloseAll();
                out.append(L"<br>");
                continue;
            }
            if (!scope.IsApplied()) {
                scope.Open(current, link_urls);
            }
            AppendHtmlEscaped(out, std::wstring_view(&c, 1));
        }
        pos = segment_end;
    }
}

void AppendNodeInlineHtml(std::pmr::wstring& out, const Node& node, uint32_t start, uint32_t end)
{
    AppendInlineHtml(out, node.GetText(), node.runs, node.view_link_urls(), start, end);
}

constexpr void AppendHexColor(std::pmr::wstring& out, uint32_t rgb)
{
    constexpr wchar_t kDigits[] = L"0123456789abcdef";
    out.push_back(L'#');
    out.push_back(kDigits[(rgb >> 20) & 0xF]);
    out.push_back(kDigits[(rgb >> 16) & 0xF]);
    out.push_back(kDigits[(rgb >> 12) & 0xF]);
    out.push_back(kDigits[(rgb >> 8) & 0xF]);
    out.push_back(kDigits[(rgb >> 4) & 0xF]);
    out.push_back(kDigits[rgb & 0xF]);
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

constexpr void AppendSyntaxHighlightedSpan(std::pmr::wstring& out, std::wstring_view chunk, SyntaxTokenType type, bool dark_mode)
{
    const auto& palette = dark_mode ? theme_palette::kDark : theme_palette::kLight;
    const auto color = SyntaxTokenColor(type, palette);
    if (!color) {
        AppendHtmlEscaped(out, chunk);
        return;
    }
    out.append(L"<span style=\"color:");
    AppendHexColor(out, *color);
    out.append(L"\">");
    AppendHtmlEscaped(out, chunk);
    out.append(L"</span>");
}

void AppendCodeBlockHtml(std::pmr::wstring& out, const Node& node, uint32_t start, uint32_t end, bool dark_mode)
{
    constexpr std::wstring_view kStyleTail =
        LR"(;padding:12px;border-radius:4px;overflow:auto;font-family:Consolas,'Courier New',monospace;font-size:13px;line-height:1.45;"><code>)";
    constexpr std::wstring_view kClose = L"</code></pre>";

    const auto& palette = dark_mode ? theme_palette::kDark : theme_palette::kLight;
    out.append(L"<pre style=\"background-color:");
    AppendHexColor(out, palette.code_bg);
    // ダーク時のみテキスト色を明示する（ライトは呼び出し側の親要素の色を継承）。
    if (dark_mode) {
        out.append(L";color:");
        AppendHexColor(out, palette.code_text);
    }
    out.append(kStyleTail);
    const auto& text = node.GetText();
    if (end > text.size()) {
        end = static_cast<uint32_t>(text.size());
    }
    if (start < end) {
        const std::wstring_view text_view{ text };
        const auto& tokens = node.syntax_tokens();
        if (tokens.empty()) {
            AppendHtmlEscaped(out, text_view.substr(start, end - start));
        }
        else {
            // tokens は start 昇順・連続配置の前提。Plain 区間は span を省略する。
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
                    const uint32_t plain_end = std::min(tok.start, end);
                    AppendHtmlEscaped(out, text_view.substr(pos, plain_end - pos));
                    pos = plain_end;
                    if (pos >= end) {
                        break;
                    }
                }
                const uint32_t seg_start = std::max(pos, tok.start);
                const uint32_t seg_end = std::min(tok_end, end);
                if (seg_start < seg_end) {
                    const auto chunk = text_view.substr(seg_start, seg_end - seg_start);
                    if (tok.type == SyntaxTokenType::Plain) {
                        AppendHtmlEscaped(out, chunk);
                    }
                    else {
                        AppendSyntaxHighlightedSpan(out, chunk, tok.type, dark_mode);
                    }
                    pos = seg_end;
                }
            }
            if (pos < end) {
                AppendHtmlEscaped(out, text_view.substr(pos, end - pos));
            }
        }
    }
    out.append(kClose);
}

// <thead> と <tbody> の排他的切替を管理する RAII スコープ。
// 行が header / data に切り替わるとき前セクションを自動で閉じ、スコープ終了時に
// 最後のセクションも閉じるため、in_thead/in_tbody フラグを持ち回す必要がない。
class TableSectionScope {
public:
    constexpr explicit TableSectionScope(std::pmr::wstring& out) noexcept : out_(out)
    {}
    // Close() は out_.append() 経由で bad_alloc を投げ得るが、デストラクタからは例外を出さない。
    ~TableSectionScope() noexcept
    {
        try {
            Close();
        } catch (...) {
        }
    }
    TableSectionScope(const TableSectionScope&) = delete;
    TableSectionScope& operator=(const TableSectionScope&) = delete;

    constexpr void EnterThead()
    {
        Enter(L"<thead>", L"</thead>");
    }
    constexpr void EnterTbody()
    {
        Enter(L"<tbody>", L"</tbody>");
    }

private:
    constexpr void Enter(std::wstring_view open_tag, std::wstring_view close_tag)
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

    std::pmr::wstring& out_;
    std::wstring_view close_tag_{};
};

constexpr void AppendTableCellStyle(std::pmr::wstring& out, TableAlign align, bool dark_mode)
{
    // 共通の border+padding を先に出し、align 指定があれば追加して閉じる。
    const auto& palette = dark_mode ? theme_palette::kDark : theme_palette::kLight;
    out.append(LR"( style="border:1px solid )");
    AppendHexColor(out, palette.table_border);
    out.append(L";padding:6px 13px");
    switch (align) {
    case TableAlign::Center:
        out.append(L";text-align:center");
        break;
    case TableAlign::Right:
        out.append(L";text-align:right");
        break;
    default:
        break;
    }
    out.append(L";\"");
}

void AppendTableHtml(std::pmr::wstring& out, const Node& node, uint32_t start, uint32_t end, bool dark_mode)
{
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0) {
        // テーブルデータがない場合はフラットテキストを <pre> で出力
        const auto& text = node.GetText();
        if (end > text.size()) {
            end = static_cast<uint32_t>(text.size());
        }
        out.append(L"<pre>");
        if (start < end) {
            AppendHtmlEscaped(out, std::wstring_view(text).substr(start, end - start));
        }
        out.append(L"</pre>");
        return;
    }

    const auto row_count = tbl->row_count;
    const auto col_count = static_cast<size_t>(tbl->col_count);
    const auto link_urls = node.view_link_urls();

    out.append(LR"(<table style="border-collapse:collapse;">)");
    {
        TableSectionScope section(out);
        for (size_t r = 0; r < row_count; r++) {
            const bool header_row = tbl->IsHeaderRow(r);
            if (header_row) {
                section.EnterThead();
            }
            else {
                section.EnterTbody();
            }

            const std::wstring_view open_tag = header_row ? L"<th" : L"<td";
            const std::wstring_view close_tag = header_row ? L"</th>" : L"</td>";
            out.append(L"<tr>");
            for (size_t c = 0; c < col_count; c++) {
                out.append(open_tag);
                AppendTableCellStyle(out, tbl->ColAlign(c), dark_mode);
                out.append(L">");
                const auto cell_text = tbl->GetCellText(r, c);
                AppendInlineHtml(out, cell_text, tbl->GetCellRuns(r, c), link_urls, 0, static_cast<uint32_t>(cell_text.size()));
                out.append(close_tag);
            }
            out.append(L"</tr>");
        }
    }
    out.append(L"</table>");
}

void AppendHeadingOpenTag(std::pmr::wstring& out, int level)
{
    std::format_to(std::back_inserter(out), L"<h{}>", level);
}

void AppendHeadingCloseTag(std::pmr::wstring& out, int level)
{
    std::format_to(std::back_inserter(out), L"</h{}>", level);
}

constexpr bool IsListNode(const Node& n) noexcept
{
    return n.type == NodeType::ListItem || n.type == NodeType::TaskListItem;
}

constexpr bool IsOrderedList(const Node& n) noexcept
{
    return IsListNode(n) && n.list_number > 0;
}

std::optional<std::pmr::wstring> FindLinkInRuns(std::span<const TextRun> runs, std::span<const std::pmr::wstring> link_urls, uint32_t pos)
{
    const auto it = std::ranges::find_if(runs, [pos](const TextRun& run) noexcept {
        return run.has_link() && (pos >= run.start) && (pos < run.start + run.length);
    });
    if (it == runs.end() || it->link_url_index < 0 || static_cast<size_t>(it->link_url_index) >= link_urls.size()) {
        return std::nullopt;
    }
    return link_urls[static_cast<size_t>(it->link_url_index)];
}

// 指定 text_pos のセルの runs を span で返す。見つからなければ空 span。
// text_pos は linearized 形式 (concat_text) の offset。
std::span<const TextRun> FindTableCellRuns(const Node& node, uint32_t text_pos, uint32_t& local_pos)
{
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0) {
        return {};
    }
    const auto row_count = tbl->row_count;
    const auto col_count = static_cast<size_t>(tbl->col_count);
    for (size_t r = 0; r < row_count; r++) {
        for (size_t c = 0; c < col_count; c++) {
            const uint32_t start = tbl->CellTextStart(r, c);
            // GetCellText が末尾区切りを除外したサイズを返すので、セル境界判定にそのまま使える。
            if (text_pos >= start && text_pos < start + tbl->GetCellText(r, c).size()) {
                local_pos = text_pos - start;
                return tbl->GetCellRuns(r, c);
            }
        }
    }
    return {};
}

} // namespace

std::pmr::wstring ExtractSelectedTextAsHtml(const std::pmr::vector<Node>& nodes, const TextSelection& selection, bool dark_mode)
{
    if (!selection.active) {
        return {};
    }

    std::pmr::wstring out;
    size_t estimated = 0;
    for (int i = selection.start_node; i <= selection.end_node; ++i) {
        if (i >= 0 && i < static_cast<int>(nodes.size())) {
            estimated += nodes[i].GetText().size();
        }
    }
    // シンタックスハイライトの span やテーブルの style 属性でタグのオーバーヘッドが増えるため、
    // 平均的なドキュメントが一回の allocation で収まるよう多めに確保する。
    out.reserve(estimated * 3 + 128);

    const wchar_t* list_close_tag = nullptr;
    auto close_list = [&]() {
        if (list_close_tag) {
            out.append(list_close_tag);
            list_close_tag = nullptr;
        }
    };

    for (int i = selection.start_node; i <= selection.end_node; ++i) {
        if (i < 0 || i >= static_cast<int>(nodes.size())) {
            continue;
        }
        const auto& node = nodes[i];
        const auto& text = node.GetText();

        uint32_t start = 0;
        uint32_t end = static_cast<uint32_t>(text.size());
        if (i == selection.start_node) {
            start = selection.start_pos;
        }
        if (i == selection.end_node) {
            end = selection.end_pos;
        }
        if (end > text.size()) {
            end = static_cast<uint32_t>(text.size());
        }

        if (IsListNode(node)) {
            const wchar_t* want_close = IsOrderedList(node) ? L"</ol>" : L"</ul>";
            if (list_close_tag != want_close) {
                close_list();
                out.append(IsOrderedList(node) ? L"<ol>" : L"<ul>");
                list_close_tag = want_close;
            }
        }
        else {
            close_list();
        }

        switch (node.type) {
        case NodeType::Heading: {
            const int level = std::clamp(static_cast<int>(node.heading_level), 1, 6);
            AppendHeadingOpenTag(out, level);
            AppendNodeInlineHtml(out, node, start, end);
            AppendHeadingCloseTag(out, level);
            break;
        }
        case NodeType::Paragraph:
            out.append(L"<p>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</p>");
            break;
        case NodeType::CodeBlock:
            AppendCodeBlockHtml(out, node, start, end, dark_mode);
            break;
        case NodeType::BlockQuote:
            out.append(L"<blockquote>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</blockquote>");
            break;
        case NodeType::ListItem:
            out.append(L"<li>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</li>");
            break;
        case NodeType::TaskListItem:
            out.append(node.task_checked ? L"<li><input type=\"checkbox\" checked disabled> " : L"<li><input type=\"checkbox\" disabled> ");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</li>");
            break;
        case NodeType::HorizontalRule:
            out.append(L"<hr>");
            break;
        case NodeType::Table:
            AppendTableHtml(out, node, start, end, dark_mode);
            break;
        case NodeType::Image:
            // 画像は未対応: 線形化テキストを <pre> で出力
            out.append(L"<pre>");
            if (start < end) {
                AppendHtmlEscaped(out, std::wstring_view(text).substr(start, end - start));
            }
            out.append(L"</pre>");
            break;
        }
    }
    close_list();
    return out;
}

std::optional<std::pmr::wstring> FindLinkAtPosition(const Node& node, uint32_t text_pos)
{
    if (node.type == NodeType::Table) {
        uint32_t local_pos = 0;
        const auto runs = FindTableCellRuns(node, text_pos, local_pos);
        return runs.empty() ? std::nullopt : FindLinkInRuns(runs, node.view_link_urls(), local_pos);
    }
    return FindLinkInRuns(std::span<const TextRun>{ node.runs.data(), node.runs.size() }, node.view_link_urls(), text_pos);
}
