#include "document_utils.h"
#include "layout_cache.h"
#include "navigation_service.h"
#include "syntax.h"
#include <windows.h>
#include <cwctype>
#include <filesystem>
#include <format>
#include <algorithm>
#include <array>
#include <iterator>
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
        // ノード間に改行を追加
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
        case L'&': out.append(L"&amp;"); break;
        case L'<': out.append(L"&lt;"); break;
        case L'>': out.append(L"&gt;"); break;
        case L'"': out.append(L"&quot;"); break;
        case L'\'': out.append(L"&#39;"); break;
        default: out.push_back(c); break;
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
    constexpr explicit InlineTagScope(std::pmr::wstring& out) noexcept : out_(out) {}
    InlineTagScope(const InlineTagScope&) = delete;
    InlineTagScope& operator=(const InlineTagScope&) = delete;
    constexpr ~InlineTagScope() { CloseAll(); }

    // 開く順: <a> → <strong> → <em> → <s> → <code>（code は最内側）。
    // 開いたタグと対の閉じタグをスタックにペアで積むため、フィールドと閉じ文字列のズレが起きない。
    constexpr void Open(const InlineState& s, const std::pmr::vector<std::pmr::wstring>& link_urls)
    {
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

    constexpr void CloseAll() noexcept
    {
        while (count_ > 0) {
            out_.append(close_stack_[--count_]);
        }
    }

    constexpr bool IsOpen() const noexcept { return count_ > 0; }

private:
    constexpr void Push(std::wstring_view close_tag) noexcept
    {
        close_stack_[count_++] = close_tag;
    }

    std::pmr::wstring& out_;
    std::array<std::wstring_view, 5> close_stack_{};
    size_t count_ = 0;
};

// runs は start 昇順・非重複で並ぶ前提。run 境界単位で処理することで
// 同一 state 区間の比較と IsSafeUrlScheme を run あたり 1 回に抑える。
constexpr void AppendInlineHtml(std::pmr::wstring& out,
    std::wstring_view text,
    const std::pmr::vector<TextRun>& runs,
    const std::pmr::vector<std::pmr::wstring>& link_urls,
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
                if (!IsSafeUrlScheme(link_urls[static_cast<size_t>(s.link_url_index)])) {
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
            if (!scope.IsOpen()) {
                scope.Open(current, link_urls);
            }
            AppendHtmlEscaped(out, std::wstring_view(&c, 1));
        }
        pos = segment_end;
    }
}

void AppendNodeInlineHtml(std::pmr::wstring& out, const Node& node, uint32_t start, uint32_t end)
{
    AppendInlineHtml(out, node.GetText(), node.runs, node.link_urls, start, end);
}

// シンタックスハイライト用 span の prefix を、ライト / ダークテーマで切り替えて返す。
// ライト: VS Code Light 風、ダーク: VS Code Dark+ 風。Plain は空で「色付けなし」。
constexpr std::wstring_view SyntaxSpanPrefix(SyntaxTokenType type, bool dark) noexcept
{
    switch (type) {
    case SyntaxTokenType::Keyword:      return dark ? L"<span style=\"color:#C586C0\">" : L"<span style=\"color:#AF00DB\">";
    case SyntaxTokenType::Type:         return dark ? L"<span style=\"color:#4EC9B0\">" : L"<span style=\"color:#267F99\">";
    case SyntaxTokenType::String:       return dark ? L"<span style=\"color:#CE9178\">" : L"<span style=\"color:#A31515\">";
    case SyntaxTokenType::Number:       return dark ? L"<span style=\"color:#B5CEA8\">" : L"<span style=\"color:#098658\">";
    case SyntaxTokenType::Comment:      return dark ? L"<span style=\"color:#6A9955\">" : L"<span style=\"color:#008000\">";
    case SyntaxTokenType::Preprocessor: return dark ? L"<span style=\"color:#DCDCAA\">" : L"<span style=\"color:#795E26\">";
    case SyntaxTokenType::Function:     return dark ? L"<span style=\"color:#DCDCAA\">" : L"<span style=\"color:#795E26\">";
    default:                            return {};
    }
}

