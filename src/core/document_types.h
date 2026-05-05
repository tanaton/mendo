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
#include "doc_text.h"
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

// 区切り '\t'/'\n' は次セル開始時に挿入されるので concat 末尾に到達したセル
// (= 最終セル / 末尾の padding セル群) は区切りを持たない。short row 等で座標判定が
// 効かないため「end が concat 全体末尾と一致するか」で末尾セルを判定する。
constexpr uint32_t CellLengthFromOffsets(uint32_t start, uint32_t end, uint32_t concat_size) noexcept
{
    return (end - start) - (end == concat_size ? 0u : 1u);
}

// テーブル専用データ（Tableノードのみ確保）。SoA レイアウトで全セルを 1 本の concat_text に連結保持し、
// セルごとのアクセスは offset テーブル経由で O(1)。
// 不変条件:
//   cell_text_starts.size() == cell_run_starts.size() == row_count * col_count + 1
//   cell_text_starts.back() == concat_text.size()
//   cell_run_starts.back() == all_runs.size()
struct NodeTableData {
    // 全セルテキストを linearized 形式で連結 ("cell\tcell\ncell\tcell")。
    mendo::doc_string concat_text;
    // 全セルの TextRun を連結。run.start は cell-local offset (layout は cell スライス単位で作成されるため)。
    std::pmr::vector<TextRun> all_runs;
    // 各セルの concat_text 内開始 offset。番兵末尾 = concat_text.size()。
    std::pmr::vector<uint32_t> cell_text_starts;
    // 各セルの all_runs 内開始 offset。番兵末尾 = all_runs.size()。
    std::pmr::vector<uint32_t> cell_run_starts;
    // 列単位の align (GitHub Markdown では align は列ごと)
    std::pmr::vector<TableAlign> aligns;
    // 行単位の header フラグ (md4c は TR 内で TH/TD が混在しない契約)
    std::pmr::vector<bool> is_header_row;

    uint16_t row_count = 0;
    uint16_t col_count = 0;

    constexpr size_t CellIndex(size_t r, size_t c) const noexcept
    {
        return r * static_cast<size_t>(col_count) + c;
    }

    // セルの concat_text 内開始 / 終了 offset。終端は次セル開始 (= 末尾区切り '\t'/'\n' を含む)。
    constexpr uint32_t CellTextStart(size_t r, size_t c) const noexcept
    {
        return cell_text_starts[CellIndex(r, c)];
    }
    constexpr uint32_t CellTextEnd(size_t r, size_t c) const noexcept
    {
        return cell_text_starts[CellIndex(r, c) + 1];
    }

    constexpr mendo::doc_string_view GetCellText(size_t r, size_t c) const noexcept
    {
        const size_t idx = CellIndex(r, c);
        const auto start = cell_text_starts[idx];
        const auto end = cell_text_starts[idx + 1];
        const auto len = CellLengthFromOffsets(start, end, static_cast<uint32_t>(concat_text.size()));
        return { concat_text.data() + start, len };
    }

    constexpr std::span<const TextRun> GetCellRuns(size_t r, size_t c) const noexcept
    {
        const size_t idx = CellIndex(r, c);
        const auto start = cell_run_starts[idx];
        const auto end = cell_run_starts[idx + 1];
        return { all_runs.data() + start, static_cast<size_t>(end - start) };
    }

    constexpr bool IsHeaderRow(size_t r) const noexcept
    {
        return is_header_row[r];
    }

    constexpr TableAlign ColAlign(size_t c) const noexcept
    {
        return aligns[c];
    }
};

struct NodeHeadingData {
    mendo::doc_string anchor_id; // 内部リンク向けGitHubスタイルのスラグ
};

struct NodeCodeData {
    std::pmr::vector<SyntaxToken> syntax_tokens;
};

struct NodeImageData {
    mendo::doc_string src; // 画像ソースパス（Markdown内の記述）
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
    mendo::pmr_unique_ptr<std::pmr::vector<mendo::doc_string>> link_urls_;

