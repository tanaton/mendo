#include "document_utils.h"
#include <windows.h>
#include <cwctype>
#include <filesystem>

std::pmr::wstring ExtractSelectedText(const std::pmr::vector<Node>& nodes,
    const TextSelection& selection) {
    if (!selection.active) {
        return {};
    }

    std::pmr::wstring result;
    for (int i = selection.start_node; i <= selection.end_node; i++) {
        if (i < 0 || i >= static_cast<int>(nodes.size())) continue;
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
    uint32_t pos) {
    for (const auto& run : runs) {
        if (run.link_url.has_value() &&
            pos >= run.start && pos < run.start + run.length) {
            return run.link_url;
        }
    }
    return std::nullopt;
}

static const std::pmr::vector<TextRun>* FindTableCellRuns(const Node& node,
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
            if (c + 1 < row.cells.size()) offset++; // タブ区切り
        }
        if (r + 1 < node.table_rows.size()) offset++; // 改行区切り
    }
    return nullptr;
}

std::optional<std::pmr::wstring> FindLinkAtPosition(const Node& node, uint32_t text_pos) {
    if (node.type == NodeType::Table) {
        uint32_t local_pos = 0;
        auto* runs = FindTableCellRuns(node, text_pos, local_pos);
        return runs ? FindLinkInRuns(*runs, local_pos) : std::nullopt;
    }
    return FindLinkInRuns(node.runs, text_pos);
}

std::pmr::wstring ToLowerAscii(std::wstring_view text) {
    std::pmr::wstring result;
    result.reserve(text.size());
    for (wchar_t c : text) {
        if (c >= L'A' && c <= L'Z') {
            result += static_cast<wchar_t>(c - L'A' + L'a');
        }
        else {
            result += c;
        }
    }
    return result;
}

int FindAnchorNodeIndex(const std::pmr::vector<Node>& nodes, std::wstring_view anchor) {
    if (anchor.empty()) {
        return -1;
    }

    // 比較のためアンカーを小文字に変換
    std::pmr::wstring target = ToLowerAscii(anchor);

    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        const auto& node = nodes[i];
        if (node.type == NodeType::Heading &&
            std::wstring_view{ node.anchor_id } == std::wstring_view{ target }) {
            return i;
        }
    }
    return -1;
}

WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) {
    WordBoundary result;
    if (text.empty()) {
        return result;
    }
    if (pos >= text.size()) {
        pos = static_cast<uint32_t>(text.size()) - 1;
    }

    auto is_word_char = [](wchar_t c) {
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

bool IsMarkdownFile(std::wstring_view path) {
    auto ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) {
        c = std::towlower(c);
    }
    return ext == L".md" || ext == L".markdown" || ext == L".mkd";
}

std::pmr::wstring ExtractFilename(std::wstring_view path) {
    if (path.empty()) {
        return {};
    }
    return std::pmr::wstring{ std::wstring_view{std::filesystem::path(path).filename().native()} };
}

std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent) {
    std::pmr::wstring title;
    if (path.empty()) {
        title = L"mendo";
    }
    else {
        auto filename = ExtractFilename(path);
        title = filename.empty() ? L"mendo" : filename + L" - mendo";
    }
    if (zoom_percent > 0 && zoom_percent != 100) {
        title += L" (" + std::to_wstring(zoom_percent) + L"%)";
    }
    return title;
}
