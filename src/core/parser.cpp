#include "parser.h"
#include "html_entities.h"
#include "document_utils.h"
#include "syntax.h"
#include "profiler.h"
#include "md4c.h"
#include <functional>
#include <unordered_map>
#include <format>
#include <iterator>
#include <algorithm>
#include <limits>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

// pmr::string キーの unordered_map を string_view で引いてもキーを確保しないための透過ハッシュ。
struct StringTransparentHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
};

struct ParseContext {
    std::stop_token stop_token;
    // md4c コールバックは 100MB 入力で 50 万回以上呼ばれるため、毎回 atomic load せず
    // 1024 callbacks ごとに stop_requested() を確認する間引きカウンタ。
    // 最大 1024 callback 分の検知遅延は 600ms parse に対し μs オーダーで許容範囲。
    uint32_t cancel_check_counter = 0;
    bool cancel_requested = false;

    bool ShouldCancel() noexcept
    {
        if (cancel_requested) {
            return true;
        }
        if ((++cancel_check_counter & 0x3FFu) != 0u) {
            return false;
        }
        cancel_requested = stop_token.stop_requested();
        return cancel_requested;
    }

    std::pmr::vector<Node> nodes;

    // パース後の全ノード走査を避けるため、特殊ノードのインデックスはパース中に構築する。
    std::pmr::vector<size_t> heading_indices;
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> diagram_indices;
    std::pmr::vector<size_t> table_indices;
    std::pmr::vector<size_t> blockquote_indices;
    size_t current_node_index = 0;

    // span ネスト追跡は md4c 推奨のカウンタ方式。enter で +1, leave で -1。
    // counter > 0 の間その属性が有効。CommonMark で link はネスト禁止のため
    // link_url_index のみ単純フィールド (-1 = リンク外)。
    uint8_t bold_count = 0;
    uint8_t italic_count = 0;
    uint8_t code_count = 0;
    uint8_t strikethrough_count = 0;
    // 上記カウンタを TextRun のフラグビットに射影したキャッシュ。MakeRun のたびに 4 回
    // ビット操作するのではなく、span enter/leave のときだけ 1 回ビット OR/AND を更新する。
    // 不変式: bit X が立っている <=> 対応する *_count > 0。
    uint8_t current_run_flags = 0;
    // 現在の <a> span に対応する link_urls インデックス。-1 = リンク外。
    // CommonMark で <a> はネスト禁止のため leave までこの値が安定する。
    int16_t current_link_url_index = -1;

    // 現在ノード用のテキスト蓄積スクラッチ (UTF-8)。FinalizeCurrentNode で view 化されるか
    // Node::owned_text_ へコピーされる。
    std::pmr::string current_text;

    // current_text 内の「未確定 TextRun」の開始位置 (UTF-8 byte unit)。
    // 同じ span 状態で連続する AppendDoc は 1 つの TextRun に統合される。
    // span 切替 / ブロック退出時に FlushPendingRun が呼ばれて確定する。
    uint32_t pending_run_start = 0;
    bool has_pending_run = false;
    // pending_run_start 以降に AppendDoc で押し込まれた改行の数。FlushPendingRun の count 走査を排除。
    // md4c の \n を size==1 の単独 chunk としてしか OnText に渡さない契約 (BR/SOFTBR/HTML 改行/code 行末) に依存している。
    int32_t pending_run_newlines = 0;

    // ブロックコンテキスト追跡
    int indent_level = 0;
    bool in_code_block = false;
    int blockquote_depth = 0;
    int blockquote_group_counter = 0;  // グループID生成用
    int current_blockquote_group = -1; // 最外側 blockquote の group ID（ネスト中は共有）
    int outermost_quote_indent = 0;    // 最外側 blockquote 進入時の indent_level（描画時のバー起点）

    // リスト追跡: 0 = 順序なしリスト, >0 = 順序ありリストのカウンター
    // スタックアダプタを挟まず vector を直接扱う (back/push_back/pop_back)。
    std::pmr::vector<int> list_counter;

    // 現在構築中のノード
    Node* current_node = nullptr;

    // AppendDoc / FlushPendingRun のターゲットバッファのキャッシュ。
    // 47-94 万回呼ばれる hot path で has_table() の variant 判定を毎回行わないよう、
    // 状態遷移点 (BeginNode / TD/TH 進入退出 / current_node clear) で更新したポインタを直接使う。
    // &current_text 以外を指すのはテーブルセル内 (NodeTableData::concat_text) のときだけ。
    // nullptr のときは AppendDoc / FlushPendingRun は no-op。
    std::pmr::string* active_text_buffer = nullptr;

    // アンカーIDの一意性追跡: スラグ -> 出現回数。
    std::pmr::unordered_map<std::pmr::string, int, StringTransparentHash, std::equal_to<>> anchor_counts;

    // 現在ノードの link_urls の URL -> インデックス索引。URL 数が kLinkUrlLinearScanMax を
    // 超えたノードでのみ構築する (テーブルは全セルで 1 ノードのため数万 URL になりうる)。
    // urls 内の SSO 文字列は vector 伸長で移動するため string_view ではなく複製をキーにする。
    static constexpr size_t kLinkUrlLinearScanMax = 8;
    std::pmr::unordered_map<std::pmr::string, int16_t, StringTransparentHash, std::equal_to<>> link_url_lookup;

