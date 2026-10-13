#include "parser.h"
#include "html_entities.h"
#include "syntax.h"
#include "profiler.h"
#include "utility.h"
#include "md4c.h"
#include <cassert>
#include <unordered_map>
#include <algorithm>
#include <format>
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

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
    // 原文をそのまま使えないテキストと、runs・リンク URL の置き場所。ParseResult 経由で Document に渡す。
    std::unique_ptr<TextArena> arena;

    // パース後の全ノード走査を避けるため、特殊ノードのインデックスはパース中に構築する。
    std::pmr::vector<TocEntry> toc_entries;
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> diagram_indices;
    std::pmr::vector<size_t> table_indices;
    std::pmr::vector<size_t> blockquote_indices;
    std::pmr::vector<int32_t> footnote_def_indices;
    std::pmr::vector<FootnoteRef> footnote_refs;
    // [id - 1] が脚注 id への参照数。md4c の ref_id はテーブル行の判定でも数えられて飛ぶので自前で数える。
    std::pmr::vector<uint32_t> footnote_ref_counts;
    // 最後に記録したソース位置。RecordSourceOffset がこれより前の位置を記録しないために持つ。
    size_t last_source_offset = 0;
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
    // 現在の <a> span に対応する current_link_urls のインデックス。-1 = リンク外。
    // CommonMark で <a> はネスト禁止のため leave までこの値が安定する。
    int16_t current_link_url_index = -1;

    // 現在ノード用のテキストと runs の蓄積スクラッチ (UTF-8)。FinalizeCurrentNode で raw_text_ を
    // 指すか arena へコピーされる。runs は伸長のたびに確保し直さないよう、ここに貯めて 1 回だけコピーする。
    std::pmr::string current_text;
    std::pmr::vector<TextRun> current_runs;

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

    // 現在ノードのリンク URL。各 URL は raw_text_ か arena 上にあり、FinalizeCurrentNode で配列だけ arena へコピーする。
    std::pmr::vector<std::string_view> current_link_urls;
    // current_link_urls の URL -> インデックス索引。URL 数が kLinkUrlLinearScanMax を
    // 超えたノードでのみ構築する (テーブルは全セルで 1 ノードのため数万 URL になりうる)。
    static constexpr size_t kLinkUrlLinearScanMax = 8;
    std::pmr::unordered_map<std::string_view, int16_t> link_url_lookup;

    // ネスト画像 (![a ![b](inner)](outer)) では最外側の src を採用するため深度を追跡する。
    int image_span_depth = 0;

    // display math スパンが 1 個だけで他の内容が無い段落を LatexMath コードブロックに昇格する状態
    int paragraph_display_math_count = 0;
    bool paragraph_has_other_content = false;

    // md_parse() に渡した入力バッファ。source_offset 計算と、md4c が渡すポインタが原文を指すかの判定に使う。
    std::string_view markdown;

    // 現在ノードのテキストを arena へコピーすることが確定しているか。span markup (** _ ` 等) の存在や、
    // entity 解決 / BR / SOFTBR で text を置換するケースは current_text と raw_slice が
    // 構造的に一致しなくなるため、FinalizeCurrentNode の memcmp を完全にスキップできる。
    bool current_node_needs_copy = false;

    // current_text が source の (offset, current_text.size()) 範囲とバイト一致するか。
    // 一致するノードは arena にコピーせず raw_text_ を指せる。
    // span マークアップ (** _ ` 等) や BR/SOFTBR/ENTITY 混在のノードは不一致で arena へのコピーに落ちる。
    // offset は呼び出し側で SourceOffset() から取り出した値を渡す (FinalizeCurrentNode で再利用するため)。
    bool CurrentTextMatchesRawSliceAt(size_t offset) const noexcept
    {
        // 高頻度呼び出し (100MB で 40万回超) のため MENDO_PROFILE は外す。
        // zone overhead が ~120ms 単位で計測自体を歪めるため。
        const size_t len = current_text.size();
        if (offset + len > markdown.size()) {
            return false;
        }
        // current_node_needs_copy で span/entity 混在は事前に弾けるため、ここまで来たノードは大半が
        // 全長一致 (success) になる。probe 短絡は worst-case で len 分の比較が走り意味がないため、
        // 1 回の memcmp に統一する。
        return std::char_traits<char>::compare(markdown.data() + offset, current_text.data(), len) == 0;
    }

    // 現在ノードのテキストと runs のスクラッチを Node に確定し、スクラッチをクリアする。
    // BeginNode 直前と各 OnLeaveBlock の current_node 解除直前に呼ぶ。
    // スクラッチの capacity は保持して次ノードで再利用する (確保回数削減のため)。
    void FinalizeCurrentNode()
    {
        FlushPendingRun();
        if (current_node) {
            // 昇格処理 (TryPromoteParagraphToDisplayMath 等) がテキストを直接設定済みのノードは、
            // current_text で上書きすると line_count ごと巻き戻るのでスキップする。
            if (!current_text.empty() && !current_node->HasText()) {
                const auto offset = current_node->SourceOffset();
                const bool matches_raw = !current_node_needs_copy && offset != kUnsetSourceOffset && CurrentTextMatchesRawSliceAt(offset);
                const std::string_view text = matches_raw
                    ? markdown.substr(offset, current_text.size())
                    : arena->CopyText(current_text);
                current_node->SetText(text);
            }
            if (!current_runs.empty()) {
                current_node->SetRuns(arena->Copy(current_runs));
            }
            if (!current_link_urls.empty()) {
                current_node->SetLinkUrls(arena->Copy(current_link_urls));
            }
        }
        current_text.clear();
        current_runs.clear();
        current_link_urls.clear();
        link_url_lookup.clear();
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
        current_node_needs_copy = false;
    }

    // md4c は MD_TEXT_NULLCHAR / MD_TEXT_BR / MD_TEXT_SOFTBR や、CODE/LATEXMATH/HTML の改行・空白置換で
    // _T(""), _T("\n"), _T(" ") といった内部静的リテラルを text として渡すことがある。
    // 範囲外 (静的リテラル) のときは記録せず、後続の実体テキストで設定できるようにする。
    // 脚注定義のように原文の途中から文書末尾へ移るノードは、ソース位置の単調増加
    // (FindNodeBySourceOffset の二分探索の前提) を崩すので記録しない。
    void RecordSourceOffset(std::string_view chunk) noexcept
    {
        if (!IsSubrangeOf(chunk, markdown)) {
            return;
        }
        const auto offset = static_cast<size_t>(chunk.data() - markdown.data());
        if (offset < last_source_offset) {
            return;
        }
        current_node->SetSourceOffset(offset);
        last_source_offset = offset;
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

    // url を current_link_urls に登録し、インデックスを current_link_url_index にキャッシュする。
    // 1 ノードあたりの URL 数は典型的に < 8 で、その領域ではハッシュ計算を伴う
    // ハッシュマップより線形 memcmp の方が速い。数が増えたノードだけ link_url_lookup に切り替え、
    // テーブル等で O(n^2) になるのを防ぐ。
    void ResolveLinkUrlIndex(std::string_view url)
    {
        current_link_url_index = (current_node && !url.empty()) ? FindOrAddLinkUrl(url) : int16_t{ -1 };
    }

    int16_t FindOrAddLinkUrl(std::string_view url)
    {
        const size_t n = current_link_urls.size();
        if (n <= kLinkUrlLinearScanMax) {
            for (size_t i = 0; i < n; ++i) {
                if (current_link_urls[i] == url) {
                    return static_cast<int16_t>(i);
                }
            }
        }
        else {
            if (link_url_lookup.empty()) {
                for (size_t i = 0; i < n; ++i) {
                    link_url_lookup.emplace(current_link_urls[i], static_cast<int16_t>(i));
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
        const auto new_index = static_cast<int16_t>(n);
        current_link_urls.push_back(InternUrl(url));
        if (!link_url_lookup.empty()) {
            link_url_lookup.emplace(current_link_urls.back(), new_index);
        }
        return new_index;
    }

    // md4c の href / 画像 src は原文を指すことが多いのでそのまま使う。参照定義の展開や脚注 URL のように
    // 原文の外にあるものだけ arena へコピーする。
    std::string_view InternUrl(std::string_view url)
    {
        return IsSubrangeOf(url, markdown) ? url : arena->CopyText(url);
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
    // 装飾もリンクも無い run は出さない。runs の隙間は既定スタイルとして扱われる。
    constexpr void FlushPendingRun()
    {
        if (has_pending_run) {
            std::pmr::string* const buf = active_text_buffer;
            if (buf && buf->size() > pending_run_start) {
                const uint32_t length = static_cast<uint32_t>(buf->size() - pending_run_start);
                const bool plain = current_run_flags == 0 && current_link_url_index < 0;
                if (buf == &current_text) {
                    current_node->line_count += pending_run_newlines;
                    if (!plain) {
                        current_runs.push_back(MakeRun(pending_run_start, length));
                    }
                }
                else if (!plain) {
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

// 画像 src を受け取り Image ノードへ昇格できる種別。
// 他の種別は extra に別の alternative を持つため ensure_image で壊れる (issue#298)。
constexpr bool IsImagePromotionCandidate(const Node& node) noexcept
{
    return node.type == NodeType::Paragraph || node.type == NodeType::BlockQuote;
}

// MD_BLOCK_P 終了時、画像 span を含む段落 / 引用ブロックを Image ノードへ昇格させる。
// 戻り値 true なら他の昇格処理はスキップしてよい。
constexpr bool TryPromoteParagraphToImage(ParseContext* ctx)
{
    auto* const cn = ctx->current_node;
    if (!cn || !IsImagePromotionCandidate(*cn) || !cn->has_image() || cn->image_data()->src.empty()) {
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
    // 昇格できる段落のテキストは "$$" + 数式 + "$$" だけ。md4c は数式内の改行を空白にして渡すので改行は無い。
    const std::string_view text = ctx->current_text;
    if (ctx->paragraph_display_math_count != 1 ||
        ctx->paragraph_has_other_content ||
        text.size() <= 4) {
        return false;
    }
    assert(text.starts_with("$$") && text.ends_with("$$"));
    node->type = NodeType::CodeBlock;
    node->set_code_language(SyntaxLanguage::LatexMath);
    ctx->current_runs.clear();
    node->SetText(ctx->arena->CopyText(text.substr(2, text.size() - 4)), 0);
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
    // md4c から正確なテーブルサイズが渡されるので、要素数が決まる配列は一度に reserve して
    // 余りを持たせない。セル本文の長さは分からないので concat_text は伸長に任せる。
    const size_t total_rows = static_cast<size_t>(detail->head_row_count) + detail->body_row_count;
    tbl->col_count = static_cast<uint16_t>(std::min<unsigned>(detail->col_count, std::numeric_limits<uint16_t>::max()));
    const size_t total_cells = total_rows * tbl->col_count;
    tbl->cell_text_starts.reserve(total_cells + 1);
    tbl->cell_run_starts.reserve(total_cells + 1);
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

// md4c は参照の中身をテキストで渡さないので、GitHub と同じく脚注番号を定義へのリンクとして補う。
void AppendFootnoteRef(ParseContext* ctx, const MD_SPAN_FOOTNOTE_REF_DETAIL* ref)
{
    if (!ctx->current_node) {
        return;
    }
    if (ref->id > ctx->footnote_ref_counts.size()) {
        ctx->footnote_ref_counts.resize(ref->id, 0);
    }
    const uint32_t ref_id = ++ctx->footnote_ref_counts[ref->id - 1];
    ctx->footnote_refs.push_back({ ref->id, ref_id, static_cast<int32_t>(ctx->current_node_index) });
    // リンク文字列内の参照でも外側のリンクが続くよう戻す。
    const int16_t outer_link = ctx->current_link_url_index;
    ctx->ResolveLinkUrlIndex(FootnoteDefUrl(ref->id));
    ctx->AppendDoc(std::format("[{}]", ref->id));
    ctx->FlushPendingRun();
    ctx->current_link_url_index = outer_link;
}

// 脚注定義は文書末尾で番号付きリストの項目にする。md4c は中身を MD_BLOCK_P 無しで、脚注番号順に渡す。
void BeginFootnoteDef(ParseContext* ctx, const MD_BLOCK_FOOTNOTE_DEF_DETAIL* def)
{
    ctx->BeginNode(NodeType::ListItem);
    auto* const ld = ctx->current_node->ensure_list();
    ld->ordered = true;
    ld->list_number = static_cast<int32_t>(def->id);
    ctx->footnote_def_indices.push_back(static_cast<int32_t>(ctx->current_node_index));
}

// GitHub と同じく、定義の末尾に参照へ戻るリンク "↩" (2 回目以降は "↩2" ...) を参照の数だけ付ける。
// 後の定義の中の参照は前の定義を閉じた後に届くので、パースを終えてからまとめて付ける。
void AppendFootnoteBackrefs(std::pmr::vector<Node>& nodes, TextArena& arena, std::span<const int32_t> def_indices, std::span<const uint32_t> ref_counts)
{
    constexpr size_t kMaxLinkUrls = std::numeric_limits<int16_t>::max();
    std::pmr::string text;
    std::pmr::vector<TextRun> runs;
    std::pmr::vector<std::string_view> urls;
    for (size_t i = 0; i < def_indices.size() && i < ref_counts.size(); ++i) {
        const auto id = static_cast<uint32_t>(i + 1);
        auto& node = nodes[static_cast<size_t>(def_indices[i])];
        text = node.GetText();
        runs.assign(node.runs().begin(), node.runs().end());
        urls.assign(node.view_link_urls().begin(), node.view_link_urls().end());
        for (uint32_t ref_id = 1; ref_id <= ref_counts[i] && urls.size() < kMaxLinkUrls; ++ref_id) {
            if (!text.empty()) {
                text += ' ';
            }
            urls.push_back(arena.CopyText(FootnoteRefUrl(id, ref_id)));
            TextRun link;
            link.start = static_cast<uint32_t>(text.size());
            link.link_url_index = static_cast<int16_t>(urls.size() - 1);
            text += "↩";
            if (ref_id > 1) {
                std::format_to(std::back_inserter(text), "{}", ref_id);
            }
            link.length = static_cast<uint32_t>(text.size()) - link.start;
            runs.push_back(link);
        }
        node.SetText(arena.CopyText(text));
        node.SetRuns(arena.Copy(runs));
        node.SetLinkUrls(arena.Copy(urls));
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
    // コード内には装飾もリンクも付かないので run は無く、調整は要らない。
    assert(ctx->current_runs.empty());
    if (!ctx->current_text.empty() && ctx->current_text.back() == '\n') {
        ctx->current_text.pop_back();
        cn->line_count--;
    }
    if (cn->code_language() == SyntaxLanguage::Mermaid) {
        ctx->diagram_indices.emplace_back(ctx->current_node_index);
    }
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

    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        ctx->BeginNode(NodeType::HorizontalRule);
        ctx->EndNode();
        ctx->indent_level++;
        break;

    case MD_BLOCK_FOOTNOTE_DEF:
        BeginFootnoteDef(ctx, static_cast<const MD_BLOCK_FOOTNOTE_DEF_DETAIL*>(detail));
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
            ctx->toc_entries.emplace_back(static_cast<int>(ctx->current_node_index), cn->heading_level());
        }
        ctx->EndNode();
        break;

    case MD_BLOCK_P:
        if (!TryPromoteParagraphToImage(ctx)) {
            TryPromoteParagraphToDisplayMath(ctx);
        }
        ctx->EndNode();
        break;

    case MD_BLOCK_LI:
    case MD_BLOCK_HR:
    case MD_BLOCK_FOOTNOTE_DEF:
        ctx->EndNode();
        break;

    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        if (ctx->indent_level > 0) {
            ctx->indent_level--;
        }
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
    std::string_view link_url;
    if (ctx->current_link_url_index >= 0) {
        link_url = ctx->current_link_urls[static_cast<size_t>(ctx->current_link_url_index)];
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
    // どの span でも原文と一致しなくなる。FinalizeCurrentNode の memcmp をスキップさせる。
    ctx->current_node_needs_copy = true;

    // display math は個数 (paragraph_display_math_count) で昇格を判定するので、ここで立てるのは他の span 全種類。
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
    case MD_SPAN_IMG:
        ++ctx->image_span_depth;
        break;
    case MD_SPAN_LATEXMATH_DISPLAY:
        // "$$" は昇格しなかったときのフォールバックテキスト用。
        ctx->AppendDoc("$$");
        break;
    case MD_SPAN_LATEXMATH:
        // インライン $...$ は昇格対象外。元の "$" を復元してテキストとして残す
        ctx->AppendDoc("$");
        break;
    case MD_SPAN_FOOTNOTE_REF:
        AppendFootnoteRef(ctx, static_cast<const MD_SPAN_FOOTNOTE_REF_DETAIL*>(detail));
        break;
    case MD_SPAN_WIKILINK:
    case MD_SPAN_U:
        break;
    default:
        std::unreachable();
    }

    return 0;
}

// md4c は leave でも enter と同じ src を渡すので、最外側の leave で受け取る。
void LeaveImageSpan(ParseContext* ctx, const MD_SPAN_IMG_DETAIL* img)
{
    if (--ctx->image_span_depth != 0 || img->src.size == 0) {
        return;
    }
    if (auto* const cn = ctx->current_node; cn && IsImagePromotionCandidate(*cn)) {
        cn->ensure_image()->src = ctx->InternUrl(std::string_view{ img->src.text, static_cast<size_t>(img->src.size) });
    }
}

int OnLeaveSpan(MD_SPANTYPE type, void* detail, void* userdata)
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
        LeaveImageSpan(ctx, static_cast<const MD_SPAN_IMG_DETAIL*>(detail));
        break;
    case MD_SPAN_LATEXMATH_DISPLAY:
        ctx->AppendDoc("$$");
        ctx->paragraph_display_math_count++;
        break;
    case MD_SPAN_LATEXMATH:
        ctx->AppendDoc("$");
        break;
    case MD_SPAN_FOOTNOTE_REF:
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

    const std::string_view chunk{ text, static_cast<size_t>(size) };

    // 各ノードの最初のテキストコールバックでソースオフセットを記録する。
    if (!ctx->current_node->HasSourceOffset()) [[unlikely]] {
        ctx->RecordSourceOffset(chunk);
    }

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        ctx->paragraph_has_other_content = true;
        ctx->AppendDoc(chunk);
        break;

    case MD_TEXT_LATEXMATH:
        ctx->AppendDoc(chunk);
        break;

    case MD_TEXT_ENTITY: {
        ctx->paragraph_has_other_content = true;
        // entity (`&amp;` 等) は現状文字に解決される。原文 (`&amp;`) と current_text (`&`) が
        // 不一致になり arena へのコピーが確定。memcmp スキップフラグを立てる。
        ctx->current_node_needs_copy = true;
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
        ctx->paragraph_has_other_content = true;
        // BR / SOFTBR は原文の `<br>` や 2 個の半角空白+改行を `\n`/` ` に置換するため raw_slice 不一致。
        ctx->current_node_needs_copy = true;
        ctx->AppendDoc("\n");
        break;

    case MD_TEXT_SOFTBR:
        ctx->paragraph_has_other_content = true;
        ctx->current_node_needs_copy = true;
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
//   - kInputBytesPerNode=128      → 1 ノード 128 byte まで細かい文書でもパース途中の再確保 (全ノードの移動) を
//                                   避ける (example/test.md 実測 ~258 byte/ノード)。予約は入力と同程度のバイト数で、
//                                   触れない分もコミットは残るが物理メモリには載らない。超過時は通常の伸長に任せる。
//   - kInputBytesPerArenaByte=3   → arena の初回確保。example/test.md の実測では arena に置くテキスト・runs・URL が
//                                   入力の約 3 割で、典型的な文書は 1 回の確保で収まる。足りない分は
//                                   monotonic_buffer_resource が 1.5 倍ずつ大きくしながら確保する (MSVC)。
//                                   原文をそのまま使うノードが多い文書では余るが、触れない分は物理メモリに載らない。
// 見出し・画像などのインデックス配列は件数が少なく、通常の伸長で足りるので予約しない。
constexpr size_t kScratchInputBytesPerByte = 8;
constexpr size_t kInputBytesPerNode = 128;
constexpr size_t kInputBytesPerArenaByte = 3;
constexpr size_t kScratchReserveMin = 1024;
constexpr size_t kArenaReserveMin = 1024;
constexpr size_t kScratchReserveMax = 64 * 1024;

void ReserveForInput(ParseContext& ctx, size_t input_size)
{
    ctx.arena = std::make_unique<TextArena>(std::max(input_size / kInputBytesPerArenaByte, kArenaReserveMin));
    ctx.current_text.reserve(std::clamp(input_size / kScratchInputBytesPerByte, kScratchReserveMin, kScratchReserveMax));
    const size_t nodes_reserve = std::max(input_size / kInputBytesPerNode, 64uz);
    ctx.nodes.reserve(nodes_reserve);
    ctx.list_counter.reserve(8);
    MENDO_STATF("nodes.reserve: input={} reserve={}", input_size, nodes_reserve);
}

} // namespace

ParseResult ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token)
{
    MENDO_PROFILE("ParseMarkdown");
    const size_t input_size = markdown_text.size();
    ParseContext ctx;
    ctx.stop_token = std::move(stop_token);
    ctx.markdown = markdown_text;
    ReserveForInput(ctx, input_size);

    MD_PARSER parser{};
    parser.abi_version = 0;
    // Alert は引用ノードから自前で検出する。md4c の admonitions はネストした引用でも認識する等、GitHub と挙動が違うので外す。
    parser.flags = (MD_DIALECT_GITHUB & ~MD_FLAG_ADMONITIONS) | MD_FLAG_LATEXMATHSPANS;
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

    AppendFootnoteBackrefs(ctx.nodes, *ctx.arena, ctx.footnote_def_indices, ctx.footnote_ref_counts);

    {
        MENDO_PROFILE("DetectAlerts");
        DetectAlerts(ctx.nodes, *ctx.arena, std::span<const size_t>{ ctx.blockquote_indices });
    }

    ParseResult result{ NodeStorage{ std::move(ctx.nodes), std::move(ctx.arena) } };
    result.toc_entries = std::move(ctx.toc_entries);
    result.image_indices = std::move(ctx.image_indices);
    result.diagram_indices = std::move(ctx.diagram_indices);
    result.table_indices = std::move(ctx.table_indices);
    result.footnote_def_indices = std::move(ctx.footnote_def_indices);
    result.footnote_refs = std::move(ctx.footnote_refs);
    return result;
}