    // ノード種別ごとの拡張データ。Heading/Code のみ variant に格納 (上限 24B + tag 8B = 32B)。
    using Extra = std::variant<std::monostate, NodeHeadingData, NodeCodeData>;
    Extra extra;

    // 表示テキストの owned バッファ。テキスト加工 (Alert / HTML entity / SOFTBR/BR / display math 昇格 / 表セル等)
    // を必要とするノードのみ確保する。raw_wide_ の連続範囲をそのまま表示できるノードは view モードに倒し
    // owned_text_ は空のまま raw_wide_ を共有 view する (view_length > 0 と排他的不変条件)。
    // SBO (~16 wchar) により短い加工結果は 0 ヒープ確保で済む。
    mendo::doc_string owned_text_;

    // view モード時の参照ベース (= Document::raw_wide_.data())。
    // Document::InjectViewBase() が ReplaceContent / move 時に一括設定する。
    const mendo::doc_char* view_base_ = nullptr;

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

    constexpr bool IsViewMode() const noexcept
    {
        return view_length > 0;
    }

    constexpr bool HasText() const noexcept
    {
        // view モードが多数派 (parse 結果で view ノードが過半) なので先に分岐させる。
        if (IsViewMode()) {
            return true;
        }
        return !owned_text_.empty();
    }

    constexpr mendo::doc_string_view GetText() const noexcept
    {
        if (IsViewMode() && view_base_ != nullptr) {
            return { view_base_ + source_offset, view_length };
        }
        return owned_text_;
    }

    void SetText(const mendo::doc_char* s)
    {
        SetText(mendo::doc_string_view{ s });
    }

    void SetText(mendo::doc_string_view s)
    {
        owned_text_.assign(s);
        FinalizeOwnedTextLineCount();
    }

    void SetText(mendo::doc_string&& s) noexcept
    {
        owned_text_ = std::move(s);
        FinalizeOwnedTextLineCount();
    }

    // 連続 NORMAL/CODE/LATEXMATH のみで構成されるノード向けの view 設定 API。
    // raw_wide_ の (source_offset_value, length) 範囲をそのまま表示テキストとして使う。
    // Document::InjectViewBase() の手前で GetText() を呼ぶパス (parser 中の Heading anchor 生成等)
    // のため view_base もここで同時設定する。move 後は InjectViewBase() で再注入される。
    void SetTextView(uint32_t source_offset_value, uint32_t length, int32_t line_count_value, const mendo::doc_char* view_base) noexcept
    {
        owned_text_.clear();
        source_offset = source_offset_value;
        view_length = length;
        line_count = line_count_value;
        view_base_ = view_base;
    }

    // text と line_count を一括更新する（呼び出し側が積算済みの line_count を渡す）。
    // 改行を逐次カウントしておけるパーサ向けの最適化バリアント。
    void SetTextWithLineCount(mendo::doc_string_view s, int32_t line_count_value)
    {
        owned_text_.assign(s);
        view_length = 0;
        line_count = line_count_value;
    }

    void SetTextWithLineCount(mendo::doc_string&& s, int32_t line_count_value) noexcept
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

    constexpr mendo::doc_string_view anchor_id() const noexcept
    {
        const auto* hd = heading_data();
        return hd ? mendo::doc_string_view{ hd->anchor_id } : mendo::doc_string_view{};
    }

    constexpr std::pmr::vector<mendo::doc_string>& ensure_link_urls()
    {
        if (!link_urls_) {
            link_urls_ = mendo::MakePmrUnique<std::pmr::vector<mendo::doc_string>>();
        }
        return *link_urls_;
    }
    constexpr std::span<const mendo::doc_string> view_link_urls() const noexcept
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
        line_count = static_cast<int32_t>(std::ranges::count(owned_text_, mendo::doc_lf));
        view_length = 0;
        view_base_ = nullptr;
    }
};
