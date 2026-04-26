#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <algorithm>
#include <utility>
#include <variant>
#include "syntax.h"
#include "string_convert.h"
#include "text_types.h"

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

static_assert(std::to_underlying(AlertType::None) == 0, "AlertType::None must be 0");
static_assert(std::to_underlying(AlertType::Note) == 1, "AlertType::Note must be 1");
static_assert(std::to_underlying(AlertType::Caution) == 5, "AlertType::Caution must be 5");

inline constexpr size_t ALERT_TYPE_COUNT = 5;

constexpr size_t AlertColorIndex(AlertType t) noexcept
{
    if (t == AlertType::None) {
        return ALERT_TYPE_COUNT;
    }
    return static_cast<size_t>(std::to_underlying(t)) - 1;
}

// md4c の MD_ALIGN と値を一致させる（0=DEFAULT, 1=LEFT, 2=CENTER, 3=RIGHT）
enum class TableAlign : uint8_t {
    Default = 0,
    Left = 1,
    Center = 2,
    Right = 3,
};

// テーブルセルデータ（純粋なドメインデータ — レイアウトキャッシュなし）
struct TableCell {
    std::pmr::wstring text;
    std::pmr::vector<TextRun> runs;
    bool is_header = false;
    TableAlign align = TableAlign::Default;
};

struct TableRow {
    std::pmr::vector<TableCell> cells;
};

// テーブル専用データ（Tableノードのみ確保）
struct NodeTableData {
    std::pmr::vector<TableRow> rows;
};

// 見出し専用データ（Headingノードのみ確保してメモリを節約）
struct NodeHeadingData {
    std::pmr::wstring anchor_id; // 内部リンク向けGitHubスタイルのスラグ
};

// コードブロック専用データ（CodeBlockノードのみ確保してメモリを節約）
struct NodeCodeData {
    std::pmr::vector<SyntaxToken> syntax_tokens;
};

// 画像専用データ（Imageノードのみ確保）
struct NodeImageData {
    std::pmr::wstring src;     // 画像ソースパス（Markdown内の記述）
    float width = 0.0f;        // 元画像の幅（ピクセル）
    float height = 0.0f;       // 元画像の高さ（ピクセル）
};

struct Node {
    std::pmr::vector<TextRun> runs;

    std::pmr::vector<std::pmr::wstring> link_urls;

    // ノード種別ごとの拡張データ。同時に持てるのは 1 種類のみ。
    // Why: 旧設計では 4 つの unique_ptr を常に保持していたため、ほとんどのノード
    // (Paragraph/ListItem 等で 87% を占める) で 32B のポインタ群が null のまま
    // 死蔵していた。variant に統合することで、データを持つノードでは別ヒープ
    // への malloc を 1 回省略でき、断片化と allocator オーバーヘッドが減る。
    using Extra = std::variant<std::monostate, NodeTableData, NodeImageData, NodeHeadingData, NodeCodeData>;
    Extra extra;

    int heading_level = 0;
    int indent_level = 0;
    int list_number = 0;      // 0 = 順序なし, >0 = 順序付きリスト番号
    uint32_t alert_label_length = 0; // ラベル部分の文字数（描画エフェクト適用範囲）
    uint32_t source_offset = UINT32_MAX; // ソースUTF-8内のバイトオフセット（未設定時UINT32_MAX）
    int blockquote_group = -1;       // 同一 MD_BLOCK_QUOTE 内のノードを識別するグループID
    int line_count = 0;              // テキスト内の改行数（パース時にカウント済み）

    NodeType type = NodeType::Paragraph;
    bool task_checked = false;
    AlertType alert_type = AlertType::None;
    SyntaxLanguage code_language = SyntaxLanguage::None;

    bool HasText() const noexcept { return !text_.empty(); }

    const std::pmr::wstring& GetText() const noexcept { return text_; }

    void SetText(const wchar_t* s) { SetText(std::wstring_view{ s }); }

    void SetText(std::wstring_view s)
    {
        text_.assign(s);
        FinalizeSetText();
    }

    void SetText(std::pmr::wstring&& s) noexcept
    {
        text_ = std::move(s);
        FinalizeSetText();
    }

    // ---- 拡張データへのアクセサ ----
    // get_if 風に *_data() でポインタを返す。所持していなければ nullptr。
    NodeTableData* table_data() noexcept { return std::get_if<NodeTableData>(&extra); }
    const NodeTableData* table_data() const noexcept { return std::get_if<NodeTableData>(&extra); }
    NodeImageData* image_data() noexcept { return std::get_if<NodeImageData>(&extra); }
    const NodeImageData* image_data() const noexcept { return std::get_if<NodeImageData>(&extra); }
    NodeHeadingData* heading_data() noexcept { return std::get_if<NodeHeadingData>(&extra); }
    const NodeHeadingData* heading_data() const noexcept { return std::get_if<NodeHeadingData>(&extra); }
    NodeCodeData* code_data() noexcept { return std::get_if<NodeCodeData>(&extra); }
    const NodeCodeData* code_data() const noexcept { return std::get_if<NodeCodeData>(&extra); }

    bool has_table() const noexcept { return std::holds_alternative<NodeTableData>(extra); }
    bool has_image() const noexcept { return std::holds_alternative<NodeImageData>(extra); }
    bool has_heading() const noexcept { return std::holds_alternative<NodeHeadingData>(extra); }
    bool has_code() const noexcept { return std::holds_alternative<NodeCodeData>(extra); }

    // ensure_*: 既に同じ型を持っていれば内部参照を、違う型を持っていれば差し替えて新規確保した参照を返す。
    NodeTableData* ensure_table()
    {
        if (!has_table()) {
            return &extra.emplace<NodeTableData>();
        }
        return std::get_if<NodeTableData>(&extra);
    }
    NodeImageData* ensure_image()
    {
        if (!has_image()) {
            return &extra.emplace<NodeImageData>();
        }
        return std::get_if<NodeImageData>(&extra);
    }
    NodeHeadingData* ensure_heading()
    {
        if (!has_heading()) {
            return &extra.emplace<NodeHeadingData>();
        }
        return std::get_if<NodeHeadingData>(&extra);
    }
    NodeCodeData* ensure_code()
    {
        if (!has_code()) {
            return &extra.emplace<NodeCodeData>();
        }
        return std::get_if<NodeCodeData>(&extra);
    }

    std::pmr::vector<TableRow>& table_rows() noexcept { return table_data()->rows; }
    const std::pmr::vector<TableRow>& table_rows() const noexcept { return table_data()->rows; }

    std::wstring_view anchor_id() const noexcept
    {
        const auto* hd = heading_data();
        return hd ? std::wstring_view{ hd->anchor_id } : std::wstring_view{};
    }

    const std::pmr::vector<SyntaxToken>& syntax_tokens() const noexcept
    {
        if (const auto* cd = code_data()) {
            return cd->syntax_tokens;
        }
        static const std::pmr::vector<SyntaxToken> empty;
        return empty;
    }
    std::pmr::vector<SyntaxToken>& syntax_tokens_mut()
    {
        return ensure_code()->syntax_tokens;
    }

private:
    void FinalizeSetText() noexcept
    {
        line_count = static_cast<int>(std::ranges::count(text_, L'\n'));
    }

    std::pmr::wstring text_;    // Wide テキスト
};
