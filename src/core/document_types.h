#pragma once
#include <cassert>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <type_traits>
#include <utility>
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
    Image,
    // Mermaid / LaTeX 等、テキストではなくビットマップで描画するコードブロック。code_language() で描画器を選ぶ。
    Diagram
};

enum class AlertType : uint8_t {
    None,
    Note,
    Tip,
    Important,
    Warning,
    Caution
};

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

    bool operator==(const NodeTableData&) const = default;
};

// 読み込んだ画像の寸法はパース結果ではないので持たない (LayoutCache の DiagramEntry が持つ)。
struct NodeImageData {
    std::string_view src; // 画像ソースパス（Markdown内の記述）。raw_text_ か TextArena を指す
};

// Node::SourceOffset() が未設定のときに返す値。
// HorizontalRule のようなテキストを持たないノードはオフセットを持たない。
// 入力は md4c の制約で 2GB 未満なので、Node が 32bit で持つ値と同じにしておく。
inline constexpr size_t kUnsetSourceOffset = std::numeric_limits<uint32_t>::max();

// 1 ノードのリンク URL の上限。TextRun::link_url_index (int16_t) で引ける数まで。
inline constexpr size_t kMaxLinkUrls = std::numeric_limits<int16_t>::max();

// 自分では何も所有しない、trivially copyable なビュー。ノード配列の伸長や詰め直しを memcpy で済ませる。
struct Node {
    // テキスト・runs・リンク URL・画像 src はノード自身では持たず、Document の raw_text_ (原文をそのまま使えるもの)
    // か TextArena (加工したテキスト・runs・URL の配列) を指す。テーブルは Document が所有する NodeTableData を指す。
    // どれも Document と寿命が同じ。長さと件数は 4 バイト領域にまとめて Node を 64B に収める。
    const char* text_ = nullptr;
    const TextRun* runs_ = nullptr;
    // `runs()` の link_url_index で引く URL の配列。
    const std::string_view* link_urls_ = nullptr;

    // ノード種別ごとのデータ。どれを使うかは type (list_number と alert_label_length は flags_) で決まる。
    union Payload {
        NodeTableData* table = nullptr; // Table
        const NodeImageData* image;     // Image
        int32_t list_number;            // 順序付きの ListItem / TaskListItem (0 始まりも可)
        uint32_t alert_label_length;    // Alert の BlockQuote 本体
    } payload_;

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
    AlertType alert_type = AlertType::None; // 同一 blockquote_group 内で伝播するため payload と別に持つ
    int8_t quote_depth = 0;                 // 現在の blockquote ネスト深さ（0 = 引用外, 1.. = ネストレベル）
    int8_t quote_outer_indent = 0;          // 最外側 blockquote が居る indent_level（バー位置の起点）
    int8_t indent_level = 0;                // リスト/引用のネスト深さ（int8_t の最大値で飽和）
    // 該当しないノードでは既定値のまま。
    int8_t heading_level_ = 0;
    SyntaxLanguage code_language_ = SyntaxLanguage::None;
    uint8_t flags_ = 0;

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

    // 種別が違う、または持っていなければ nullptr。
    template <class Self>
    constexpr auto* table_data(this Self& self) noexcept
    {
        using T = std::conditional_t<std::is_const_v<Self>, const NodeTableData, NodeTableData>;
        return self.has_table() ? static_cast<T*>(self.payload_.table) : nullptr;
    }
    constexpr const NodeImageData* image_data() const noexcept
    {
        return has_image() ? payload_.image : nullptr;
    }
    constexpr bool has_table() const noexcept
    {
        return type == NodeType::Table && payload_.table != nullptr;
    }
    constexpr bool has_image() const noexcept
    {
        return type == NodeType::Image && payload_.image != nullptr;
    }

    // table / image はノードより長く生きる領域を指すこと。
    constexpr void set_table_data(NodeTableData* table) noexcept
    {
        payload_.table = table;
    }
    constexpr void set_image_data(const NodeImageData* image) noexcept
    {
        payload_.image = image;
    }

    // 該当しないノードでは黙ってデフォルト値 (0 / false) を返す。
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
    constexpr bool has_child_blocks() const noexcept
    {
        return (flags_ & kHasChildBlocks) != 0;
    }
    constexpr void set_has_child_blocks() noexcept
    {
        flags_ |= kHasChildBlocks;
    }
    constexpr int32_t list_number() const noexcept
    {
        return list_ordered() ? payload_.list_number : 0;
    }
    constexpr bool list_ordered() const noexcept
    {
        return (flags_ & kListOrdered) != 0;
    }
    // 順序付きリストの項目にする。
    constexpr void set_list_number(int32_t number) noexcept
    {
        payload_.list_number = number;
        flags_ |= kListOrdered;
    }
    constexpr bool task_checked() const noexcept
    {
        return (flags_ & kTaskChecked) != 0;
    }
    constexpr void set_task_checked() noexcept
    {
        flags_ |= kTaskChecked;
    }
    // Alert の伝播先 (後続ノード) は alert_type だけを持ち、ラベル長は 0。
    constexpr uint32_t alert_label_length() const noexcept
    {
        return (flags_ & kHasAlertLabel) != 0 ? payload_.alert_label_length : 0u;
    }
    constexpr void set_alert_label_length(uint32_t length) noexcept
    {
        payload_.alert_label_length = length;
        flags_ |= kHasAlertLabel;
    }

