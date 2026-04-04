#include "document_utils.h"
#include <windows.h>
#include <cwctype>
#include <filesystem>
#include <format>
#include <algorithm>
#include <iterator>
#include <ranges>

std::pmr::wstring ExtractSelectedText(const std::pmr::vector<Node>& nodes,
    const TextSelection& selection)
{
    if (!selection.active) {
        return {};
    }

    std::pmr::wstring result;
    for (int i = selection.start_node; i <= selection.end_node; i++) {
        if (i < 0 || i >= static_cast<int>(nodes.size())) {
            continue;
        }
        const auto& text = nodes[i].text;

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

static std::optional<std::pmr::wstring> FindLinkInRuns(const std::pmr::vector<TextRun>& runs,
    const std::pmr::vector<std::pmr::wstring>& link_urls,
    uint32_t pos)
{
    for (const auto& run : runs) {
        if (run.has_link() && (pos >= run.start) && (pos < run.start + run.length)) {
            return link_urls[static_cast<size_t>(run.link_url_index)];
        }
    }
    return std::nullopt;
}

static const std::pmr::vector<TextRun>* FindTableCellRuns(const Node& node,
    uint32_t text_pos,
    uint32_t& local_pos)
{
    uint32_t offset = 0;
    const auto& rows = node.table_rows();
    const auto row_count = rows.size();
    for (size_t r = 0; r < row_count; r++) {
        const auto& row = rows[r];
        const auto col_count = row.cells.size();
        for (size_t c = 0; c < col_count; c++) {
            const uint32_t cell_len = static_cast<uint32_t>(row.cells[c].text.size());
            if (text_pos >= offset && text_pos < offset + cell_len) {
                local_pos = text_pos - offset;
                return &row.cells[c].runs;
            }
            offset += cell_len;
            if (c + 1 < col_count) {
                offset++; // タブ区切り
            }
        }
        if (r + 1 < row_count) {
            offset++; // 改行区切り
        }
    }
    return nullptr;
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
    result.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(result),
        [](wchar_t c) static noexcept -> wchar_t {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
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
        if (node.type == NodeType::Heading && node.anchor_id == target) {
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
    const auto [it_old, it_new] = std::mismatch(old_text.begin(), old_text.end(), new_text.begin(), new_text.end());
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
