#pragma once
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <algorithm>
#include <utility>
#include <variant>
#include "pmr_unique_ptr.h"
#include "small_vector.h"
#include "syntax.h"
#include "text_types.h"
#include "utility.h"

// Node::runs / TableCell::runs の型エイリアス。SBO=4 で大半のノード (run < 4 が中央値)
// で動的確保ゼロ。SBO を超えた場合のみ操作 new_delete に確保する (synchronized_pool を
// 経由しないため、parse 時のロック取得を Node 数だけ削減できる)。
using TextRunList = mendo::small_vector<TextRun, 4>;

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

struct TableCell {
    std::pmr::wstring text;
    TextRunList runs;
    bool is_header = false;
    TableAlign align = TableAlign::Default;
};

struct TableRow {
    std::pmr::vector<TableCell> cells;
};

// テーブル専用データ（Tableノードのみ確保）
struct NodeTableData {
    std::pmr::vector<TableRow> rows;
    // パーサがセル追加時に維持する全行最大列数。MeasureTable の全行走査を省く。
    uint16_t col_count = 0;
};

struct NodeHeadingData {
    std::pmr::wstring anchor_id; // 内部リンク向けGitHubスタイルのスラグ
};

struct NodeCodeData {
    std::pmr::vector<SyntaxToken> syntax_tokens;
};

struct NodeImageData {
    std::pmr::wstring src; // 画像ソースパス（Markdown内の記述）
    float width = 0.0f;    // 元画像の幅（ピクセル）
    float height = 0.0f;   // 元画像の高さ（ピクセル）
};

// Node::source_offset が未設定であることを示すセンチネル値。
// HorizontalRule のようなテキストを持たないノードはオフセットを記録しない。
inline constexpr uint32_t kUnsetSourceOffset = std::numeric_limits<uint32_t>::max();

struct Node {
    TextRunList runs;

    // Table と Image のデータは Node 全体の少数派 (画像/表のあるノードのみ) なので、
    // variant に詰め込むと最大 alternative である NodeImageData が全 Node の sizeof を支配する。
    // pmr_unique_ptr で外出しすると variant 上限が下がり、持たないノードは null pointer の
    // オーバーヘッドだけで済む。PmrDefaultDeleter (ステートレス) が pmr default_resource に
    // 自動返却するため Node 自身は copy/move/dtor の手書き不要 (= スマートポインタメンバの
    // 存在で Node は move-only になる)。
    // Parser invariant: type == NodeType::Table のとき table_ != nullptr。MeasureTable や
    // HitTestTable は type 判定だけで table_data() を参照するため、parser はテーブル開始時に
    // 必ず ensure_table() を呼ぶこと。
    mendo::pmr_unique_ptr<NodeTableData> table_;
    mendo::pmr_unique_ptr<NodeImageData> image_;

    // リンク URL の集合。リンクを含むノードでのみ確保される。
    // Why: `Node::runs` が link_url_index で参照する URL テーブル。リンクを持つノードは
    // 全体の少数派 (見出し/段落の一部) なので、空時は 8B ポインタ 1 本に抑える。
    mendo::pmr_unique_ptr<std::pmr::vector<std::pmr::wstring>> link_urls_;

    // ノード種別ごとの拡張データ。Heading/Code のみ variant に格納 (上限 24B + tag 8B = 32B)。
    using Extra = std::variant<std::monostate, NodeHeadingData, NodeCodeData>;
    Extra extra;

    // 加工テキスト (Alert / HTML entity 解決後 / SOFTBR/BR の置換を含む / display math 昇格 / 表セル等)
    // を保持する owned バッファ。view モード時 (= raw_wide_ の連続範囲をそのまま表示できる場合) は空。
    // raw_wide_ への view と用途を分離することで、テキスト加工が無いノードは Document の raw_wide_ を
    // 共有 view し、加工があるノードのみ owned の中身を持つ。
    // inline 保持にすることで wstring の SBO (~16 wchar) が活き、短い owned テキストは 0 ヒープ確保で済む。
    // 不変条件: view モード時 (view_length > 0) は owned_text_.empty()。
    std::pmr::wstring owned_text_;

