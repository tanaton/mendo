#pragma once
#include <cassert>
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

// Node::runs / TableCell::runs の型エイリアス。SBO=2 で size=1 (実測中央値) のノード 65%
// を完全 inline 保持しつつ、Node 直下の 8B 削減と Paragraph/BlockQuote (中央値 4) の
// heap fallback を許容する設計。example/test.md 実測ヒット率 75.8% (テキスト系のみ)。
// SBO を超えた場合のみ operator new_delete に確保する (synchronized_pool を経由しないため、
// parse 時のロック取得を Node 数だけ削減できる)。
using TextRunList = mendo::small_vector<TextRun, 2>;

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
    std::pmr::string concat_text;
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

    constexpr std::string_view GetCellText(size_t r, size_t c) const noexcept
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

// Heading 固有データ。anchor_id は外部 heap 持ちにすることで variant alternative を 16B に抑える。
struct NodeHeadingData {
    mendo::pmr_unique_ptr<std::pmr::string> anchor_id;
    int8_t heading_level = 0;
};

// CodeBlock 固有データ。tokens は外部 heap 持ちにすることで variant alternative を 16B に抑える。
struct NodeCodeData {
    mendo::pmr_unique_ptr<std::pmr::vector<SyntaxToken>> tokens;
    SyntaxLanguage code_language = SyntaxLanguage::None;
};

struct NodeListData {
    int32_t list_number = 0;   // 0 = 順序なし, >0 = 順序付きリスト番号
    bool task_checked = false; // TaskListItem のときのみ意味を持つ
};

// alert_type は同一 blockquote_group の後続ノードにも伝播するため Node 直下に置く
// (parser.cpp:DetectAlertAt 参照)。alert_label_length は BlockQuote 本体専用なので variant 内。
struct NodeAlertData {
    uint32_t alert_label_length = 0;
};

using NodeTablePtr = mendo::pmr_unique_ptr<NodeTableData>;

struct NodeImageData {
    std::pmr::string src; // 画像ソースパス（Markdown内の記述）
    float width = 0.0f;   // 元画像の幅（ピクセル）
    float height = 0.0f;  // 元画像の高さ（ピクセル）
};
using NodeImagePtr = mendo::pmr_unique_ptr<NodeImageData>;

// Node::SourceOffsetFrom() が未設定 (view_.data() == nullptr) のときに返す値。
// HorizontalRule のようなテキストを持たないノードはオフセットを持たない。
inline constexpr uint32_t kUnsetSourceOffset = std::numeric_limits<uint32_t>::max();

struct Node {
    TextRunList runs;

    // リンク URL の集合。リンクを含むノードでのみ確保される。
    // Why: `Node::runs` が link_url_index で参照する URL テーブル。リンクを持つノードは
    // 全体の少数派 (見出し/段落の一部) なので、空時は 8B ポインタ 1 本に抑える。
    mendo::pmr_unique_ptr<std::pmr::vector<std::pmr::string>> link_urls_;

    // ノード種別ごとの拡張データ。重データ (anchor_id / tokens / table / image) は pmr_unique_ptr で
    // 外出しすることで variant alternative の最大サイズを 16B に抑える。
    // Parser invariant: 1 つのノードは 1 つの alternative しか持たない (ensure_* は他を破壊する)。
    using Extra = std::variant<
        std::monostate,  // Paragraph / HorizontalRule など固有データ無し
        NodeHeadingData, // Heading
        NodeCodeData,    // CodeBlock
        NodeListData,    // ListItem / TaskListItem
        NodeAlertData,   // BlockQuote 本体の alert ラベル
        NodeTablePtr,    // Table
        NodeImagePtr>;   // Image
    Extra extra;

    // 表示テキストの owned バッファ。テキスト加工 (Alert / HTML entity / SOFTBR/BR / display math 昇格 / 表セル等)
    // を必要とするノードのみ確保する。raw_text_ の連続範囲をそのまま表示できるノードは view モードに倒し
    // owned_text_ は空のまま raw_text_ を共有 view する (view_.size() > 0 と排他的不変条件)。
    // SBO (~16 byte) により短い加工結果は 0 ヒープ確保で済む。
    std::pmr::string owned_text_;