constexpr void AppendSyntaxHighlightedSpan(std::pmr::wstring& out, std::wstring_view chunk,
    SyntaxTokenType type, bool dark_mode)
{
    const auto prefix = SyntaxSpanPrefix(type, dark_mode);
    if (prefix.empty()) {
        AppendHtmlEscaped(out, chunk);
        return;
    }
    out.append(prefix);
    AppendHtmlEscaped(out, chunk);
    out.append(L"</span>");
}

void AppendCodeBlockHtml(std::pmr::wstring& out, const Node& node, uint32_t start, uint32_t end,
    bool dark_mode)
{
    constexpr std::wstring_view kOpenLight =
        LR"(<pre style="background-color:#f6f8fa;padding:12px;border-radius:4px;overflow:auto;font-family:Consolas,'Courier New',monospace;font-size:13px;line-height:1.45;"><code>)";
    constexpr std::wstring_view kOpenDark =
        LR"(<pre style="background-color:#2d2d2d;color:#d4d4d4;padding:12px;border-radius:4px;overflow:auto;font-family:Consolas,'Courier New',monospace;font-size:13px;line-height:1.45;"><code>)";
    constexpr std::wstring_view kClose = L"</code></pre>";

    out.append(dark_mode ? kOpenDark : kOpenLight);
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

// cell.align は md4c の MD_ALIGN（0=DEFAULT, 1=LEFT, 2=CENTER, 3=RIGHT）をそのまま保持。
inline constexpr int kTableAlignCenter = 2;
inline constexpr int kTableAlignRight = 3;

// <thead> と <tbody> の排他的切替を管理する RAII スコープ。
// 行が header / data に切り替わるとき前セクションを自動で閉じ、スコープ終了時に
// 最後のセクションも閉じるため、in_thead/in_tbody フラグを持ち回す必要がない。
class TableSectionScope {
public:
    constexpr explicit TableSectionScope(std::pmr::wstring& out) noexcept : out_(out) {}
    constexpr ~TableSectionScope() { Close(); }
    TableSectionScope(const TableSectionScope&) = delete;
    TableSectionScope& operator=(const TableSectionScope&) = delete;

    constexpr void EnterThead() { Enter(L"<thead>", L"</thead>"); }
    constexpr void EnterTbody() { Enter(L"<tbody>", L"</tbody>"); }

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
    constexpr void Close() noexcept
    {
        if (!close_tag_.empty()) {
            out_.append(close_tag_);
            close_tag_ = {};
        }
    }

    std::pmr::wstring& out_;
    std::wstring_view close_tag_{};
};

constexpr void AppendTableCellStyle(std::pmr::wstring& out, const TableCell& cell, bool dark_mode)
{
    // 共通の border+padding を先に出し、align 指定があれば追加して閉じる。
    constexpr std::wstring_view kOpenLight = LR"( style="border:1px solid #d0d7de;padding:6px 13px)";
    constexpr std::wstring_view kOpenDark  = LR"( style="border:1px solid #3c3c3c;padding:6px 13px)";
    out.append(dark_mode ? kOpenDark : kOpenLight);
    switch (cell.align) {
    case kTableAlignCenter: out.append(L";text-align:center"); break;
    case kTableAlignRight:  out.append(L";text-align:right"); break;
    default: break;
    }
    out.append(L";\"");
}

// テーブルノードは常に <table> 構造で出力する。
// node.GetText() の線形化テキストはレイアウトパスで初めて埋まるため、
// ここで start/end による部分選択判定は行わない（全体出力が実用的）。
void AppendTableHtml(std::pmr::wstring& out, const Node& node, uint32_t start, uint32_t end,
    bool dark_mode)
{
    if (!node.has_table() || node.table_rows().empty()) {
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

    out.append(LR"(<table style="border-collapse:collapse;">)");
    {
        TableSectionScope section(out);
        for (const auto& row : node.table_rows()) {
            const bool header_row = !row.cells.empty() && row.cells[0].is_header;
            if (header_row) {
                section.EnterThead();
            }
            else {
                section.EnterTbody();
            }

            const std::wstring_view open_tag = header_row ? L"<th" : L"<td";
            const std::wstring_view close_tag = header_row ? L"</th>" : L"</td>";
            out.append(L"<tr>");
            for (const auto& cell : row.cells) {
                out.append(open_tag);
                AppendTableCellStyle(out, cell, dark_mode);
                out.append(L">");
                AppendInlineHtml(out, cell.text, cell.runs, node.link_urls, 0,
                    static_cast<uint32_t>(cell.text.size()));
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

// 順序付きリスト内の項目なら true。TaskListItem も親リストに応じて
// list_number が設定されるため両タイプを対象とする。
constexpr bool IsOrderedList(const Node& n) noexcept
{
    return IsListNode(n) && n.list_number > 0;
}

} // namespace

static std::optional<std::pmr::wstring> FindLinkInRuns(const std::pmr::vector<TextRun>& runs, const std::pmr::vector<std::pmr::wstring>& link_urls, uint32_t pos)
{
    const auto it = std::ranges::find_if(runs, [pos](const TextRun& run) noexcept {
        return run.has_link() && (pos >= run.start) && (pos < run.start + run.length);
    });
    if (it == runs.end()) {
        return std::nullopt;
    }
    return link_urls[static_cast<size_t>(it->link_url_index)];
}

static const std::pmr::vector<TextRun>* FindTableCellRuns(const Node& node, uint32_t text_pos, uint32_t& local_pos)
{
    uint32_t offset = 0;
    const auto& rows = node.table_rows();
    const auto last_row = static_cast<ptrdiff_t>(rows.size()) - 1;
    for (const auto& [r, row] : rows | std::views::enumerate) {
        const auto last_col = static_cast<ptrdiff_t>(row.cells.size()) - 1;
        for (const auto& [c, cell] : row.cells | std::views::enumerate) {
            const uint32_t cell_len = static_cast<uint32_t>(cell.text.size());
            if (text_pos >= offset && text_pos < offset + cell_len) {
                local_pos = text_pos - offset;
                return &cell.runs;
            }
            offset += cell_len;
            if (c < last_col) {
                offset++; // タブ区切り
            }
        }
        if (r < last_row) {
            offset++; // 改行区切り
        }
    }
    return nullptr;
}

std::pmr::wstring ExtractSelectedTextAsHtml(const std::pmr::vector<Node>& nodes,
    const TextSelection& selection, bool dark_mode)
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
            const int level = std::clamp(node.heading_level, 1, 6);
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
            out.append(node.task_checked
                ? L"<li><input type=\"checkbox\" checked disabled> "
                : L"<li><input type=\"checkbox\" disabled> ");
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
        const auto* runs = FindTableCellRuns(node, text_pos, local_pos);
        return runs ? FindLinkInRuns(*runs, node.link_urls, local_pos) : std::nullopt;
    }
    return FindLinkInRuns(node.runs, node.link_urls, text_pos);
}

std::pmr::wstring ToLowerAscii(std::wstring_view text)
{
    std::pmr::wstring result;
    result.resize_and_overwrite(text.size(), [&text](wchar_t* buf, size_t count) noexcept -> size_t {
        for (size_t i = 0; i < count; ++i) {
            const wchar_t c = text[i];
            buf[i] = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }
        return count;
    });
    return result;
}

int FindAnchorNodeIndex(const std::pmr::vector<Node>& nodes, std::wstring_view anchor)
{
    if (anchor.empty()) {
        return -1;
    }

    // 比較のためアンカーを小文字に変換
    const std::pmr::wstring target = ToLowerAscii(anchor);

    for (const auto& [i, node] : nodes | std::views::enumerate) {
        if (node.type == NodeType::Heading && node.anchor_id() == target) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept
{
    WordBoundary result;
    if (text.empty()) {
        return result;
    }
    if (pos >= text.size()) {
        pos = static_cast<uint32_t>(text.size()) - 1;
    }

    const auto is_word_char = [](wchar_t c) static noexcept {
        return IsCharAlphaNumericW(c) || c == L'_';
    };

    if (!is_word_char(text[pos])) {
        return result;
    }

    uint32_t word_start = pos;
    while (word_start > 0 && is_word_char(text[word_start - 1])) {
        word_start--;
    }

    uint32_t word_end = pos + 1;
    while (word_end < text.size() && is_word_char(text[word_end])) {
        word_end++;
    }

    result.start = word_start;
    result.end = word_end;
    result.found = true;
    return result;
}

bool IsMarkdownFile(std::wstring_view path)
{
    auto ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) {
        c = std::towlower(c);
    }
    return ext == L".md" || ext == L".markdown" || ext == L".mkd";
}

size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept
{
    const auto [it_old, it_new] = std::ranges::mismatch(old_text, new_text);
    if (it_old == old_text.end() && it_new == new_text.end()) {
        return std::string_view::npos;
    }
    return static_cast<size_t>(it_old - old_text.begin());
}

int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, uint32_t diff_offset) noexcept
{
    // source_offset はパース順で基本的に単調増加するため二分探索を使用。
    // UINT32_MAX（未設定）ノードに当たった場合は左に有効ノードを探してから判定する。
    int lo = 0, hi = static_cast<int>(nodes.size()) - 1;
    int result = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint32_t offset = nodes[mid].source_offset;
        if (offset == UINT32_MAX) {
            // 左側で最も近い有効ノードを探す
            int probe = mid - 1;
            while (probe >= lo && nodes[probe].source_offset == UINT32_MAX) {
                probe--;
            }
            if (probe < lo) {
                // lo..mid に有効ノードがないので右へ
                lo = mid + 1;
            }
            else if (nodes[probe].source_offset <= diff_offset) {
                result = probe;
                lo = mid + 1;
            }
            else {
                hi = probe - 1;
            }
            continue;
        }
        if (offset <= diff_offset) {
            result = mid;
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }
    return result;
}

float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept
{
    const int changed_node = FindNodeBySourceOffset(nodes, static_cast<uint32_t>(diff_pos));
    if (changed_node < 0 || changed_node >= static_cast<int>(cache.size())) {
        return fallback_scroll;
    }

    float node_y = cache[changed_node].y_position;
    const float node_h = cache[changed_node].height;

    // ノード内での相対位置を推定してY座標を補正
    const uint32_t node_start = nodes[changed_node].source_offset;
    if (node_start != UINT32_MAX) {
        uint32_t next_start = static_cast<uint32_t>(content.size());
        const auto node_count = static_cast<int>(nodes.size());
        for (int i = changed_node + 1; i < node_count; ++i) {
            if (nodes[i].source_offset != UINT32_MAX && nodes[i].source_offset > node_start) {
                next_start = nodes[i].source_offset;
                break;
            }
        }
        if (next_start > node_start) {
            const float fraction = static_cast<float>(diff_pos - node_start)
                / static_cast<float>(next_start - node_start);
            node_y += node_h * std::min(fraction, 1.0f);
        }
    }

    const float margin = viewport_height * 0.2f;
    return std::max(0.0f, node_y - margin);
}

std::pmr::wstring ExtractFilename(std::wstring_view path)
{
    if (path.empty()) {
        return {};
    }
    return std::pmr::wstring{ std::filesystem::path(path).filename().native() };
}

std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent)
{
    std::pmr::wstring title;
    if (path.empty()) {
        title = L"mendo";
    }
    else {
        const auto filename = ExtractFilename(path);
        title = filename.empty() ? L"mendo" : filename + L" - mendo";
    }
    if (zoom_percent > 0 && zoom_percent != 100) {
        std::format_to(std::back_inserter(title), L" ({}%)", zoom_percent);
    }
    return title;
}