    // 画像スパンの src 蓄積バッファ。
    // ネスト画像 (![a ![b](inner)](outer)) では最外側の src を採用するため深度を追跡する。
    std::pmr::string pending_image_src;
    int image_span_depth = 0;

    // display math スパンが 1 個だけで他の内容が無い段落を LatexMath コードブロックに昇格する状態
    bool in_display_math = false;
    int paragraph_display_math_count = 0;
    bool paragraph_has_other_content = false;
    std::pmr::string display_math_buf;
    // display_math_buf に append された範囲の改行数。昇格時の line_count 設定に使い、
    // current_text 全体を std::ranges::count で走査するコストを避ける。
    int32_t display_math_newlines = 0;

    // md_parse() に渡した入力バッファ。source_offset 計算と OnText の text ポインタ範囲判定に使う。
    const char* markdown_base = nullptr;
    size_t markdown_size = 0;

    // 現在ノードが owned 経路確定か。span markup (** _ ` 等) の存在や、
    // entity 解決 / BR / SOFTBR で text を置換するケースは current_text と raw_slice が
    // 構造的に一致しなくなるため、FinalizeCurrentNode の memcmp を完全にスキップできる。
    bool current_node_owned_only = false;

    // current_text が source の (offset, current_text.size()) 範囲とバイト一致するか。
    // 一致するノードは Node::owned_text_ を確保せず raw_text_ への view に倒せる。
    // span マークアップ (** _ ` 等) や BR/SOFTBR/ENTITY 混在のノードは不一致で owned 経路に落ちる。
    // offset は呼び出し側で SourceOffsetFrom した結果を渡す (FinalizeCurrentNode で再利用するため)。
    bool CurrentTextMatchesRawSliceAt(size_t offset) const noexcept
    {
        // 高頻度呼び出し (100MB で 40万回超) のため MENDO_PROFILE は外す。
        // zone overhead が ~120ms 単位で計測自体を歪めるため。
        const size_t len = current_text.size();
        if (offset + len > markdown_size) {
            return false;
        }
        // current_node_owned_only で span/entity 混在は事前に弾けるため、ここまで来たノードは大半が
        // 全長一致 (success) になる。probe 短絡は worst-case で len 分の比較が走り意味がないため、
        // 1 回の memcmp に統一する。
        return std::char_traits<char>::compare(markdown_base + offset, current_text.data(), len) == 0;
    }

    // 現在ノードのテキストスクラッチを Node に確定し、スクラッチをクリアする。
    // BeginNode 直前と各 OnLeaveBlock の current_node 解除直前に呼ぶ。
    // current_text の capacity は保持して次ノードで再利用する (確保回数削減のため)。
    void FinalizeCurrentNode()
    {
        FlushPendingRun();
        // 昇格処理 (TryPromoteParagraphToDisplayMath 等) がテキストを直接設定済みのノードは、
        // current_text で上書きすると line_count ごと巻き戻るのでスキップする。
        if (current_node && !current_text.empty() && !current_node->HasText()) {
            const auto offset = current_node->SourceOffsetFrom(markdown_base);
            if (!current_node_owned_only && offset != kUnsetSourceOffset && CurrentTextMatchesRawSliceAt(offset)) {
                current_node->SetTextView(
                    markdown_base,
                    offset,
                    current_text.size(),
                    current_node->line_count);
            }
            else {
                current_node->SetTextWithLineCount(current_text, current_node->line_count);
            }
        }
        current_text.clear();
    }

    // current_node と active_text_buffer は対で管理する不変式があるため一括で nullptr に倒す。
    constexpr void ClearCurrentNode() noexcept
    {
        current_node = nullptr;
        active_text_buffer = nullptr;
    }

    void EndNode()
    {
        FinalizeCurrentNode();
        ClearCurrentNode();
    }

    // display math 以外の内容が段落に現れたことを記録する (昇格判定用)。
    constexpr void NoteNonMathContent() noexcept
    {
        if (!in_display_math) {
            paragraph_has_other_content = true;
        }
    }

    void BeginNode(NodeType type)
    {
        FinalizeCurrentNode();
        nodes.emplace_back();
        current_node = &nodes.back();
        active_text_buffer = &current_text;
        current_node->type = type;
        current_node_index = nodes.size() - 1;
        constexpr int kInt8Max = std::numeric_limits<int8_t>::max();
        current_node->indent_level = static_cast<int8_t>(std::min(indent_level, kInt8Max));
        if (blockquote_depth > 0) {
            current_node->blockquote_group = current_blockquote_group;
            current_node->quote_depth = static_cast<int8_t>(std::min(blockquote_depth, kInt8Max));
            current_node->quote_outer_indent = static_cast<int8_t>(std::min(outermost_quote_indent, kInt8Max));
        }
        // runs は SBO 内で初期確保ゼロを狙う。reserve すると SBO の利点が消えるので呼ばない。
        current_node_owned_only = false;
        link_url_lookup.clear();
    }