    // view モード時の参照ベース (= Document::raw_wide_.data())。
    // ReplaceContent / move 時に Document::InjectViewBase() が一括で書き込む。
    // owned モード時は不問 (GetText が owned_text_ を優先するため)。
    const wchar_t* view_base_ = nullptr;

    // --- 4 バイトアライメント ---
    int32_t list_number = 0;                     // 0 = 順序なし, >0 = 順序付きリスト番号
    uint32_t alert_label_length = 0;             // ラベル部分の文字数（描画エフェクト適用範囲）
    uint32_t source_offset = kUnsetSourceOffset; // ソース wide テキスト内の UTF-16 コード単位オフセット (view モード時は view 開始位置)
    int32_t blockquote_group = -1;               // 最外側 blockquote 単位のグループID（ネストしてもgroupは共有）
    int32_t line_count = 0;                      // テキスト内の改行数（パース時にカウント済み）
    uint32_t view_length = 0;                    // view モード時の長さ (UTF-16 wchar 単位)。owned モード時は 0。

    // --- 1 バイトアライメント ---
    NodeType type = NodeType::Paragraph;
    bool task_checked = false;
    AlertType alert_type = AlertType::None;
    SyntaxLanguage code_language = SyntaxLanguage::None;
    int8_t quote_depth = 0;        // 現在の blockquote ネスト深さ（0 = 引用外, 1.. = ネストレベル）
    int8_t quote_outer_indent = 0; // 最外側 blockquote が居る indent_level（バー位置の起点）
    int8_t heading_level = 0;      // 1〜6（0 = 見出しでない）
    int8_t indent_level = 0;       // リスト/引用のネスト深さ（int8_t の最大値で飽和）

    constexpr bool HasText() const noexcept
    {
        // view モードを優先判定: 22000 ノード規模で view モードが多数派になるパスのため、
        // 単一比較で早抜けする方が分岐予測に優しい。owned とは排他的不変条件。
        if (view_length > 0) {
            return true;
        }
        return !owned_text_.empty();
    }

    // 戻り値は wstring_view。view モード優先で raw_wide_ の view を返し、
    // owned モードでは owned_text_ をそのまま view に変換する。
    // view_base_ は Document::InjectViewBase() で注入される前提。
    constexpr std::wstring_view GetText() const noexcept
    {
        if (view_length > 0 && view_base_ != nullptr) {
            return { view_base_ + source_offset, view_length };
        }
        return owned_text_;
    }

    void SetText(const wchar_t* s)
    {
        SetText(std::wstring_view{ s });
    }

    void SetText(std::wstring_view s)
    {
        owned_text_.assign(s);
        FinalizeOwnedTextLineCount();
    }

    void SetText(std::pmr::wstring&& s) noexcept
    {
        owned_text_ = std::move(s);
        FinalizeOwnedTextLineCount();
    }

    // 加工テキスト (Alert/HTML entity/SOFTBR/BR/DisplayMath) 用の owned バッファ設定 API。
    // 呼び出し側が改行数を積算済みのバリアント。
    void SetTextOwned(std::pmr::wstring s, int32_t line_count_value)
    {
        owned_text_ = std::move(s);
        view_length = 0;
        line_count = line_count_value;
    }

    // 連続 NORMAL/CODE/LATEXMATH のみで構成されるノード向けの view 設定 API。
    // raw_wide_ の (source_offset_value, length) 範囲をそのまま表示テキストとして使う。
    // view_base は markdown_base / raw_wide_.data()。Document::InjectViewBase() の手前で
    // GetText() を呼ぶ可能性のあるパス (parser 中の Heading anchor 生成等) のため、
    // この場で同時に設定する。Document の move 後は InjectViewBase() で再注入される。
    void SetTextView(uint32_t source_offset_value, uint32_t length, int32_t line_count_value, const wchar_t* view_base) noexcept
    {
        owned_text_.clear();
        source_offset = source_offset_value;
        view_length = length;
        line_count = line_count_value;
        view_base_ = view_base;
    }

