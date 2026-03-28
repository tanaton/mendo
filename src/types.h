#pragma once
#include <string>
#include <vector>
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
    TaskListItem,
    Image
};

enum class AlertType : uint8_t {
    None,
    Note,
    Tip,
    Important,
    Warning,
    Caution
};

static_assert(static_cast<size_t>(AlertType::None) == 0, "AlertType::None must be 0");
static_assert(static_cast<size_t>(AlertType::Note) == 1, "AlertType::Note must be 1");
static_assert(static_cast<size_t>(AlertType::Caution) == 5, "AlertType::Caution must be 5");

inline constexpr size_t ALERT_TYPE_COUNT = 5;

// AlertType → 0ベースの色/ブラシインデックス。None の場合は ALERT_TYPE_COUNT を返す（範囲外）。
constexpr size_t AlertColorIndex(AlertType t) noexcept
{
    if (t == AlertType::None) {
        return ALERT_TYPE_COUNT;
    }
    return static_cast<size_t>(t) - 1;
}

struct TextRun {
    uint32_t start = 0;
    uint32_t length = 0;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strikethrough = false;
    int16_t link_url_index = -1; // -1 = リンクなし, >= 0 = Node::link_urls へのインデックス

    constexpr bool has_link() const noexcept { return link_url_index >= 0; }
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
        int node_b, uint32_t pos_b) noexcept
    {
        TextSelection s;
        if (node_a < node_b || (node_a == node_b && pos_a <= pos_b)) {
            s.start_node = node_a; s.start_pos = pos_a;
            s.end_node = node_b;   s.end_pos = pos_b;
        }
        else {
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
    AlertType alert_type = AlertType::None;
    uint32_t alert_label_length = 0; // ラベル部分の文字数（描画エフェクト適用範囲）
    int blockquote_group = -1;       // 同一 MD_BLOCK_QUOTE 内のノードを識別するグループID
    std::pmr::wstring text;
    std::pmr::vector<TextRun> runs;
    std::pmr::wstring anchor_id;   // 見出し用: 内部リンク向けGitHubスタイルのスラグ
    SyntaxLanguage code_language = SyntaxLanguage::None;
    std::pmr::vector<SyntaxToken> syntax_tokens;

    // runs および table_rows 内の TextRun::link_url_index が参照するリンクURLプール
    std::pmr::vector<std::pmr::wstring> link_urls;

    // テーブルデータ（type == Table の場合のみ使用）
    std::pmr::vector<TableRow> table_rows;

    // 画像データ（type == Image の場合のみ使用）
    std::pmr::wstring image_src;    // 画像ソースパス（Markdown内の記述）
    float image_width = 0.0f;       // 元画像の幅（ピクセル）
    float image_height = 0.0f;      // 元画像の高さ（ピクセル）
};
