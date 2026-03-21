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

// テキスト選択: 位置は (node_index, char_offset) のペアで表す。
// "start" はドキュメント順で常に "end" 以下。
struct TextSelection {
    int start_node = -1;
    uint32_t start_pos = 0;
    int end_node = -1;
    uint32_t end_pos = 0;
    bool active = false;

    constexpr void Clear() noexcept { start_node = end_node = -1; active = false; }

    // アンカー/キャレットをドキュメント順で start <= end に正規化する
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

// テーブルセルデータ（純粋なドメインデータ — レイアウトキャッシュなし）
struct TableCell {
    std::pmr::wstring text;
    std::pmr::vector<TextRun> runs;
    bool is_header = false;
    int align = 0; // 0=左寄せ, 1=中央寄せ, 2=右寄せ (MD_ALIGN由来)
};

struct TableRow {
    std::pmr::vector<TableCell> cells;
};

struct Node {
    NodeType type = NodeType::Paragraph;
    int heading_level = 0;
    int indent_level = 0;
    int list_number = 0;      // 0 = 順序なし, >0 = 順序付きリスト番号
    bool task_checked = false;
    std::pmr::wstring text;
    std::pmr::vector<TextRun> runs;
    std::pmr::wstring anchor_id;   // 見出し用: 内部リンク向けGitHubスタイルのスラグ
    SyntaxLanguage code_language = SyntaxLanguage::None;
    std::pmr::vector<SyntaxToken> syntax_tokens;

    // テーブルデータ（type == Table の場合のみ使用）
    std::pmr::vector<TableRow> table_rows;
};