    // text と line_count を一括更新する（呼び出し側が積算済みの line_count を渡す）。
    // 改行を逐次カウントしておけるパーサ向けの最適化バリアント。
    void SetTextWithLineCount(std::wstring_view s, int32_t line_count_value)
    {
        owned_text_.assign(s);
        view_length = 0;
        line_count = line_count_value;
    }

    void SetTextWithLineCount(std::pmr::wstring&& s, int32_t line_count_value) noexcept
    {
        owned_text_ = std::move(s);
        view_length = 0;
        line_count = line_count_value;
    }

    // get_if 風に *_data() でポインタを返す。所持していなければ nullptr。
    constexpr NodeTableData* table_data() noexcept
    {
        return table_.get();
    }
    constexpr const NodeTableData* table_data() const noexcept
    {
        return table_.get();
    }
    constexpr NodeImageData* image_data() noexcept
    {
        return image_.get();
    }
    constexpr const NodeImageData* image_data() const noexcept
    {
        return image_.get();
    }
    constexpr NodeHeadingData* heading_data() noexcept
    {
        return std::get_if<NodeHeadingData>(&extra);
    }
    constexpr const NodeHeadingData* heading_data() const noexcept
    {
        return std::get_if<NodeHeadingData>(&extra);
    }
    constexpr NodeCodeData* code_data() noexcept
    {
        return std::get_if<NodeCodeData>(&extra);
    }
    constexpr const NodeCodeData* code_data() const noexcept
    {
        return std::get_if<NodeCodeData>(&extra);
    }

    constexpr bool has_table() const noexcept
    {
        return static_cast<bool>(table_);
    }
    constexpr bool has_image() const noexcept
    {
        return static_cast<bool>(image_);
    }
    constexpr bool has_heading() const noexcept
    {
        return std::holds_alternative<NodeHeadingData>(extra);
    }
    constexpr bool has_code() const noexcept
    {
        return std::holds_alternative<NodeCodeData>(extra);
    }

    // ensure_*: 既に存在すれば内部参照、無ければ新規確保して返す。
    constexpr NodeTableData* ensure_table()
    {
        if (!table_) {
            table_ = mendo::MakePmrUnique<NodeTableData>();
        }
        return table_.get();
    }
    constexpr NodeImageData* ensure_image()
    {
        if (!image_) {
            image_ = mendo::MakePmrUnique<NodeImageData>();
        }
        return image_.get();
    }
    constexpr NodeHeadingData* ensure_heading()
    {
        if (!has_heading()) {
            return &extra.emplace<NodeHeadingData>();
        }
        return std::get_if<NodeHeadingData>(&extra);
    }
    constexpr NodeCodeData* ensure_code()
    {
        if (!has_code()) {
            return &extra.emplace<NodeCodeData>();
        }
        return std::get_if<NodeCodeData>(&extra);
    }

    constexpr std::pmr::vector<TableRow>& table_rows() noexcept
    {
        return table_data()->rows;
    }
    constexpr const std::pmr::vector<TableRow>& table_rows() const noexcept
    {
        return table_data()->rows;
    }

    constexpr std::wstring_view anchor_id() const noexcept
    {
        const auto* hd = heading_data();
        return hd ? std::wstring_view{ hd->anchor_id } : std::wstring_view{};
    }

    constexpr std::pmr::vector<std::pmr::wstring>& ensure_link_urls()
    {
        if (!link_urls_) {
            link_urls_ = mendo::MakePmrUnique<std::pmr::vector<std::pmr::wstring>>();
        }
        return *link_urls_;
    }
    constexpr std::span<const std::pmr::wstring> view_link_urls() const noexcept
    {
        return SpanOrEmpty(link_urls_);
    }

    const std::pmr::vector<SyntaxToken>& syntax_tokens() const noexcept
    {
        if (const auto* cd = code_data()) {
            return cd->syntax_tokens;
        }
        static const std::pmr::vector<SyntaxToken> empty;
        return empty;
    }
    constexpr std::pmr::vector<SyntaxToken>& syntax_tokens_mut() noexcept
    {
        return ensure_code()->syntax_tokens;
    }

private:
    void FinalizeOwnedTextLineCount() noexcept
    {
        line_count = static_cast<int32_t>(std::ranges::count(owned_text_, L'\n'));
        view_length = 0;
    }
};
