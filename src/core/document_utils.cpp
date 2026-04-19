#include "document_utils.h"
#include "layout_cache.h"
#include "navigation_service.h"
#include <windows.h>
#include <cwctype>
#include <filesystem>
#include <format>
#include <algorithm>
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

void AppendHtmlEscaped(std::pmr::wstring& out, std::wstring_view text)
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

// runs は start 昇順・非重複で並ぶ前提。pos を単調増加で呼び出しつつ
// run_idx を更新していくため、AppendNodeInlineHtml 全体で O(N + M)。
// 危険なスキーム (file://, javascript: 等) のリンクはここで -1 に落とす。
InlineState ResolveInlineState(
    const std::pmr::vector<TextRun>& runs,
    const std::pmr::vector<std::pmr::wstring>& link_urls,
    uint32_t pos,
    size_t& run_idx)
{
    while (run_idx < runs.size() && runs[run_idx].start + runs[run_idx].length <= pos) {
        ++run_idx;
    }
    InlineState s;
    if (run_idx >= runs.size() || pos < runs[run_idx].start) {
        return s;
    }
    const auto& r = runs[run_idx];
    s.bold = r.bold();
    s.italic = r.italic();
    s.code = r.code();
    s.strike = r.strikethrough();
    s.link_url_index = r.link_url_index;
    if (s.link_url_index >= 0 && static_cast<size_t>(s.link_url_index) < link_urls.size()) {
        const auto& url = link_urls[static_cast<size_t>(s.link_url_index)];
        if (!IsSafeUrlScheme(url)) {
            s.link_url_index = -1;
        }
    }
    return s;
}

// 開く順: <a> → <strong> → <em> → <s> → <code>（code は最内側）。
void OpenInlineTags(std::pmr::wstring& out, const InlineState& s,
    const std::pmr::vector<std::pmr::wstring>& link_urls)
{
    if (s.link_url_index >= 0 && static_cast<size_t>(s.link_url_index) < link_urls.size()) {
        out.append(L"<a href=\"");
        AppendHtmlEscaped(out, link_urls[static_cast<size_t>(s.link_url_index)]);
        out.append(L"\">");
    }
    if (s.bold) { out.append(L"<strong>"); }
    if (s.italic) { out.append(L"<em>"); }
    if (s.strike) { out.append(L"<s>"); }
    if (s.code) { out.append(L"<code>"); }
}

void CloseInlineTags(std::pmr::wstring& out, const InlineState& s)
{
    if (s.code) { out.append(L"</code>"); }
    if (s.strike) { out.append(L"</s>"); }
    if (s.italic) { out.append(L"</em>"); }
    if (s.bold) { out.append(L"</strong>"); }
    if (s.link_url_index >= 0) { out.append(L"</a>"); }
}

void AppendNodeInlineHtml(std::pmr::wstring& out, const Node& node, uint32_t start, uint32_t end)
{
    const auto& text = node.GetText();
    if (end > text.size()) {
        end = static_cast<uint32_t>(text.size());
    }
    if (start >= end) {
        return;
    }
    InlineState current;
    bool tags_open = false;
    size_t run_idx = 0;
    for (uint32_t i = start; i < end; ++i) {
        const InlineState s = ResolveInlineState(node.runs, node.link_urls, i, run_idx);
        if (!(s == current)) {
            if (tags_open) {
                CloseInlineTags(out, current);
            }
            current = s;
            OpenInlineTags(out, current, node.link_urls);
            tags_open = true;
        }
        const wchar_t c = text[i];
        if (c == L'\n') {
            if (tags_open) {
                CloseInlineTags(out, current);
                tags_open = false;
            }
            out.append(L"<br>");
            current = InlineState{};
            continue;
        }
        AppendHtmlEscaped(out, std::wstring_view(&c, 1));
    }
    if (tags_open) {
        CloseInlineTags(out, current);
    }
}

void AppendHeadingOpenTag(std::pmr::wstring& out, int level)
{
    std::format_to(std::back_inserter(out), L"<h{}>", level);
}

void AppendHeadingCloseTag(std::pmr::wstring& out, int level)
{
    std::format_to(std::back_inserter(out), L"</h{}>", level);
}

bool IsListNode(const Node& n) noexcept
{
    return n.type == NodeType::ListItem || n.type == NodeType::TaskListItem;
}

// 順序付きリスト内の項目なら true。TaskListItem も親リストに応じて
// list_number が設定されるため両タイプを対象とする。
bool IsOrderedList(const Node& n) noexcept
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

std::pmr::wstring ExtractSelectedTextAsHtml(const std::pmr::vector<Node>& nodes, const TextSelection& selection)
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
    out.reserve(estimated * 2 + 32);

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
        if (i == selection.start_node) { start = selection.start_pos; }
        if (i == selection.end_node) { end = selection.end_pos; }
        if (end > text.size()) { end = static_cast<uint32_t>(text.size()); }

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
        case NodeType::Paragraph: {
            out.append(L"<p>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</p>");
            break;
        }
        case NodeType::CodeBlock: {
            out.append(L"<pre><code>");
            if (start < end) {
                AppendHtmlEscaped(out, std::wstring_view(text).substr(start, end - start));
            }
            out.append(L"</code></pre>");
            break;
        }
        case NodeType::BlockQuote: {
            out.append(L"<blockquote>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</blockquote>");
            break;
        }
        case NodeType::ListItem: {
            out.append(L"<li>");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</li>");
            break;
        }
        case NodeType::TaskListItem: {
            out.append(node.task_checked
                ? L"<li><input type=\"checkbox\" checked disabled> "
                : L"<li><input type=\"checkbox\" disabled> ");
            AppendNodeInlineHtml(out, node, start, end);
            out.append(L"</li>");
            break;
        }
        case NodeType::HorizontalRule: {
            out.append(L"<hr>");
            break;
        }
        case NodeType::Table:
        case NodeType::Image: {
            // テーブルと画像は未対応: 線形化テキストを <pre> で出力
            out.append(L"<pre>");
            if (start < end) {
                AppendHtmlEscaped(out, std::wstring_view(text).substr(start, end - start));
            }
            out.append(L"</pre>");
            break;
        }
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