    // 長いテキストはパーサが区切り、同じ種別のノードの並びにする (DirectWrite の計測が長さに対して二乗に近く遅くなるため)。
    // 並びは見た目もコピーも 1 つのブロックとして扱う。
    constexpr bool continues_prev() const noexcept
    {
        return (flags_ & kContinuesPrev) != 0;
    }
    constexpr bool continues_next() const noexcept
    {
        return (flags_ & kContinuesNext) != 0;
    }
    // 前のノードとの間にあった改行を、区切るときに取り除いた。立っていなければ前のノードにそのままつながる。
    constexpr bool newline_before_chunk() const noexcept
    {
        return (flags_ & kNewlineBeforeChunk) != 0;
    }
    constexpr void set_continues_prev(bool newline_before) noexcept
    {
        flags_ |= kContinuesPrev | (newline_before ? kNewlineBeforeChunk : 0);
    }
    constexpr void set_continues_next() noexcept
    {
        flags_ |= kContinuesNext;
    }

    constexpr std::span<const std::string_view> view_link_urls() const noexcept
    {
        return { link_urls_, link_url_count_ };
    }

    // urls も各 URL 文字列も、SetText と同じくノードより長く生きる領域を指すこと。
    constexpr void SetLinkUrls(std::span<const std::string_view> urls) noexcept
    {
        assert(urls.size() <= kMaxLinkUrls);
        link_urls_ = urls.data();
        link_url_count_ = static_cast<uint16_t>(urls.size());
    }

private:
    static constexpr uint8_t kListOrdered = 1u << 0;
    static constexpr uint8_t kTaskChecked = 1u << 1;    // TaskListItem のときのみ意味を持つ
    static constexpr uint8_t kHasChildBlocks = 1u << 2; // LI のみ。中身を子ブロック (loose list の段落・入れ子リスト等) に持つ
    static constexpr uint8_t kHasAlertLabel = 1u << 3;
    static constexpr uint8_t kContinuesPrev = 1u << 4;
    static constexpr uint8_t kContinuesNext = 1u << 5;
    static constexpr uint8_t kNewlineBeforeChunk = 1u << 6;
};

// 超過したら /d1reportSingleClassLayoutNode で並びを確認し、payload か末尾の 1 バイト領域に寄せる。
static_assert(sizeof(Node) == 64, "Node size regression: expected 64 bytes");
static_assert(std::is_trivially_copyable_v<Node>, "Node must stay a trivially copyable view");

// i を含む、区切ったノードの並びの先頭と末尾。区切っていなければ i。
constexpr size_t ChunkRunFirst(std::span<const Node> nodes, size_t i) noexcept
{
    while (i > 0 && nodes[i].continues_prev()) {
        --i;
    }
    return i;
}
constexpr size_t ChunkRunLast(std::span<const Node> nodes, size_t i) noexcept
{
    while (i + 1 < nodes.size() && nodes[i].continues_next()) {
        ++i;
    }
    return i;
}

// 区切ったノードの間に戻す文字。next は続きのノード。
constexpr std::string_view ChunkSeparator(const Node& next) noexcept
{
    return next.newline_before_chunk() ? "\n" : "";
}

// テキストではなくビットマップを描くノード。
constexpr bool IsBitmapDrawNode(const Node& node) noexcept
{
    return node.type == NodeType::Image || node.type == NodeType::Diagram;
}

constexpr bool IsListItem(const Node& node) noexcept
{
    return node.type == NodeType::ListItem || node.type == NodeType::TaskListItem;
}

// loose list の LI (md4c が中身を MD_BLOCK_P に外出しするためテキストを持たない枠)。
// 高さ / spacing / cull / bullet 描画で同じ判定を共有するため集約する (issue#237)。
// 子ブロックも無い本当に空の LI は含めない。高さ 0 にすると空の "1." の繰り返しが全部同じ Y に
// 重なり、可視判定で間引けず毎フレーム全件描くことになる。
constexpr bool IsEmptyListItemContainer(const Node& node) noexcept
{
    return IsListItem(node) && !node.HasText() && node.has_child_blocks();
}

// 目次は文書順の見出しの列 (パーサが見出しの終わりで積む)。
struct TocEntry {
    int node_index = -1;
    int heading_level = 1;
};
