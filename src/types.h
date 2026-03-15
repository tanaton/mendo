#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <wrl/client.h>
#include <dwrite.h>
#include "syntax.h"

using Microsoft::WRL::ComPtr;

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
    std::optional<std::wstring> link_url;
};

// Text selection: positions are (node_index, char_offset) pairs.
// "start" is always <= "end" in document order.
struct TextSelection {
    int start_node = -1;
    uint32_t start_pos = 0;
    int end_node = -1;
    uint32_t end_pos = 0;
    bool active = false;

    void Clear() { start_node = end_node = -1; active = false; }

    // Normalize anchor/caret so start <= end in document order
    static TextSelection MakeOrdered(int node_a, uint32_t pos_a,
                                     int node_b, uint32_t pos_b) {
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

// Table cell data
struct TableCell {
    std::wstring text;
    std::vector<TextRun> runs;
    bool is_header = false;
    int align = 0; // 0=left, 1=center, 2=right (from MD_ALIGN)
    ComPtr<IDWriteTextLayout> text_layout;
};

struct TableRow {
    std::vector<TableCell> cells;
    float row_height = 0.0f;
};

struct RenderNode {
    NodeType type = NodeType::Paragraph;
    int heading_level = 0;
    int indent_level = 0;
    int list_number = 0;      // 0 = unordered, >0 = ordered list number
    bool task_checked = false;
    std::wstring text;
    std::vector<TextRun> runs;
    std::wstring anchor_id;   // For headings: GitHub-style slug for internal links
    SyntaxLanguage code_language = SyntaxLanguage::None;
    std::vector<SyntaxToken> syntax_tokens;

    // Table data (only used when type == Table)
    std::vector<TableRow> table_rows;
    std::vector<float> col_widths; // computed column widths

    // Layout cache
    float y_position = 0.0f;
    float height = 0.0f;
    ComPtr<IDWriteTextLayout> text_layout;
    bool layout_dirty = true;
    bool effects_applied = false;

    // Cached inline code background rects (relative to text layout origin)
    struct InlineCodeBg {
        float left, top, width, height;
    };
    std::vector<InlineCodeBg> inline_code_bgs;
};
