#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory_resource>
#include "syntax.h"

enum class NodeType : uint8_t {
    Heading,
    Paragraph,
    CodeBlock,
    HorizontalRule,
    ListItem,
    BlockQuote,
    Table,
    TaskListItem
};

struct TextRun {
    uint32_t start = 0;
    uint32_t length = 0;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strikethrough = false;
    std::optional<std::pmr::wstring> link_url;
};

// Text selection: positions are (node_index, char_offset) pairs.
// "start" is always <= "end" in document order.
struct TextSelection {
    int start_node = -1;
    uint32_t start_pos = 0;
    int end_node = -1;
    uint32_t end_pos = 0;
    bool active = false;

    constexpr void Clear() noexcept { start_node = end_node = -1; active = false; }

    // Normalize anchor/caret so start <= end in document order
    static constexpr TextSelection MakeOrdered(int node_a, uint32_t pos_a,
                                     int node_b, uint32_t pos_b) noexcept {
        TextSelection s;
        if (node_a < node_b || (node_a == node_b && pos_a <= pos_b)) {
            s.start_node = node_a; s.start_pos = pos_a;
            s.end_node = node_b;   s.end_pos = pos_b;
        } else {
            s.start_node = node_b; s.start_pos = pos_b;
            s.end_node = node_a;   s.end_pos = pos_a;
        }
        s.active = (s.start_node != s.end_node || s.start_pos != s.end_pos);
        return s;
    }
};

// Table cell data (pure domain — no layout cache)
struct TableCell {
    std::pmr::wstring text;
    std::pmr::vector<TextRun> runs;
    bool is_header = false;
    int align = 0; // 0=left, 1=center, 2=right (from MD_ALIGN)
};

struct TableRow {
    std::pmr::vector<TableCell> cells;
};

struct Node {
    NodeType type = NodeType::Paragraph;
    int heading_level = 0;
    int indent_level = 0;
    int list_number = 0;      // 0 = unordered, >0 = ordered list number
    bool task_checked = false;
    std::pmr::wstring text;
    std::pmr::vector<TextRun> runs;
    std::pmr::wstring anchor_id;   // For headings: GitHub-style slug for internal links
    SyntaxLanguage code_language = SyntaxLanguage::None;
    std::pmr::vector<SyntaxToken> syntax_tokens;

    // Table data (only used when type == Table)
    std::pmr::vector<TableRow> table_rows;
};