    // md4c は MD_TEXT_NULLCHAR / MD_TEXT_BR / MD_TEXT_SOFTBR や、CODE/LATEXMATH/HTML の改行・空白置換で
    // _T(""), _T("\n"), _T(" ") といった内部静的リテラルを text として渡すことがある。
    // 異なる array 同士のポインタ減算は UB なので、範囲判定もオフセット計算も uintptr_t の
    // 整数演算で行う (std::less<T*> の total order はアドレスの数値順と一致する保証がない)。
    // 範囲外 (静的リテラル) のときは記録せず、後続の実体テキストで設定できるようにする。
    void RecordSourceOffset(const char* text) noexcept
    {
        const auto text_addr = reinterpret_cast<uintptr_t>(text);
        const auto base_addr = reinterpret_cast<uintptr_t>(markdown_base);
        const auto end_addr = base_addr + markdown_size;
        if (text_addr >= base_addr && text_addr < end_addr) {
            current_node->SetSourceOffset(markdown_base, static_cast<size_t>(text_addr - base_addr));
        }
    }

    // 引用内の段落は BlockQuote ノードとして作り、Alert 検出対象に登録する。
    void BeginParagraphNode()
    {
        const auto node_type = (blockquote_depth > 0) ? NodeType::BlockQuote : NodeType::Paragraph;
        BeginNode(node_type);
        if (node_type == NodeType::BlockQuote) {
            blockquote_indices.emplace_back(current_node_index);
        }
    }

    // md4c は tight list の LI 直下の段落を MD_BLOCK_P で囲まないため、LI 内で見出し / HR /
    // フェンス等の後に続く本文は current_node 無しで届く。loose list と同じ段落ノードを補って受け止める。
    bool EnsureNodeForListItemText()
    {
        if (current_node) {
            return true;
        }
        // md4c は UL/OL 直下に LI しか置かないため、list_counter が空でなければ LI 内にいる。
        if (list_counter.empty()) {
            return false;
        }
        BeginParagraphNode();
        return true;
    }

    constexpr TextRun MakeRun(uint32_t start, uint32_t length)
    {
        TextRun run;
        run.start = start;
        run.length = length;
        run.set_raw_flags(current_run_flags);
        run.link_url_index = current_link_url_index;
        return run;
    }

    // url を Node::link_urls に登録し、インデックスを current_link_url_index にキャッシュする。
    // 1 ノードあたりの URL 数は典型的に < 8 で、その領域ではキー複製・ハッシュ計算を伴う
    // ハッシュマップより線形 memcmp の方が速い。数が増えたノードだけ link_url_lookup に切り替え、
    // テーブル等で O(n^2) になるのを防ぐ。
    void ResolveLinkUrlIndex(std::string_view url)
    {
        current_link_url_index = (current_node && !url.empty()) ? FindOrAddLinkUrl(url) : int16_t{ -1 };
    }

    int16_t FindOrAddLinkUrl(std::string_view url)
    {
        const auto existing = current_node->view_link_urls();
        const size_t n = existing.size();
        if (n <= kLinkUrlLinearScanMax) {
            for (size_t i = 0; i < n; ++i) {
                if (existing[i] == url) {
                    return static_cast<int16_t>(i);
                }
            }
        }
        else {
            if (link_url_lookup.empty()) {
                for (size_t i = 0; i < n; ++i) {
                    link_url_lookup.emplace(existing[i], static_cast<int16_t>(i));
                }
            }
            if (const auto it = link_url_lookup.find(url); it != link_url_lookup.end()) {
                return it->second;
            }
        }
        // link_url_index (int16_t) を超える URL はリンクなしとして扱う
        if (n >= static_cast<size_t>(std::numeric_limits<int16_t>::max())) {
            return -1;
        }
        auto& urls = current_node->ensure_link_urls();
        const auto new_index = static_cast<int16_t>(urls.size());
        urls.emplace_back(url);
        if (!link_url_lookup.empty()) {
            link_url_lookup.emplace(url, new_index);
        }
        return new_index;
    }

    // テキストを現在のノードまたはセルに追加する (UTF-8)。
    // 同じ span 状態で連続して呼ばれると 1 つの TextRun に統合される (cell も統合対象)。
    // セル切替・span 切替・ブロック退出の各タイミングで FlushPendingRun が走る前提。
    // セル内では active_text_buffer が NodeTableData::concat_text を指す。pending_run_start は
    // バッファサイズベースで、FlushPendingRun が現在セルの開始 offset を引いて cell-local にする。
    constexpr void AppendDoc(std::string_view text)
    {
        std::pmr::string* const target = active_text_buffer;
        if (!target) {
            return;
        }
        // md4c は size 0 の text コールバックを発生させない契約 (md4c.c の MD_TEXT マクロで size > 0 ガード済) なので empty 判定は省く。
        if (!has_pending_run) {
            pending_run_start = static_cast<uint32_t>(target->size());
            has_pending_run = true;
        }
        // 1-char chunk fastpath: md4c は \n / 空白 / 単一 entity 等を size==1 で渡してくるので push_back に振り分ける。
        if (text.size() == 1) {
            const char c = text[0];
            pending_run_newlines += (c == '\n');
            target->push_back(c);
        }
        else {
            target->append(text);
        }
    }

    // 未確定 TextRun を確定して runs に push する。
    // span 状態が変わる直前 (OnEnter/Leave Span)、セル切替、OnLeaveBlock の冒頭で呼ぶ。
    // line_count はノード単位なので、セル内では更新しない。
    // active_text_buffer が非 null なら current_node も非 null (両者は対で更新される)。
    constexpr void FlushPendingRun()
    {
        if (has_pending_run) {
            std::pmr::string* const buf = active_text_buffer;
            if (buf && buf->size() > pending_run_start) {
                const uint32_t length = static_cast<uint32_t>(buf->size() - pending_run_start);
                if (buf == &current_text) {
                    current_node->line_count += pending_run_newlines;
                    current_node->runs.emplace_back(MakeRun(pending_run_start, length));
                }
                else {
                    // セル内: cell_text_starts.back() は TD/TH 進入時に積んだ現在セルの開始 offset。
                    auto* const tbl = current_node->table_data();
                    tbl->all_runs.push_back(MakeRun(pending_run_start - tbl->cell_text_starts.back(), length));
                }
            }
        }
        has_pending_run = false;
        pending_run_newlines = 0;
    }
};