    // raw_text_ 内のソース位置 + 表示モードを 1 本に統合した表現。
    //   view_.data() == nullptr                : source_offset 未設定 (HorizontalRule 等)。
    //   view_.data() != nullptr, view_.size() == 0 : owned モード。表示は owned_text_、data() は raw 内位置。
    //   view_.data() != nullptr, view_.size()  > 0 : view モード。view_ をそのまま表示テキストとして使う。
    // Document::RebaseViews() が move 時に rebase する。
    std::string_view view_;

    // --- 4 バイトアライメント ---
    int32_t blockquote_group = -1; // 最外側 blockquote 単位のグループID（ネストしてもgroupは共有）
    int32_t line_count = 0;        // テキスト内の改行数（パース時にカウント済み）

    // --- 1 バイトアライメント ---
    NodeType type = NodeType::Paragraph;
    AlertType alert_type = AlertType::None; // 同一 blockquote_group 内で伝播するため直下に残す
    int8_t quote_depth = 0;                 // 現在の blockquote ネスト深さ（0 = 引用外, 1.. = ネストレベル）
    int8_t quote_outer_indent = 0;          // 最外側 blockquote が居る indent_level（バー位置の起点）
    int8_t indent_level = 0;                // リスト/引用のネスト深さ（int8_t の最大値で飽和）

    constexpr bool IsViewMode() const noexcept
    {
        return !view_.empty();
    }

    constexpr bool HasSourceOffset() const noexcept
    {
        return view_.data() != nullptr;
    }

    // base (= Document::raw_text_.data()) 起算の UTF-8 byte オフセット。未設定時は kUnsetSourceOffset。
    constexpr uint32_t SourceOffsetFrom(const char* base) const noexcept
    {
        return HasSourceOffset() ? static_cast<uint32_t>(view_.data() - base) : kUnsetSourceOffset;
    }

    // ソース位置 (および任意の view 長) を埋める。length=0 は owned モードでの位置追跡用。
    constexpr void SetSourceOffset(const char* base, uint32_t offset, uint32_t length = 0) noexcept
    {
        view_ = std::string_view{ base + offset, length };
    }

    // raw_text_ の data() が relocate された (Document move) 時に view_ を rebase する。
    constexpr void RebaseSourceOffset(const char* old_base, const char* new_base) noexcept
    {
        if (HasSourceOffset()) {
            view_ = std::string_view{ new_base + (view_.data() - old_base), view_.size() };
        }
    }

    constexpr bool HasText() const noexcept
    {
        // view モードが多数派 (parse 結果で view ノードが過半) なので先に分岐させる。
        if (IsViewMode()) {
            return true;
        }
        return !owned_text_.empty();
    }

    constexpr std::string_view GetText() const noexcept
    {
        if (IsViewMode()) {
            return view_;
        }
        return owned_text_;
    }

    // テーブルは owned_text_ が空で線形化テキストを table->concat_text に保持する。
    // HitTestTable / OnLButtonDblClk が返す text_pos も concat_text 内のグローバル offset
    // 体系で揃っているため、選択範囲を前提とする抽出側はこちらを使う。
    constexpr std::string_view LinearizedText() const noexcept
    {
        if (type == NodeType::Table) {
            if (const auto* tbl = table_data()) {
                return tbl->concat_text;
            }
        }
        return GetText();
    }

    void SetText(const char* s)
    {
        SetText(std::string_view{ s });
    }

    void SetText(std::string_view s)
    {
        owned_text_.assign(s);
        FinalizeOwnedTextLineCount();
    }

    void SetText(std::pmr::string&& s) noexcept
    {
        owned_text_ = std::move(s);
        FinalizeOwnedTextLineCount();
    }

    // 連続 NORMAL/CODE/LATEXMATH のみで構成されるノード向けの view 設定 API。
    // raw_text_ の (source_offset_value, length) 範囲をそのまま表示テキストとして使う。
    // parser 中の Heading anchor 生成等で GetText() が呼ばれるパスがあるため view_base もここで同時設定する。
    void SetTextView(uint32_t source_offset_value, uint32_t length, int32_t line_count_value, const char* view_base) noexcept
    {
        owned_text_.clear();
        view_ = std::string_view{ view_base + source_offset_value, length };
        line_count = line_count_value;
    }

    // text と line_count を一括更新する（呼び出し側が積算済みの line_count を渡す）。
    // 改行を逐次カウントしておけるパーサ向けの最適化バリアント。
    void SetTextWithLineCount(std::string_view s, int32_t line_count_value)
    {
        owned_text_.assign(s);
        DemoteToOwned();
        line_count = line_count_value;
    }

