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
#include <type_traits>
#include <utility>
#include <variant>
#include "syntax.h"
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

    // row は数十万〜数百万行になりうるため uint32_t。col_count は parser 側で uint16 上限に
    // クランプ済み (65535 列超は非現実的)。両者の積 (cell index) は CellIndex で size_t 昇格。
    uint32_t row_count = 0;
    uint16_t col_count = 0;

    constexpr size_t CellIndex(size_t r, size_t c) const noexcept
    {
        return r * static_cast<size_t>(col_count) + c;
    }

    // セルの concat_text 内開始 offset。
    constexpr uint32_t CellTextStart(size_t r, size_t c) const noexcept
    {
        return cell_text_starts[CellIndex(r, c)];
    }

    // セルの concat_text 内終端 offset (末尾の区切り文字を含まない)。
    constexpr uint32_t CellTextEnd(size_t r, size_t c) const noexcept
    {
        return CellTextStart(r, c) + static_cast<uint32_t>(GetCellText(r, c).size());
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

struct NodeListData {
    int32_t list_number = 0;   // 順序付きリストの番号 (ordered=true のとき有効。0 始まりも可)
    bool ordered = false;
    bool task_checked = false; // TaskListItem のときのみ意味を持つ
};

// alert_type は同一 blockquote_group の後続ノードにも伝播するため Node 直下に置く
// (parser_alerts.cpp:DetectAlertAt 参照)。alert_label_length は BlockQuote 本体専用なので variant 内。
struct NodeAlertData {
    uint32_t alert_label_length = 0;
};

using NodeTablePtr = std::unique_ptr<NodeTableData>;

struct NodeImageData {
    std::string_view src; // 画像ソースパス（Markdown内の記述）。raw_text_ か TextArena を指す
    float width = 0.0f;   // 元画像の幅（ピクセル）
    float height = 0.0f;  // 元画像の高さ（ピクセル）
};
using NodeImagePtr = std::unique_ptr<NodeImageData>;

// Node::SourceOffset() が未設定のときに返す値。
// HorizontalRule のようなテキストを持たないノードはオフセットを持たない。
// 入力は md4c の制約で 2GB 未満なので、Node が 32bit で持つ値と同じにしておく。
inline constexpr size_t kUnsetSourceOffset = std::numeric_limits<uint32_t>::max();

struct Node {
    // テキスト・runs・リンク URL はノード自身では持たず、Document の raw_text_ (原文をそのまま使えるもの)
    // か TextArena (加工したテキスト・runs・URL の配列) を指す。どちらも Document と寿命が同じ。
    // 長さと件数は 4 バイト領域にまとめて Node を 72B に収める。
    const char* text_ = nullptr;
    const TextRun* runs_ = nullptr;
    // `runs()` の link_url_index で引く URL の配列。
    const std::string_view* link_urls_ = nullptr;

    // ノード種別ごとの拡張データ。重データ (table / image) は unique_ptr で
    // 外出しすることで variant alternative の最大サイズを 8B に抑える。
    // Parser invariant: 1 つのノードは 1 つの alternative しか持たない (ensure_* は他を破壊する)。
    using Extra = std::variant<
        std::monostate,  // Paragraph / Heading / CodeBlock / HorizontalRule など固有データ無し
        NodeListData,    // ListItem / TaskListItem
        NodeAlertData,   // BlockQuote 本体の alert ラベル
        NodeTablePtr,    // Table
        NodeImagePtr>;   // Image
    Extra extra;

    // --- 4 バイトアライメント ---
    // テキストは入力 (md4c の制約で 2GB 未満) に Alert ラベルと上限付きの脚注 backref を足した程度なので 32bit に収まる。
    uint32_t text_size_ = 0;
    uint32_t run_count_ = 0;
    uint32_t source_offset_ = kUnsetSourceOffset;
    int32_t blockquote_group = -1; // 最外側 blockquote 単位のグループID（ネストしてもgroupは共有）
    int32_t line_count = 0;        // テキスト内の改行数（パース時にカウント済み）

    // --- 2 バイトアライメント ---
    // link_url_index (int16_t) で引ける数までしか持たない。
    uint16_t link_url_count_ = 0;

    // --- 1 バイトアライメント ---
    NodeType type = NodeType::Paragraph;
    AlertType alert_type = AlertType::None; // 同一 blockquote_group 内で伝播するため直下に残す
    int8_t quote_depth = 0;                 // 現在の blockquote ネスト深さ（0 = 引用外, 1.. = ネストレベル）
    int8_t quote_outer_indent = 0;          // 最外側 blockquote が居る indent_level（バー位置の起点）
    int8_t indent_level = 0;                // リスト/引用のネスト深さ（int8_t の最大値で飽和）
    // variant alternative に入れると 8B ポインタと合わせて 16B に膨らむため、末尾 padding に置く。
    // 該当しないノードでは既定値のまま。
    int8_t heading_level_ = 0;
    SyntaxLanguage code_language_ = SyntaxLanguage::None;

    constexpr bool HasSourceOffset() const noexcept
    {
        return source_offset_ != kUnsetSourceOffset;
    }

    // Document::raw_text_ 起算の UTF-8 byte オフセット。未設定時は kUnsetSourceOffset。
    constexpr size_t SourceOffset() const noexcept
    {
        return source_offset_;
    }

    constexpr void SetSourceOffset(size_t offset) noexcept
    {
        assert(offset < kUnsetSourceOffset);
        source_offset_ = static_cast<uint32_t>(offset);
    }

    constexpr bool HasText() const noexcept
    {
        return text_size_ != 0;
    }

    constexpr std::string_view GetText() const noexcept
    {
        return { text_, text_size_ };
    }

    // text はノードより長く生きる領域 (raw_text_ / TextArena / テストでは静的文字列) を指すこと。
    // line_count は変えない (パーサがテキスト確定前に積算している)。
    constexpr void SetText(std::string_view text) noexcept
    {
        assert(text.size() <= std::numeric_limits<uint32_t>::max());
        text_ = text.data();
        text_size_ = static_cast<uint32_t>(text.size());
    }

    // line_count は呼び出し側が数えた text 内の改行数。
    constexpr void SetText(std::string_view text, int32_t line_count_value) noexcept
    {
        SetText(text);
        line_count = line_count_value;
    }

    constexpr std::span<const TextRun> runs() const noexcept
    {
        return { runs_, run_count_ };
    }

    // runs も SetText と同じく、ノードより長く生きる領域を指すこと。
    constexpr void SetRuns(std::span<const TextRun> runs_value) noexcept
    {
        runs_ = runs_value.data();
        run_count_ = static_cast<uint32_t>(runs_value.size());
    }

    // テーブルは text_ が空で線形化テキストを table->concat_text に保持する。
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

    // get_if 風に *_data() でポインタを返す。所持していなければ nullptr。
    // C++23 deducing this で const/非 const を 1 つに集約。
    constexpr auto* list_data(this auto& self) noexcept
    {
        return std::get_if<NodeListData>(&self.extra);
    }
    constexpr auto* alert_data(this auto& self) noexcept
    {
        return std::get_if<NodeAlertData>(&self.extra);
    }

    // table / image は variant に unique_ptr を格納する形なので、
    // alternative の存在 → 内部 unique_ptr の中身、と 2 段で取り出す。
    constexpr auto* table_data(this auto& self) noexcept
    {
        return GetPtrAlt<NodeTablePtr>(self);
    }
    constexpr auto* image_data(this auto& self) noexcept
    {
        return GetPtrAlt<NodeImagePtr>(self);
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
    constexpr NodeListData* ensure_list()
    {
        return EnsureAlt<NodeListData>();
    }
    constexpr NodeAlertData* ensure_alert()
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
    // アンカーは Document::FindAnchorIndex が必要時に作る。
    constexpr int8_t heading_level() const noexcept
    {
        return type == NodeType::Heading ? heading_level_ : static_cast<int8_t>(0);
    }
    constexpr SyntaxLanguage code_language() const noexcept
    {
        return code_language_;
    }
    constexpr void set_heading_level(int8_t level) noexcept
    {
        heading_level_ = level;
    }
    constexpr void set_code_language(SyntaxLanguage lang) noexcept
    {
        code_language_ = lang;
    }
    constexpr int32_t list_number() const noexcept
    {
        const auto* ld = list_data();
        return ld ? ld->list_number : 0;
    }
    constexpr bool list_ordered() const noexcept
    {
        const auto* ld = list_data();
        return ld && ld->ordered;
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

    constexpr std::span<const std::string_view> view_link_urls() const noexcept
    {
        return { link_urls_, link_url_count_ };
    }

    // urls も各 URL 文字列も、SetText と同じくノードより長く生きる領域を指すこと。
    constexpr void SetLinkUrls(std::span<const std::string_view> urls) noexcept
    {
        assert(urls.size() <= static_cast<size_t>(std::numeric_limits<int16_t>::max()));
        link_urls_ = urls.data();
        link_url_count_ = static_cast<uint16_t>(urls.size());
    }

private:
    // parser 契約: BeginNode 直後の Node は monostate で、対応する 1 種類だけが ensure される。
    // 異種 alternative を保持した状態で別種を ensure すると元データはサイレントに破棄されるため、
    // Debug ビルドでは契約違反を assert で捕捉する (Release では noop)。
    template <class T>
    constexpr T* EnsureAlt()
    {
        if (auto* p = std::get_if<T>(&extra)) {
            return p;
        }
        assert(std::holds_alternative<std::monostate>(extra) &&
               "ensure_*<T>: Node already holds a different alternative — parser contract violation");
        return &extra.emplace<T>();
    }

    // unique_ptr::get() は const でも非 const ポインタを返すため、self の const 性を明示的に伝播する。
    template <class Ptr, class Self>
    static constexpr auto* GetPtrAlt(Self& self) noexcept
    {
        using T = std::conditional_t<std::is_const_v<Self>, const typename Ptr::element_type, typename Ptr::element_type>;
        const auto* p = std::get_if<Ptr>(&self.extra);
        return p ? static_cast<T*>(p->get()) : nullptr;
    }

    template <class Ptr>
    auto* EnsurePtrAlt()
    {
        using T = typename Ptr::element_type;
        auto* p = std::get_if<Ptr>(&extra);
        if (!p) {
            assert(std::holds_alternative<std::monostate>(extra) &&
                   "ensure_*<Ptr>: Node already holds a different alternative — parser contract violation");
            p = &extra.emplace<Ptr>(std::make_unique<T>());
        }
        return p->get();
    }
};

// 超過時の対処: (1) `sizeof(Node)` の実際の値を確認 (例: /d1reportSingleClassLayoutNode)、
// (2) variant alternative のうち最大のものを unique_ptr 経由で外出しする。
// 注意: variant の discriminator パディングは std lib 実装依存なので、ツールチェーン切替時にも踏む可能性あり。
static_assert(sizeof(Node) <= 72,
              "Node size regression: exceeded 72 bytes — see comment above for remediation steps");

// Mermaid / LaTeX 等、テキストではなくビットマップで描画するコードブロック。
constexpr bool IsDiagramCodeBlock(const Node& node) noexcept
{
    return node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language());
}

// テキストではなくビットマップを描くノード (画像 / Diagram コードブロック)。
constexpr bool IsBitmapDrawNode(const Node& node) noexcept
{
    return node.type == NodeType::Image || IsDiagramCodeBlock(node);
}

// CodeBlock かつ非 Diagram 言語。ブロック横スクロール対象判定で頻出する組合せ。
constexpr bool IsScrollableCodeBlock(const Node& node) noexcept
{
    return node.type == NodeType::CodeBlock && !IsDiagramLanguage(node.code_language());
}

constexpr bool IsListItem(const Node& node) noexcept
{
    return node.type == NodeType::ListItem || node.type == NodeType::TaskListItem;
}

// loose list の LI (md4c が中身を MD_BLOCK_P に外出しするためテキストを持たない枠)。
// 高さ / spacing / cull / bullet 描画で同じ判定を共有するため集約する (issue#237)。
constexpr bool IsEmptyListItemContainer(const Node& node) noexcept
{
    return IsListItem(node) && !node.HasText();
}