// MD_BLOCK_P 終了時、画像 span を含む段落 / 引用ブロックを Image ノードへ昇格させる。
// 戻り値 true なら他の昇格処理はスキップしてよい。
constexpr bool TryPromoteParagraphToImage(ParseContext* ctx)
{
    auto* const cn = ctx->current_node;
    if (!cn || !cn->has_image() || cn->image_data()->src.empty()) {
        return false;
    }
    if (cn->type != NodeType::Paragraph && cn->type != NodeType::BlockQuote) {
        return false;
    }
    cn->type = NodeType::Image;
    ctx->image_indices.emplace_back(ctx->current_node_index);
    return true;
}

// MD_BLOCK_P 終了時、$$..$$ ブロック数式が単独の段落を LaTeX CodeBlock へ昇格させる。
// blockquote 内は引用文脈を保ちたいため Paragraph のみ昇格対象。
constexpr bool TryPromoteParagraphToDisplayMath(ParseContext* ctx)
{
    auto* const node = ctx->current_node;
    if (!node || node->type != NodeType::Paragraph) {
        return false;
    }
    if (ctx->paragraph_display_math_count != 1 ||
        ctx->paragraph_has_other_content ||
        ctx->display_math_buf.empty()) {
        return false;
    }
    node->type = NodeType::CodeBlock;
    node->set_code_language(SyntaxLanguage::LatexMath);
    node->runs.clear();
    // current_text 経由で渡すと FinalizeCurrentNode で 2 回目のコピーが走るため、ノードへ直接書く。
    node->SetTextWithLineCount(ctx->display_math_buf, ctx->display_math_newlines);
    ctx->diagram_indices.emplace_back(ctx->current_node_index);
    return true;
}

void BeginListItem(ParseContext* ctx, const MD_BLOCK_LI_DETAIL* li)
{
    const bool is_task = li->is_task;
    ctx->BeginNode(is_task ? NodeType::TaskListItem : NodeType::ListItem);
    // unordered (counter<0) かつ非 task のときは NodeListData を確保しない
    // (getter が ordered=false / list_number=0 / task_checked=false を返すため)。
    const int counter = ctx->list_counter.empty() ? -1 : ctx->list_counter.back();
    const bool ordered = (counter >= 0); // OL は start>=0、UL は番兵 -1
    if (!is_task && !ordered) {
        return;
    }
    auto* const ld = ctx->current_node->ensure_list();
    if (is_task) {
        ld->task_checked = (li->task_mark == 'x' || li->task_mark == 'X');
    }
    if (ordered) {
        ld->ordered = true;
        ld->list_number = counter;
        ctx->list_counter.back()++;
    }
}

void BeginTable(ParseContext* ctx, const MD_BLOCK_TABLE_DETAIL* detail)
{
    ctx->BeginNode(NodeType::Table);
    ctx->table_indices.emplace_back(ctx->current_node_index);
    // 後続の TR/TH/TD で nullable チェックなく参照できるよう先に確保する。
    // これに依存して TR/TH/TD は has_table() ガードを省いている。
    auto* const tbl = ctx->current_node->ensure_table();
    if (!detail) {
        return;
    }
    // md4c から正確なテーブルサイズが渡されるので、各 vector を一度に reserve して
    // 巨大テーブル時の段階的 realloc (~quadratic コスト) を避ける。
    const size_t total_rows = static_cast<size_t>(detail->head_row_count) + detail->body_row_count;
    tbl->col_count = static_cast<uint16_t>(std::min<unsigned>(detail->col_count, std::numeric_limits<uint16_t>::max()));
    const size_t total_cells = total_rows * tbl->col_count;
    tbl->cell_text_starts.reserve(total_cells + 1);
    tbl->cell_run_starts.reserve(total_cells + 1);
    // 1 セル平均 16 byte + 区切り 1 byte の見積もり。
    tbl->concat_text.reserve(total_cells * 17);
    tbl->aligns.reserve(tbl->col_count);
    tbl->is_header_row.reserve(total_rows);
}

void BeginTableCell(ParseContext* ctx, bool is_header, const MD_BLOCK_TD_DETAIL* detail)
{
    auto* const cn = ctx->current_node;
    if (!cn || cn->type != NodeType::Table) {
        return;
    }
    auto* const tbl = cn->table_data();
    if (tbl->row_count == 0) {
        return;
    }
    // 行内セル数 (is_header_row エントリ数が現 row_count 未満なら未確定 = 行頭)。
    const bool first_cell_in_row = (tbl->is_header_row.size() < tbl->row_count);
    const bool first_row = (tbl->row_count == 1);
    // 区切り: 行内 2 セル目以降は '\t'、行頭かつ 2 行目以降は '\n'。
    if (!first_cell_in_row) {
        tbl->concat_text.push_back('\t');
    }
    else if (!first_row) {
        tbl->concat_text.push_back('\n');
    }
    tbl->cell_text_starts.push_back(static_cast<uint32_t>(tbl->concat_text.size()));
    tbl->cell_run_starts.push_back(static_cast<uint32_t>(tbl->all_runs.size()));
    ctx->active_text_buffer = &tbl->concat_text;

    // 列単位の align は header 行 (1 行目) で決まる。col_count は BeginTable で確定済み。
    if (first_row) {
        tbl->aligns.push_back(detail ? static_cast<TableAlign>(detail->align) : TableAlign::Default);
    }
    // md4c は TR 内で TH/TD を混在させないため、1 セル目で行の種別が確定する。
    if (first_cell_in_row) {
        tbl->is_header_row.push_back(is_header);
    }
}