    void SetTextWithLineCount(std::pmr::string&& s, int32_t line_count_value) noexcept
    {
        owned_text_ = std::move(s);
        DemoteToOwned();
        line_count = line_count_value;
    }

    // get_if 風に *_data() でポインタを返す。所持していなければ nullptr。
    // C++23 deducing this で const/非 const を 1 つに集約。
    constexpr auto* heading_data(this auto& self) noexcept
    {
        return std::get_if<NodeHeadingData>(&self.extra);
    }
    constexpr auto* code_data(this auto& self) noexcept
    {
        return std::get_if<NodeCodeData>(&self.extra);
    }
    constexpr auto* list_data(this auto& self) noexcept
    {
        return std::get_if<NodeListData>(&self.extra);
    }
    constexpr auto* alert_data(this auto& self) noexcept
    {
        return std::get_if<NodeAlertData>(&self.extra);
    }

    // table / image は variant に NodePtr (pmr_unique_ptr) を格納する形なので、
    // alternative の存在 → 内部 unique_ptr の中身、と 2 段で取り出す。
    constexpr NodeTableData* table_data() noexcept
    {
        auto* p = std::get_if<NodeTablePtr>(&extra);
        return p ? p->get() : nullptr;
    }
    constexpr const NodeTableData* table_data() const noexcept
    {
        const auto* p = std::get_if<NodeTablePtr>(&extra);
        return p ? p->get() : nullptr;
    }
    constexpr NodeImageData* image_data() noexcept
    {
        auto* p = std::get_if<NodeImagePtr>(&extra);
        return p ? p->get() : nullptr;
    }
    constexpr const NodeImageData* image_data() const noexcept
    {
        const auto* p = std::get_if<NodeImagePtr>(&extra);
        return p ? p->get() : nullptr;
    }

    constexpr bool has_heading() const noexcept
    {
        return std::holds_alternative<NodeHeadingData>(extra);
    }
    constexpr bool has_code() const noexcept
    {
        return std::holds_alternative<NodeCodeData>(extra);
    }
    constexpr bool has_list() const noexcept
    {
        return std::holds_alternative<NodeListData>(extra);
    }
    constexpr bool has_alert() const noexcept
    {
        return std::holds_alternative<NodeAlertData>(extra);
    }
    constexpr bool has_table() const noexcept
    {
        return std::holds_alternative<NodeTablePtr>(extra);
    }
    constexpr bool has_image() const noexcept
    {
        return std::holds_alternative<NodeImagePtr>(extra);
    }

    // ensure_*: 既に存在すれば内部参照、無ければ新規確保して返す。
    // 既存 alternative がある状態で別種の ensure_* を呼ぶと、それは破棄される (variant の emplace 動作)。
    // parser は BeginNode 直後に 1 種類だけ ensure する規約。
    NodeHeadingData* ensure_heading()
    {
        return EnsureAlt<NodeHeadingData>();
    }
    NodeCodeData* ensure_code()
    {
        return EnsureAlt<NodeCodeData>();
    }
    NodeListData* ensure_list()
    {
        return EnsureAlt<NodeListData>();
    }
    NodeAlertData* ensure_alert()
    {
        return EnsureAlt<NodeAlertData>();
    }
    NodeTableData* ensure_table()
    {
        return EnsurePtrAlt<NodeTablePtr>();
    }
    NodeImageData* ensure_image()
    {
        return EnsurePtrAlt<NodeImagePtr>();
    }

    // 該当 alternative を持たないノードでは黙ってデフォルト値 (0 / None / false) を返す。
    //
    // 規約: ホットパス (全 Node ループ等) では `node.type == NodeType::X && node.foo()` の順で短絡評価し、
    //       variant タグ比較を skip すること。例: `node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language())`、
    //       `node.alert_type != AlertType::None && node.alert_label_length() > 0`。
    //       新規アクセサを追加する場合もこの規約に倣い、type で先にゲートできる呼び出し側を維持する。
    constexpr int8_t heading_level() const noexcept
    {
        const auto* hd = heading_data();
        return hd ? hd->heading_level : static_cast<int8_t>(0);
    }
    constexpr SyntaxLanguage code_language() const noexcept
    {
        const auto* cd = code_data();
        return cd ? cd->code_language : SyntaxLanguage::None;
    }
    constexpr int32_t list_number() const noexcept
    {
        const auto* ld = list_data();
        return ld ? ld->list_number : 0;
    }
    constexpr bool task_checked() const noexcept
    {
        const auto* ld = list_data();
        return ld && ld->task_checked;
    }
    constexpr uint32_t alert_label_length() const noexcept
    {
        const auto* ad = alert_data();
        return ad ? ad->alert_label_length : 0u;
    }

