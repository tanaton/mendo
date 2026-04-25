#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <algorithm>
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

static_assert(static_cast<size_t>(AlertType::None) == 0, "AlertType::None must be 0");
static_assert(static_cast<size_t>(AlertType::Note) == 1, "AlertType::Note must be 1");
static_assert(static_cast<size_t>(AlertType::Caution) == 5, "AlertType::Caution must be 5");

inline constexpr size_t ALERT_TYPE_COUNT = 5;

constexpr size_t AlertColorIndex(AlertType t) noexcept
{
    if (t == AlertType::None) {
        return ALERT_TYPE_COUNT;
    }
    return static_cast<size_t>(t) - 1;
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
    // パース中の md4c コールバック専用の UTF-8 蓄積領域。
    // ConvertTextFromUtf8() 後は空である契約 — 下流は text_ のみを読む。
    std::pmr::string text_utf8;
    std::pmr::vector<TextRun> runs;

    std::pmr::vector<std::pmr::wstring> link_urls;
    std::unique_ptr<NodeTableData> table_data;
    std::unique_ptr<NodeImageData> image_data;
    std::unique_ptr<NodeHeadingData> heading_data;
    std::unique_ptr<NodeCodeData> code_data;

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

    // UTF-8→Wide 変換。ParseMarkdown 末尾で全ノードに一括呼び出しされる。
    // Node を手動構築する経路（テスト等）では個別に呼ぶ。
    void ConvertTextFromUtf8()
    {
        if (text_utf8.empty()) {
            return;
        }
        string_convert::Utf8ToWide(text_utf8, text_);
        text_utf8.clear();
    }

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

    std::pmr::vector<TableRow>& table_rows() noexcept { return table_data->rows; }
    const std::pmr::vector<TableRow>& table_rows() const noexcept { return table_data->rows; }
    void ensure_table()
    {
        if (!table_data) {
            table_data = std::make_unique<NodeTableData>();
        }
    }
    bool has_table() const noexcept { return table_data != nullptr; }

    void ensure_image()
    {
        if (!image_data) {
            image_data = std::make_unique<NodeImageData>();
        }
    }
    bool has_image() const noexcept { return image_data != nullptr; }

    void ensure_heading()
    {
        if (!heading_data) {
            heading_data = std::make_unique<NodeHeadingData>();
        }
    }
    bool has_heading() const noexcept { return heading_data != nullptr; }

    std::wstring_view anchor_id() const noexcept
    {
        return heading_data ? std::wstring_view(heading_data->anchor_id) : std::wstring_view{};
    }

    void ensure_code()
    {
        if (!code_data) {
            code_data = std::make_unique<NodeCodeData>();
        }
    }

    bool has_code() const noexcept { return code_data != nullptr; }
    const std::pmr::vector<SyntaxToken>& syntax_tokens() const noexcept
    {
        if (code_data) {
            return code_data->syntax_tokens;
        }
        static const std::pmr::vector<SyntaxToken> empty;
        return empty;
    }
    std::pmr::vector<SyntaxToken>& syntax_tokens_mut()
    {
        ensure_code();
        return code_data->syntax_tokens;
    }

private:
    void FinalizeSetText() noexcept
    {
        text_utf8.clear();
        line_count = static_cast<int>(std::ranges::count(text_, L'\n'));
    }

    std::pmr::wstring text_;    // Wide テキスト（ConvertTextFromUtf8 / SetText 後に有効）
};
