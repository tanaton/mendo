#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <algorithm>
#include "syntax.h"
#include "string_convert.h"

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
    int16_t link_url_index = -1; // -1 = リンクなし, >= 0 = Node::link_urls へのインデックス

    constexpr bool bold() const noexcept { return flags & BOLD; }
    constexpr bool italic() const noexcept { return flags & ITALIC; }
    constexpr bool code() const noexcept { return flags & CODE; }
    constexpr bool strikethrough() const noexcept { return flags & STRIKETHROUGH; }
    constexpr bool has_link() const noexcept { return link_url_index >= 0; }

    constexpr void set_bold(bool v) noexcept { set_flag(BOLD, v); }
    constexpr void set_italic(bool v) noexcept { set_flag(ITALIC, v); }
    constexpr void set_code(bool v) noexcept { set_flag(CODE, v); }
    constexpr void set_strikethrough(bool v) noexcept { set_flag(STRIKETHROUGH, v); }

private:
    static constexpr uint8_t BOLD = 0x01;
    static constexpr uint8_t ITALIC = 0x02;
    static constexpr uint8_t CODE = 0x04;
    static constexpr uint8_t STRIKETHROUGH = 0x08;

    constexpr void set_flag(uint8_t mask, bool v) noexcept
    {
        flags = v ? (flags | mask) : static_cast<uint8_t>(flags & ~mask);
    }

    uint8_t flags = 0;
};

// テキスト選択: 位置は (node_index, char_offset) のペアで表す。
// "start" はドキュメント順で常に "end" 以下。
struct TextSelection {
    int start_node = -1;
    uint32_t start_pos = 0;
    int end_node = -1;
    uint32_t end_pos = 0;
    bool active = false;

    constexpr void Clear() noexcept
    {
        start_node = -1;
        end_node = -1;
        active = false;
    }

    // アンカー/キャレットをドキュメント順で start <= end に正規化する
    static constexpr TextSelection MakeOrdered(int node_a, uint32_t pos_a, int node_b, uint32_t pos_b) noexcept
    {
        TextSelection s;
        if (node_a < node_b || (node_a == node_b && pos_a <= pos_b)) {
            s.start_node = node_a;
            s.start_pos = pos_a;
            s.end_node = node_b;
            s.end_pos = pos_b;
        }
        else {
            s.start_node = node_b;
            s.start_pos = pos_b;
            s.end_node = node_a;
            s.end_pos = pos_a;
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
    int align = 0; // MD_ALIGN: 0=DEFAULT, 1=LEFT, 2=CENTER, 3=RIGHT
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
    mutable std::pmr::string text_utf8; // テキスト主記憶（UTF-8、パーサーが設定。GetText()後に解放される場合あり）
    std::pmr::vector<TextRun> runs;

    // runs および table_data->rows 内の TextRun::link_url_index が参照するリンクURLプール
    std::pmr::vector<std::pmr::wstring> link_urls;

    // テーブルデータ（type == Table の場合のみ確保、それ以外は nullptr）
    std::unique_ptr<NodeTableData> table_data;

    // 画像データ（type == Image の場合のみ確保、それ以外は nullptr）
    std::unique_ptr<NodeImageData> image_data;

    // 見出しデータ（type == Heading の場合のみ確保、それ以外は nullptr）
    std::unique_ptr<NodeHeadingData> heading_data;

    // コードブロックデータ（type == CodeBlock の場合のみ確保、それ以外は nullptr）
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

    // テキストが存在するか（遅延変換を発火させずに両表現をチェック）
    bool HasText() const noexcept
    {
        return !text_utf8.empty() || (text_valid_ && !text_.empty());
    }

    // 明示的なUTF-8→Wide変換。パース完了後に一括呼び出しすることで、
    // 描画時の暗黙的な変換とメモリ解放を排除する。
    // CodeBlock以外ではtext_utf8を解放（Mermaidハッシュ計算で直接参照するCodeBlockは保持）。
    void EnsureTextConverted() const
    {
        if (!text_valid_) {
            if (!text_utf8.empty()) {
                string_convert::Utf8ToWide(text_utf8, text_);
                if (type != NodeType::CodeBlock) {
                    text_utf8.clear();
                }
            }
            else {
                text_.clear();
            }
            text_valid_ = true;
        }
    }

    // テキスト取得。EnsureTextConverted() が事前に呼ばれていれば O(1) で返る。
    const std::pmr::wstring& GetText() const
    {
        EnsureTextConverted();
        return text_;
    }

    // const wchar_t* からの呼び出し（リテラル文字列等）をオーバーロード解決で曖昧にしないための委譲
    void SetText(const wchar_t* s) { SetText(std::wstring_view{ s }); }

    // Wideテキストを直接設定
    void SetText(std::wstring_view s)
    {
        text_.assign(s);
        FinalizeSetText();
    }

    // Wideテキストをムーブで設定（コピー回避）
    void SetText(std::pmr::wstring&& s) noexcept
    {
        text_ = std::move(s);
        FinalizeSetText();
    }

    // テーブル行への便利アクセサ
    std::pmr::vector<TableRow>& table_rows() noexcept { return table_data->rows; }
    const std::pmr::vector<TableRow>& table_rows() const noexcept { return table_data->rows; }
    void ensure_table()
    {
        if (!table_data) {
            table_data = std::make_unique<NodeTableData>();
        }
    }
    bool has_table() const noexcept { return table_data != nullptr; }

    // 画像への便利アクセサ
    void ensure_image()
    {
        if (!image_data) {
            image_data = std::make_unique<NodeImageData>();
        }
    }
    bool has_image() const noexcept { return image_data != nullptr; }

    // 見出しへの便利アクセサ
    void ensure_heading()
    {
        if (!heading_data) {
            heading_data = std::make_unique<NodeHeadingData>();
        }
    }
    bool has_heading() const noexcept { return heading_data != nullptr; }
    // アンカーIDへの安全アクセス。Headingでなければ空文字列を返す。
    // 注: 戻り値は heading_data のライフタイムに依存する。
    std::wstring_view anchor_id() const noexcept
    {
        return heading_data ? std::wstring_view(heading_data->anchor_id) : std::wstring_view{};
    }

    // コードブロックへの便利アクセサ
    void ensure_code()
    {
        if (!code_data) {
            code_data = std::make_unique<NodeCodeData>();
        }
    }
    bool has_code() const noexcept { return code_data != nullptr; }
    const std::pmr::vector<SyntaxToken>& syntax_tokens() const noexcept
    {
        // code_data が nullptr の場合のみ呼ばれる想定だが、安全のためガード
        if (code_data) {
            return code_data->syntax_tokens;
        }
        // 空のベクターを返す（CodeBlock以外では syntax_tokens() は呼ばれない設計）
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
        text_valid_ = true;
        text_utf8.clear();
        line_count = static_cast<int>(std::ranges::count(text_, L'\n'));
    }

    mutable std::pmr::wstring text_;    // Wideキャッシュ（GetText()で遅延変換）
    mutable bool text_valid_ = false;   // text_ が最新ならtrue
};
