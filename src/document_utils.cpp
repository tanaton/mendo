#include "document_utils.h"
#include <windows.h>

std::wstring ExtractSelectedText(const std::vector<Node>& nodes,
                                  const TextSelection& selection) {
    if (!selection.active) return {};

    std::wstring result;
    for (int i = selection.start_node; i <= selection.end_node; i++) {
        if (i < 0 || i >= static_cast<int>(nodes.size())) continue;
        const auto& text = nodes[i].text;

        uint32_t start = 0;
        uint32_t end = static_cast<uint32_t>(text.size());
        if (i == selection.start_node) start = selection.start_pos;
        if (i == selection.end_node)   end = selection.end_pos;

        if (start < end && start < text.size()) {
            if (end > text.size()) end = static_cast<uint32_t>(text.size());
            result += text.substr(start, end - start);
        }
        // Add newline between nodes
        if (i < selection.end_node) {
            result += L"\r\n";
        }
    }
    return result;
}

static std::optional<std::wstring> FindLinkInRuns(const std::vector<TextRun>& runs,
                                                   uint32_t pos) {
    for (const auto& run : runs) {
        if (run.link_url.has_value() &&
            pos >= run.start && pos < run.start + run.length) {
            return run.link_url;
        }
    }
    return std::nullopt;
}

static const std::vector<TextRun>* FindTableCellRuns(const Node& node,
                                                      uint32_t text_pos,
                                                      uint32_t& local_pos) {
    uint32_t offset = 0;
    for (size_t r = 0; r < node.table_rows.size(); r++) {
        const auto& row = node.table_rows[r];
        for (size_t c = 0; c < row.cells.size(); c++) {
            uint32_t cell_len = static_cast<uint32_t>(row.cells[c].text.size());
            if (text_pos >= offset && text_pos < offset + cell_len) {
                local_pos = text_pos - offset;
                return &row.cells[c].runs;
            }
            offset += cell_len;
            if (c + 1 < row.cells.size()) offset++; // tab separator
        }
        if (r + 1 < node.table_rows.size()) offset++; // newline separator
    }
    return nullptr;
}

std::optional<std::wstring> FindLinkAtPosition(const Node& node, uint32_t text_pos) {
    if (node.type == NodeType::Table) {
        uint32_t local_pos = 0;
        auto* runs = FindTableCellRuns(node, text_pos, local_pos);
        return runs ? FindLinkInRuns(*runs, local_pos) : std::nullopt;
    }
    return FindLinkInRuns(node.runs, text_pos);
}

int FindAnchorNodeIndex(const std::vector<Node>& nodes, const std::wstring& anchor) {
    if (anchor.empty()) return -1;

    // Convert anchor to lowercase for comparison
    std::wstring target = anchor;
    for (auto& c : target) {
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    }

    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Heading && node.anchor_id == target) {
            return i;
        }
    }
    return -1;
}

WordBoundary FindWordBoundaries(const std::wstring& text, uint32_t pos) {
    WordBoundary result;
    if (text.empty()) return result;
    if (pos >= text.size()) pos = static_cast<uint32_t>(text.size()) - 1;

    auto is_word_char = [](wchar_t c) {
        return IsCharAlphaNumericW(c) || c == L'_';
    };

    if (!is_word_char(text[pos])) return result;

    uint32_t word_start = pos;
    while (word_start > 0 && is_word_char(text[word_start - 1])) word_start--;

    uint32_t word_end = pos + 1;
    while (word_end < text.size() && is_word_char(text[word_end])) word_end++;

    result.start = word_start;
    result.end = word_end;
    result.found = true;
    return result;
}

std::wstring ExtractFilename(const std::wstring& path) {
    if (path.empty()) return {};
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::wstring BuildTitleString(const std::wstring& path, int zoom_percent) {
    std::wstring title;
    if (path.empty()) {
        title = L"mendo";
    } else {
        auto filename = ExtractFilename(path);
        title = filename.empty() ? L"mendo" : filename + L" - mendo";
    }
    if (zoom_percent > 0 && zoom_percent != 100) {
        title += L" (" + std::to_wstring(zoom_percent) + L"%)";
    }
    return title;
}