// offset テーブルを R*C+1 サイズに揃える。行内セル数が col_count に満たない行は空セル扱い
// (offset は concat 末尾に詰めて padding)。
void FinalizeTableOffsets(NodeTableData& tbl)
{
    const size_t expected_cells = static_cast<size_t>(tbl.row_count) * tbl.col_count;
    const auto text_end = static_cast<uint32_t>(tbl.concat_text.size());
    const auto run_end = static_cast<uint32_t>(tbl.all_runs.size());
    // padding は extend のみ (md4c が誤ってセル超過した場合に切り詰めない)。
    if (tbl.cell_text_starts.size() < expected_cells) {
        tbl.cell_text_starts.resize(expected_cells, text_end);
        tbl.cell_run_starts.resize(expected_cells, run_end);
    }
    tbl.cell_text_starts.push_back(text_end);
    tbl.cell_run_starts.push_back(run_end);
    if (tbl.is_header_row.size() < tbl.row_count) {
        tbl.is_header_row.resize(tbl.row_count, false);
    }
    if (tbl.aligns.size() < tbl.col_count) {
        tbl.aligns.resize(tbl.col_count, TableAlign::Default);
    }
}

void LeaveCodeBlock(ParseContext* ctx)
{
    ctx->in_code_block = false;
    auto* const cn = ctx->current_node;
    if (!cn) {
        return;
    }
    // md4c はコード各行を改行付きで渡すため、最終行の改行を表示テキストから落とす。
    if (!ctx->current_text.empty() && ctx->current_text.back() == '\n') {
        ctx->current_text.pop_back();
        cn->line_count--;
        if (!cn->runs.empty()) {
            auto& last = cn->runs.back();
            if (last.length > 0) {
                last.length--;
            }
        }
    }
    if (cn->code_language() == SyntaxLanguage::Mermaid) {
        ctx->diagram_indices.emplace_back(ctx->current_node_index);
    }
}

// 重複する見出しは "-N" を付けて一意化する。
void AssignHeadingAnchor(ParseContext* ctx, Node& heading)
{
    std::pmr::string base_id;
    GenerateAnchorIdInto(heading.GetText(), base_id);
    const auto [it, inserted] = ctx->anchor_counts.try_emplace(std::move(base_id), 0);
    auto& aid = heading.ensure_anchor_id_mut();
    if (inserted) {
        aid.assign(it->first.data(), it->first.size());
        return;
    }
    // "A","A","A-1" のように連番付きスラグが別見出しの素のスラグと衝突しうるため、
    // github-slugger と同じく未使用になるまで連番を進め、採用したスラグ自体も登録する。
    std::pmr::string candidate;
    do {
        candidate.assign(it->first);
        std::format_to(std::back_inserter(candidate), "-{}", ++it->second);
    } while (ctx->anchor_counts.contains(candidate));
    aid.assign(candidate.data(), candidate.size());
    ctx->anchor_counts.try_emplace(std::move(candidate), 0);
}

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_H: {
        auto* const h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        ctx->BeginNode(NodeType::Heading);
        ctx->current_node->set_heading_level(static_cast<int8_t>(h->level));
        break;
    }

    case MD_BLOCK_P:
        if (!ctx->in_code_block) {
            ctx->BeginParagraphNode();
        }
        ctx->paragraph_display_math_count = 0;
        ctx->paragraph_has_other_content = false;
        ctx->display_math_buf.clear();
        ctx->display_math_newlines = 0;
        ctx->in_display_math = false;
        break;

    case MD_BLOCK_CODE: {
        ctx->in_code_block = true;
        ctx->BeginNode(NodeType::CodeBlock);
        auto* const code_detail = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        if (code_detail && code_detail->lang.text && code_detail->lang.size > 0) {
            ctx->current_node->set_code_language(DetectLanguage(std::string_view{ code_detail->lang.text, static_cast<size_t>(code_detail->lang.size) }));
        }
        break;
    }

    case MD_BLOCK_QUOTE:
        if (ctx->blockquote_depth == 0) {
            ctx->current_blockquote_group = ++ctx->blockquote_group_counter;
            ctx->outermost_quote_indent = ctx->indent_level + 1;
        }
        ctx->blockquote_depth++;
        ctx->indent_level++;
        break;

    case MD_BLOCK_UL:
        ctx->list_counter.push_back(-1); // unordered の番兵 (ordered の start は 0 以上)
        ctx->indent_level++;
        break;

    case MD_BLOCK_OL: {
        auto* const ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        ctx->list_counter.push_back(static_cast<int>(ol->start));
        ctx->indent_level++;
        break;
    }

    case MD_BLOCK_LI:
        BeginListItem(ctx, static_cast<const MD_BLOCK_LI_DETAIL*>(detail));
        break;

    case MD_BLOCK_HR:
        ctx->BeginNode(NodeType::HorizontalRule);
        break;

    case MD_BLOCK_TABLE:
        BeginTable(ctx, static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail));
        break;

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        break;

    case MD_BLOCK_TR:
        if (auto* cn = ctx->current_node; cn && cn->type == NodeType::Table) {
            ++cn->table_data()->row_count;
        }
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        BeginTableCell(ctx, type == MD_BLOCK_TH, static_cast<const MD_BLOCK_TD_DETAIL*>(detail));
        break;

    case MD_BLOCK_HTML:
        break;
    default:
        std::unreachable();
    }

    return 0;
}

int OnLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    ctx->FlushPendingRun();

    switch (type) {
    case MD_BLOCK_CODE:
        LeaveCodeBlock(ctx);
        // 他のリーフブロックと同様に leave で閉じる。残すと tight list でフェンス直後に
        // MD_BLOCK_P 無しで届く本文がコードに混入する。
        ctx->EndNode();
        break;

    case MD_BLOCK_QUOTE:
        if (ctx->blockquote_depth > 0) {
            ctx->blockquote_depth--;
        }
        if (ctx->indent_level > 0) {
            ctx->indent_level--;
        }
        if (ctx->blockquote_depth == 0) {
            ctx->current_blockquote_group = -1;
            ctx->outermost_quote_indent = 0;
        }
        break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (!ctx->list_counter.empty()) {
            ctx->list_counter.pop_back();
        }
        if (ctx->indent_level > 0) {
            ctx->indent_level--;
        }
        break;

    case MD_BLOCK_TABLE:
        if (auto* const cn = ctx->current_node; cn && cn->has_table()) {
            FinalizeTableOffsets(*cn->table_data());
        }
        ctx->EndNode();
        break;

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
    case MD_BLOCK_TR:
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        // セル退出後は table ノード自体への AppendDoc は想定されないため nullptr に倒す。
        // 次の TR/TD/TH 進入で再設定される。
        ctx->active_text_buffer = nullptr;
        break;

    case MD_BLOCK_H:
        if (auto* const cn = ctx->current_node; cn && cn->type == NodeType::Heading) {
            // アンカー生成に確定済みテキストが必要なので先に Finalize する。
            ctx->FinalizeCurrentNode();

            AssignHeadingAnchor(ctx, *cn);
            ctx->heading_indices.emplace_back(ctx->current_node_index);
        }
        ctx->ClearCurrentNode();
        break;

    case MD_BLOCK_P:
        if (!TryPromoteParagraphToImage(ctx)) {
            TryPromoteParagraphToDisplayMath(ctx);
        }
        ctx->EndNode();
        break;

    case MD_BLOCK_LI:
    case MD_BLOCK_HR:
        ctx->EndNode();
        break;

    case MD_BLOCK_DOC:
    case MD_BLOCK_HTML:
        break;

    default:
        std::unreachable();
    }

    return 0;
}

// "> [!NOTE]\n> ![img](p.png)" は md4c では 1 段落になり、画像段落として丸ごと Image に昇格すると
// Alert 判定の対象 (引用ノード) が消える。マーカーだけのノードをここで閉じ、画像は同じ引用グループの
// 次の段落で受けて、マーカーと画像が別段落の場合と同じ構造にする (Alert は後続ノードへ伝播する)。
void SplitAlertMarkerBeforeImage(ParseContext* ctx)
{
    if (!ctx->current_node || !IsAlertHeadCandidate(ctx->nodes, ctx->current_node_index) ||
        !IsAlertMarkerOnly(ctx->current_text)) {
        return;
    }
    // [![badge](b.svg)](url) では MD_SPAN_A が先に来て URL を閉じるノードへ登録済みなので、新ノードへ登録し直す。
    std::pmr::string link_url;
    if (ctx->current_link_url_index >= 0) {
        link_url = ctx->current_node->view_link_urls()[static_cast<size_t>(ctx->current_link_url_index)];
    }
    ctx->EndNode();
    ctx->BeginParagraphNode();
    if (!link_url.empty()) {
        ctx->ResolveLinkUrlIndex(link_url);
    }
}