    constexpr std::string_view anchor_id() const noexcept
    {
        const auto* hd = heading_data();
        if (hd && hd->anchor_id) {
            return std::string_view{ *hd->anchor_id };
        }
        return std::string_view{};
    }

    // anchor_id は pmr_unique_ptr<pmr::string> で外出しされているため書き込みが 3 段になる。
    // parser と test での重複を避けるため helper に集約する。
    std::pmr::string& ensure_anchor_id_mut()
    {
        auto* hd = ensure_heading();
        if (!hd->anchor_id) {
            hd->anchor_id = mendo::MakePmrUnique<std::pmr::string>();
        }
        return *hd->anchor_id;
    }

    constexpr std::pmr::vector<std::pmr::string>& ensure_link_urls()
    {
        if (!link_urls_) {
            link_urls_ = mendo::MakePmrUnique<std::pmr::vector<std::pmr::string>>();
        }
        return *link_urls_;
    }
    constexpr std::span<const std::pmr::string> view_link_urls() const noexcept
    {
        return SpanOrEmpty(link_urls_);
    }

    const std::pmr::vector<SyntaxToken>& syntax_tokens() const noexcept
    {
        if (const auto* cd = code_data(); cd && cd->tokens) {
            return *cd->tokens;
        }
        static const std::pmr::vector<SyntaxToken> empty;
        return empty;
    }
    std::pmr::vector<SyntaxToken>& syntax_tokens_mut() noexcept
    {
        auto* cd = ensure_code();
        if (!cd->tokens) {
            cd->tokens = mendo::MakePmrUnique<std::pmr::vector<SyntaxToken>>();
        }
        return *cd->tokens;
    }

private:
    // owned モードへデモートする。view_.data() (= source_offset) は保持し size のみ 0 にする。
    constexpr void DemoteToOwned() noexcept
    {
        view_ = std::string_view{ view_.data(), 0 };
    }

    void FinalizeOwnedTextLineCount() noexcept
    {
        line_count = static_cast<int32_t>(std::ranges::count(owned_text_, mendo::doc_lf));
        DemoteToOwned();
    }

    // parser 契約: BeginNode 直後の Node は monostate で、対応する 1 種類だけが ensure される。
    // 異種 alternative を保持した状態で別種を ensure すると元データはサイレントに破棄されるため、
    // Debug ビルドでは契約違反を assert で捕捉する (Release では noop)。
    template <class T>
    T* EnsureAlt()
    {
        if (auto* p = std::get_if<T>(&extra)) {
            return p;
        }
        assert(std::holds_alternative<std::monostate>(extra) &&
               "ensure_*<T>: Node already holds a different alternative — parser contract violation");
        return &extra.emplace<T>();
    }

    template <class Ptr>
    auto* EnsurePtrAlt()
    {
        using T = typename Ptr::element_type;
        auto* p = std::get_if<Ptr>(&extra);
        if (!p) {
            assert(std::holds_alternative<std::monostate>(extra) &&
                   "ensure_*<Ptr>: Node already holds a different alternative — parser contract violation");
            p = &extra.emplace<Ptr>(mendo::MakePmrUnique<T>());
        }
        return p->get();
    }
};

// 超過時の対処: (1) `sizeof(Node)` の実際の値を確認 (例: /d1reportSingleClassLayoutNode)、
// (2) variant alternative のうち最大のものを pmr_unique_ptr 経由で外出し、または
// (3) TextRunList の SBO 値を再検討。閾値変更は ViewStats.RunsSizeHistogram* の実測も併せて確認。
// 注意: variant の discriminator パディングは std lib 実装依存なので、ツールチェーン切替時にも踏む可能性あり。
static_assert(sizeof(Node) <= 144,
              "Node size regression: exceeded 144 bytes — see comment above for remediation steps");

// CodeBlock かつ非 Diagram 言語。ブロック横スクロール対象判定で頻出する組合せ。
constexpr bool IsScrollableCodeBlock(const Node& node) noexcept
{
    return node.type == NodeType::CodeBlock && !IsDiagramLanguage(node.code_language());
}