int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    ctx->EnsureNodeForListItemText();
    ctx->FlushPendingRun();
    // src が空の画像は Image ノードに昇格しないので、分割せず Alert 本文のままにする。
    if (type == MD_SPAN_IMG && ctx->image_span_depth == 0 && static_cast<const MD_SPAN_IMG_DETAIL*>(detail)->src.size > 0) {
        SplitAlertMarkerBeforeImage(ctx);
    }
    // span markup (** _ ` [] 等) は原文にあるが current_text には入らないため、
    // どの span でも view 化は構造的に失敗する。FinalizeCurrentNode の memcmp をスキップさせる。
    ctx->current_node_owned_only = true;

    // LATEXMATH_DISPLAY のみ「最初の display math は昇格対象 (other_content を立てない)」
    // という特殊扱いがあるため、ここで一律に立てるのは他の span 全種類。
    if (type != MD_SPAN_LATEXMATH_DISPLAY) {
        ctx->paragraph_has_other_content = true;
    }

    const auto enter_span_flag = [ctx](uint8_t& counter, uint8_t flag) {
        ++counter;
        ctx->current_run_flags |= flag;
    };
    switch (type) {
    case MD_SPAN_STRONG:
        enter_span_flag(ctx->bold_count, TextRun::kBold);
        break;
    case MD_SPAN_EM:
        enter_span_flag(ctx->italic_count, TextRun::kItalic);
        break;
    case MD_SPAN_CODE:
        enter_span_flag(ctx->code_count, TextRun::kCode);
        break;
    case MD_SPAN_DEL:
        enter_span_flag(ctx->strikethrough_count, TextRun::kStrikethrough);
        break;
    case MD_SPAN_A: {
        auto* const a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        ctx->ResolveLinkUrlIndex(std::string_view{ a->href.text, static_cast<size_t>(a->href.size) });
        break;
    }
    case MD_SPAN_IMG: {
        auto* const img = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        if (++ctx->image_span_depth == 1 && img->src.text && img->src.size > 0) {
            ctx->pending_image_src.assign(img->src.text, static_cast<size_t>(img->src.size));
        }
        break;
    }
    case MD_SPAN_LATEXMATH_DISPLAY:
        // "$$" はフォールバックテキスト用。昇格対象でなくなった時点で has_other_content を立てる
        ctx->in_display_math = true;
        ctx->AppendDoc("$$");
        if (ctx->paragraph_display_math_count == 0 && !ctx->paragraph_has_other_content) {
            ctx->display_math_buf.clear();
        }
        else {
            ctx->paragraph_has_other_content = true;
        }
        break;
    case MD_SPAN_LATEXMATH:
        // インライン $...$ は昇格対象外。元の "$" を復元してテキストとして残す
        ctx->AppendDoc("$");
        break;
    case MD_SPAN_WIKILINK:
    case MD_SPAN_U:
        break;
    default:
        std::unreachable();
    }

    return 0;
}

void LeaveImageSpan(ParseContext* ctx)
{
    // ネスト画像では最外側の src だけを採用する。
    if (--ctx->image_span_depth != 0) {
        return;
    }
    // Table / Heading で ensure_image を呼ぶと variant の既存データが破壊され、
    // テーブルでは active_text_buffer がダングリングになるため、Image 昇格候補と
    // リスト項目 (型を保ったまま src を持つ既存仕様) に限定する。
    if (auto* const cn = ctx->current_node;
        cn && !ctx->pending_image_src.empty() &&
        (cn->type == NodeType::Paragraph || cn->type == NodeType::BlockQuote || IsListItem(*cn))) {
        cn->ensure_image()->src = ctx->pending_image_src;
    }
    ctx->pending_image_src.clear();
}

int OnLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    ctx->FlushPendingRun();

    // md4c は enter/leave が常にバランスする契約なのでアンダーフローは起きない。
    const auto leave_span_flag = [ctx](uint8_t& counter, uint8_t flag) {
        if (--counter == 0) {
            ctx->current_run_flags &= static_cast<uint8_t>(~flag);
        }
    };
    switch (type) {
    case MD_SPAN_STRONG:
        leave_span_flag(ctx->bold_count, TextRun::kBold);
        break;
    case MD_SPAN_EM:
        leave_span_flag(ctx->italic_count, TextRun::kItalic);
        break;
    case MD_SPAN_CODE:
        leave_span_flag(ctx->code_count, TextRun::kCode);
        break;
    case MD_SPAN_DEL:
        leave_span_flag(ctx->strikethrough_count, TextRun::kStrikethrough);
        break;
    case MD_SPAN_A:
        ctx->current_link_url_index = -1;
        break;
    case MD_SPAN_IMG:
        LeaveImageSpan(ctx);
        break;
    case MD_SPAN_LATEXMATH_DISPLAY:
        ctx->in_display_math = false;
        ctx->AppendDoc("$$");
        ctx->paragraph_display_math_count++;
        break;
    case MD_SPAN_LATEXMATH:
        ctx->AppendDoc("$");
        break;
    case MD_SPAN_WIKILINK:
    case MD_SPAN_U:
        break;
    default:
        std::unreachable();
    }

    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    if (!ctx->current_node) [[unlikely]] {
        if (type == MD_TEXT_HTML || type == MD_TEXT_NULLCHAR || !ctx->EnsureNodeForListItemText()) {
            return 0;
        }
    }

    // 各ノードの最初のテキストコールバックでソースオフセットを記録する。
    if (!ctx->current_node->HasSourceOffset()) [[unlikely]] {
        ctx->RecordSourceOffset(text);
    }

    const std::string_view chunk{ text, static_cast<size_t>(size) };

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        ctx->NoteNonMathContent();
        ctx->AppendDoc(chunk);
        break;

    case MD_TEXT_LATEXMATH:
        ctx->AppendDoc(chunk);
        if (ctx->in_display_math && ctx->paragraph_display_math_count == 0 &&
            !ctx->paragraph_has_other_content) {
            // size==1 のとき md4c が \n をそのまま渡してくるケースが多いのでスカラ比較で済ませ、
            // size>1 のときだけ std::ranges::count にフォールバックする両対応。
            if (chunk.size() == 1) {
                ctx->display_math_newlines += (chunk[0] == '\n');
            }
            else {
                ctx->display_math_newlines += static_cast<int32_t>(std::ranges::count(chunk, '\n'));
            }
            ctx->display_math_buf.append(chunk);
        }
        break;

    case MD_TEXT_ENTITY: {
        ctx->NoteNonMathContent();
        // entity (`&amp;` 等) は現状文字に解決される。原文 (`&amp;`) と current_text (`&`) が
        // 不一致になり view 化失敗確定。memcmp スキップフラグを立てる。
        ctx->current_node_owned_only = true;
        char entity_buf[4];
        if (const auto resolved = ResolveHtmlEntity(chunk, entity_buf)) {
            ctx->AppendDoc(*resolved);
        }
        else {
            ctx->AppendDoc(chunk);
        }
        break;
    }

    case MD_TEXT_BR:
        ctx->NoteNonMathContent();
        // BR / SOFTBR は原文の `<br>` や 2 個の半角空白+改行を `\n`/` ` に置換するため raw_slice 不一致。
        ctx->current_node_owned_only = true;
        ctx->AppendDoc("\n");
        break;

    case MD_TEXT_SOFTBR:
        ctx->NoteNonMathContent();
        ctx->current_node_owned_only = true;
        ctx->AppendDoc(" ");
        break;

    case MD_TEXT_NULLCHAR:
    case MD_TEXT_HTML:
        break;

    default:
        std::unreachable();
    }

    return 0;
}

// 各種予約サイズのヒント定数。実測 (100MB 入力 = 30k ノード相当) を基準に、
// 初期確保サイズと再確保回数のバランスで決めている。値は「入力 N byte あたり 1 個」を表す:
//   - kScratchInputBytesPerByte=8 → ノード当たりの平均テキスト長 ~8B 想定の scratch。
//   - kInputBytesPerNode=256      → example/test.md 実測 ~258 byte/ノード。超過時は通常の伸長に任せる。
//   - kInputBytesPerHeading=4096  → 1MB あたり 256 個の見出し相当。実測数十〜数百に収まる。
//   - kInputBytesPerImage=512     → 画像頻度 ~0.2%。
//   - kInputBytesPerBlockquote=512 → blockquote 頻度 (image と同程度)。
//   - kInputBytesPerDiagram=1024  → ダイアグラム頻度 ~0.1%。
//   - kInputBytesPerTable=1024    → テーブル頻度 (ダイアグラムと同程度を想定)。
constexpr size_t kScratchInputBytesPerByte = 8;
constexpr size_t kInputBytesPerNode = 256;
constexpr size_t kInputBytesPerHeading = 4096;
constexpr size_t kInputBytesPerImage = 512;
constexpr size_t kInputBytesPerBlockquote = 512;
constexpr size_t kInputBytesPerDiagram = 1024;
constexpr size_t kInputBytesPerTable = 1024;
constexpr size_t kScratchReserveMin = 1024;
constexpr size_t kScratchReserveMax = 64 * 1024;

void ReserveForInput(ParseContext& ctx, size_t input_size)
{
    ctx.current_text.reserve(std::clamp(input_size / kScratchInputBytesPerByte, kScratchReserveMin, kScratchReserveMax));
    const size_t nodes_reserve = std::max(input_size / kInputBytesPerNode, 64uz);
    ctx.nodes.reserve(nodes_reserve);
    ctx.list_counter.reserve(8);
    MENDO_STATF("nodes.reserve: input={} reserve={}", input_size, nodes_reserve);
    // heading_indices と anchor_counts は 1 見出し 1 エントリで対になるので同じヒントを使う。
    const size_t heading_hint = std::clamp(input_size / kInputBytesPerHeading, 8uz, 256uz);
    ctx.heading_indices.reserve(heading_hint);
    ctx.anchor_counts.reserve(heading_hint);
    ctx.image_indices.reserve(std::clamp(input_size / kInputBytesPerImage, 4uz, 256uz));
    ctx.diagram_indices.reserve(std::clamp(input_size / kInputBytesPerDiagram, 4uz, 128uz));
    ctx.table_indices.reserve(std::clamp(input_size / kInputBytesPerTable, 4uz, 128uz));
    ctx.blockquote_indices.reserve(std::clamp(input_size / kInputBytesPerBlockquote, 4uz, 256uz));
}

} // namespace

ParseResult ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token)
{
    MENDO_PROFILE("ParseMarkdown");
    const size_t input_size = markdown_text.size();
    ParseContext ctx;
    ctx.stop_token = std::move(stop_token);
    ctx.markdown_base = markdown_text.data();
    ctx.markdown_size = input_size;
    ReserveForInput(ctx, input_size);

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;

    {
        MENDO_PROFILE("md_parse");
        md_parse(markdown_text.data(), static_cast<MD_SIZE>(markdown_text.size()), &parser, &ctx);
        // 最後の current_node が残っていれば（典型的には全 OnLeaveBlock で処理済みだが
        // 安全のため）テキストを確定する。
        ctx.FinalizeCurrentNode();
    }

    // キャンセル時は中途半端な nodes に対する DetectAlerts は無意味なので skip し、
    // 呼び出し側がチェックしやすいよう空 ParseResult を返す。
    if (ctx.cancel_requested) {
        return {};
    }

    {
        MENDO_PROFILE("DetectAlerts");
        DetectAlerts(ctx.nodes, std::span<const size_t>{ ctx.blockquote_indices });
    }

    ParseResult result;
    result.nodes = std::move(ctx.nodes);
    result.heading_indices = std::move(ctx.heading_indices);
    result.image_indices = std::move(ctx.image_indices);
    result.diagram_indices = std::move(ctx.diagram_indices);
    result.table_indices = std::move(ctx.table_indices);
    return result;
}
