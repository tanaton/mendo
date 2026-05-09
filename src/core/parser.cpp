#include "parser.h"
#include "ascii_util.h"
#include "document_utils.h"
#include "syntax.h"
#include "memory_resource.h"
#include "profiler.h"
#include "utility.h"
#include "utf8_codec.h"
#include "md4c.h"
#include <cstring>
#include <functional>
#include <unordered_map>
#include <charconv>
#include <climits>
#include <format>
#include <iterator>
#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

// current_text スクラッチの初期確保サイズ。入力サイズから動的に決める。
constexpr size_t SCRATCH_RESERVE_MIN = 1024;
constexpr size_t SCRATCH_RESERVE_MAX = 64 * 1024;

struct ParseContext {
    explicit ParseContext(size_t initial_arena_bytes)
        : parse_resource{ initial_arena_bytes }
    {}

    // パース用 monotonic リソース（一括確保→一括解放）。初期サイズは入力に応じて動的に決定する。
    MonotonicResource parse_resource;
    // append/clear が走るスクラッチや hash map 用の pool。単一スレッドなので unsynchronized。
    // upstream を monotonic にして pool 自身の解放は ParseContext 破棄時の一括解放に任せる。
    std::pmr::unsynchronized_pool_resource pool{ parse_resource.resource() };

    std::pmr::vector<Node> nodes;

    // パース中に構築する特殊ノードインデックス（BuildIndicesのO(n)走査を除去）
    std::pmr::vector<size_t> heading_indices;
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> diagram_indices;
    // DetectAlerts 後に破棄するため parse_resource に載せられる（他 indices は ParseResult 経由で持ち出すので不可）。
    std::pmr::vector<size_t> blockquote_indices{ parse_resource.resource() };
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

    // 現在ノード用のテキスト蓄積スクラッチ (UTF-8)。FinalizeCurrentNode で Node::text_ へ view コピーされる
    // (allocator 不一致を避けるため move ではなくコピー)。
    std::pmr::string current_text{ &pool };

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
    std::pmr::vector<int> list_counter{ parse_resource.resource() };

    // テーブル追跡
    bool in_table = false;
    bool in_thead = false;
    // セル内かどうか (AppendDoc / FlushPendingRun の振り分けに使う)。
    bool in_table_cell = false;
    // 現在セルの concat_text 内開始 offset。run.start を cell-local に保つために保持する。
    uint32_t current_table_cell_text_start = 0;

    // 現在構築中のノード
    Node* current_node = nullptr;

    // AppendDoc / FlushPendingRun のターゲットバッファのキャッシュ。
    // 47-94 万回呼ばれる hot path で in_table_cell + has_table() の variant 判定を
    // 毎回行わないよう、状態遷移点 (BeginNode / TD/TH 進入退出 / current_node clear) で
    // 更新したポインタを直接使う。nullptr のときは AppendDoc / FlushPendingRun は no-op。
    std::pmr::string* active_text_buffer = nullptr;

    // アンカーIDの一意性追跡: スラグ -> 出現回数。
    // 再ハッシュ時の旧 bucket は pool 内で再利用されるため monotonic は膨らまない。
    std::pmr::unordered_map<std::pmr::string, int, mendo::StringTransparentHash, std::equal_to<>> anchor_counts{ &pool };

    // 画像スパンの src 蓄積バッファ。NodeImageData::src へは allocator 不一致を避けるため assign(view) でコピー。
    std::pmr::string pending_image_src{ &pool };

    // display math スパンが 1 個だけで他の内容が無い段落を LatexMath コードブロックに昇格する状態
    bool in_display_math = false;
    int paragraph_display_math_count = 0;
    bool paragraph_has_other_content = false;
    std::pmr::string display_math_buf{ &pool };
    // display_math_buf に append された範囲の改行数。昇格時の line_count 設定に使い、
    // current_text 全体を std::ranges::count で走査するコストを避ける。
    int32_t display_math_newlines = 0;

    // md_parse() に渡した入力バッファ。source_offset 計算と OnText の text ポインタ範囲判定に使う。
    const char* markdown_base = nullptr;
    size_t markdown_size = 0;

    // 現在ノードの source_offset が既に設定済みか。
    // OnText の uint32 比較 (current_node->source_offset == kUnsetSourceOffset) を毎テキストで
    // 評価する代わりに、BeginNode でリセット → 範囲内マッチで設定 → 以降は素通しでよい。
    bool node_source_offset_set = false;

    // 現在ノードが owned 経路確定か。span markup (** _ ` 等) の存在や、
    // entity 解決 / BR / SOFTBR で text を置換するケースは current_text と raw_slice が
    // 構造的に一致しなくなるため、FinalizeCurrentNode の memcmp を完全にスキップできる。
    bool current_node_owned_only = false;

    // current_text が source の (source_offset, current_text.size()) 範囲とバイト一致するか。
    // 一致するノードは Node::owned_text_ を確保せず raw_text_ への view に倒せる。
    // span マークアップ (** _ ` 等) や BR/SOFTBR/ENTITY 混在のノードは不一致で owned 経路に落ちる。
    bool CurrentTextMatchesRawSlice() const noexcept
    {
        // 高頻度呼び出し (100MB で 40万回超) のため MENDO_PROFILE は外す。
        // zone overhead が ~120ms 単位で計測自体を歪めるため。
        if (!current_node || current_node->source_offset == kUnsetSourceOffset) {
            return false;
        }
        const size_t offset = current_node->source_offset;
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
            if (!current_node_owned_only && CurrentTextMatchesRawSlice()) {
                current_node->SetTextView(
                    static_cast<uint32_t>(current_node->source_offset),
                    static_cast<uint32_t>(current_text.size()),
                    current_node->line_count,
                    markdown_base);
            }
            else {
                current_node->SetTextWithLineCount(current_text, current_node->line_count);
            }
        }
        current_text.clear();
    }

    // OnLeaveBlock (TABLE / H / P / LI / HR) でノード構築を終えるときの後処理。
    // current_node と active_text_buffer は対で管理する不変式があるため一括で nullptr に倒す。
    constexpr void ClearCurrentNode() noexcept
    {
        current_node = nullptr;
        active_text_buffer = nullptr;
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
        // runs は SBO=4 で初期確保ゼロを狙う。reserve すると SBO の利点が消えるので呼ばない。
        node_source_offset_set = false;
        current_node_owned_only = false;
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
    // 1 ノードあたりの URL 数は典型的に < 8 なので、ハッシュマップではなく線形探索する。
    // ハッシュマップにはキーの string 複製・per-node clear()・URL 文字列ハッシュの
    // コストがあり、N が小さい領域では線形 memcmp の方が速い。脚注で urls 数が増えても
    // 比較は string_view 同士なので allocator 確保を伴わない。
    constexpr void ResolveLinkUrlIndex(std::string_view url)
    {
        if (!current_node || url.empty()) {
            current_link_url_index = -1;
            return;
        }
        const auto existing = current_node->view_link_urls();
        const size_t n = existing.size();
        for (size_t i = 0; i < n; ++i) {
            if (std::string_view{ existing[i] } == url) {
                current_link_url_index = static_cast<int16_t>(i);
                return;
            }
        }
        auto& urls = current_node->ensure_link_urls();
        const int16_t new_index = static_cast<int16_t>(urls.size());
        urls.emplace_back(url);
        current_link_url_index = new_index;
    }

    // テキストを現在のノードまたはセルに追加する (UTF-8)。
    // 同じ span 状態で連続して呼ばれると 1 つの TextRun に統合される (cell も統合対象)。
    // セル切替・span 切替・ブロック退出の各タイミングで FlushPendingRun が走る前提。
    // セル内では active_text_buffer が NodeTableData::concat_text を指す。pending_run_start は
    // バッファサイズベースだが、cell 内では current_table_cell_text_start からの相対 (cell-local) として扱う。
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
            pending_run_newlines += (c == mendo::doc_lf);
            target->push_back(c);
        }
        else {
            target->append(text);
        }
    }

    // 未確定 TextRun を確定して runs に push する。
    // span 状態が変わる直前 (OnEnter/Leave Span)、セル切替、OnLeaveBlock の冒頭で呼ぶ。
    // line_count はノード単位なので、セル内では更新しない。
    constexpr void FlushPendingRun()
    {
        if (has_pending_run) {
            std::pmr::string* const buf = active_text_buffer;
            if (buf && buf->size() > pending_run_start) {
                const uint32_t length = static_cast<uint32_t>(buf->size() - pending_run_start);
                if (in_table_cell && current_node && current_node->has_table()) {
                    const uint32_t cell_local_start = pending_run_start - current_table_cell_text_start;
                    current_node->table_data()->all_runs.push_back(MakeRun(cell_local_start, length));
                }
                else if (current_node) {
                    current_node->line_count += pending_run_newlines;
                    current_node->runs.emplace_back(MakeRun(pending_run_start, length));
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
    node->code_language = SyntaxLanguage::LatexMath;
    node->runs.clear();
    // current_text 経由で渡すと FinalizeCurrentNode で 2 回目のコピーが走るため、Node::text_ へ直接書く。
    node->SetTextWithLineCount(ctx->display_math_buf, ctx->display_math_newlines);
    ctx->diagram_indices.emplace_back(ctx->current_node_index);
    return true;
}

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_H: {
        auto* const h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        ctx->BeginNode(NodeType::Heading);
        ctx->current_node->heading_level = static_cast<int8_t>(h->level);
        break;
    }

    case MD_BLOCK_P:
        if (!ctx->in_code_block) {
            const auto node_type = (ctx->blockquote_depth > 0) ? NodeType::BlockQuote : NodeType::Paragraph;
            ctx->BeginNode(node_type);
            if (node_type == NodeType::BlockQuote) {
                ctx->blockquote_indices.emplace_back(ctx->current_node_index);
            }
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
            ctx->current_node->code_language = DetectLanguage(std::string_view{ code_detail->lang.text, static_cast<size_t>(code_detail->lang.size) });
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
        ctx->list_counter.push_back(0);
        ctx->indent_level++;
        break;

    case MD_BLOCK_OL: {
        auto* const ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        ctx->list_counter.push_back(static_cast<int>(ol->start));
        ctx->indent_level++;
        break;
    }

    case MD_BLOCK_LI: {
        auto* const li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (li->is_task) {
            ctx->BeginNode(NodeType::TaskListItem);
            ctx->current_node->task_checked = (li->task_mark == 'x' || li->task_mark == 'X');
        }
        else {
            ctx->BeginNode(NodeType::ListItem);
        }
        if (!ctx->list_counter.empty()) {
            const int counter = ctx->list_counter.back();
            ctx->current_node->list_number = counter;
            if (counter > 0) {
                ctx->list_counter.back()++;
            }
        }
        break;
    }

    case MD_BLOCK_HR:
        ctx->BeginNode(NodeType::HorizontalRule);
        break;

    case MD_BLOCK_TABLE: {
        ctx->BeginNode(NodeType::Table);
        // 後続の TR/TH/TD で nullable チェックなく参照できるよう先に確保する。
        // これに依存して TR/TH/TD は has_table() ガードを省いている。
        auto* tbl = ctx->current_node->ensure_table();
        // md4c から正確なテーブルサイズが渡されるので、各 vector を一度に reserve して
        // 巨大テーブル時の段階的 realloc (~quadratic コスト) を避ける。
        if (auto* td = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail); td) {
            const size_t total_rows = static_cast<size_t>(td->head_row_count) + td->body_row_count;
            tbl->col_count = static_cast<uint16_t>(std::min<unsigned>(td->col_count, std::numeric_limits<uint16_t>::max()));
            const size_t total_cells = total_rows * tbl->col_count;
            tbl->cell_text_starts.reserve(total_cells + 1);
            tbl->cell_run_starts.reserve(total_cells + 1);
            // 1 セル平均 16 wchar + 区切り 1 wchar の見積もり。
            tbl->concat_text.reserve(total_cells * 17);
            tbl->aligns.reserve(tbl->col_count);
            tbl->is_header_row.reserve(total_rows);
        }
        ctx->in_table = true;
        break;
    }

    case MD_BLOCK_THEAD:
        ctx->in_thead = true;
        break;

    case MD_BLOCK_TBODY:
        ctx->in_thead = false;
        break;

    case MD_BLOCK_TR:
        if (auto* cn = ctx->current_node; cn && cn->type == NodeType::Table) {
            ++cn->table_data()->row_count;
        }
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        if (auto* cn = ctx->current_node; cn && cn->type == NodeType::Table && cn->table_data()->row_count > 0) {
            auto* tbl = cn->table_data();
            // 行内セル数 (is_header_row エントリ数が現 row_count 未満なら未確定 = 行頭)。
            const bool first_cell_in_row = (tbl->is_header_row.size() < tbl->row_count);
            const bool first_row = (tbl->row_count == 1);
            // 区切り: 行内 2 セル目以降は '\t'、行頭かつ 2 行目以降は '\n'。
            if (!first_cell_in_row) {
                tbl->concat_text.push_back(mendo::doc_tab);
            }
            else if (!first_row) {
                tbl->concat_text.push_back(mendo::doc_lf);
            }
            tbl->cell_text_starts.push_back(static_cast<uint32_t>(tbl->concat_text.size()));
            tbl->cell_run_starts.push_back(static_cast<uint32_t>(tbl->all_runs.size()));
            ctx->current_table_cell_text_start = static_cast<uint32_t>(tbl->concat_text.size());
            ctx->in_table_cell = true;
            ctx->active_text_buffer = &tbl->concat_text;

            // 1 行目で列単位の align を確定 (列属性は header 行で決まる)。
            // col_count は MD_BLOCK_TABLE で md4c から取得済みなのでここでは触らない。
            if (first_row) {
                const auto align = detail ? static_cast<TableAlign>(static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align) : TableAlign::Default;
                tbl->aligns.push_back(align);
            }
            // 1 セル目で is_header_row を確定 (md4c は TR 内で TH/TD を混在させない)。
            if (first_cell_in_row) {
                tbl->is_header_row.push_back(type == MD_BLOCK_TH);
            }
        }
        break;
    }
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

    ctx->FlushPendingRun();

    switch (type) {
    case MD_BLOCK_CODE: {
        auto* cn = ctx->current_node;
        ctx->in_code_block = false;
        // 末尾の改行があれば除去（current_text スクラッチに対して操作）
        if (cn && !ctx->current_text.empty() && ctx->current_text.back() == mendo::doc_lf) {
            ctx->current_text.pop_back();
            cn->line_count--;
            if (!cn->runs.empty()) {
                auto& last = cn->runs.back();
                if (last.length > 0) {
                    last.length--;
                }
            }
        }
        if (cn && cn->code_language == SyntaxLanguage::Mermaid) {
            ctx->diagram_indices.emplace_back(ctx->current_node_index);
        }
        break;
    }
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
        // テーブル終了時に offset テーブルを R*C+1 サイズに揃える。
        // 行内セル数が col_count に満たない行は空セル扱い (offset は concat 末尾に詰めて padding)。
        if (auto* cn = ctx->current_node; cn && cn->has_table()) {
            auto* tbl = cn->table_data();
            const size_t expected_cells = static_cast<size_t>(tbl->row_count) * tbl->col_count;
            const auto text_end = static_cast<uint32_t>(tbl->concat_text.size());
            const auto run_end = static_cast<uint32_t>(tbl->all_runs.size());
            // padding (extend のみ; md4c が誤ってセル超過した場合に切り詰めない) と番兵末尾。
            if (tbl->cell_text_starts.size() < expected_cells) {
                tbl->cell_text_starts.resize(expected_cells, text_end);
                tbl->cell_run_starts.resize(expected_cells, run_end);
            }
            tbl->cell_text_starts.push_back(text_end);
            tbl->cell_run_starts.push_back(run_end);
            if (tbl->is_header_row.size() < tbl->row_count) {
                tbl->is_header_row.resize(tbl->row_count, false);
            }
            if (tbl->aligns.size() < tbl->col_count) {
                tbl->aligns.resize(tbl->col_count, TableAlign::Default);
            }
        }
        ctx->in_table = false;
        ctx->FinalizeCurrentNode();
        ctx->ClearCurrentNode();
        break;

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
    case MD_BLOCK_TR:
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        ctx->in_table_cell = false;
        // セル退出後は table ノード自体への AppendDoc は想定されないため nullptr に倒す。
        // 次の TR/TD/TH 進入で再設定される。
        ctx->active_text_buffer = nullptr;
        break;

    case MD_BLOCK_H:
        if (auto* cn = ctx->current_node; cn && cn->type == NodeType::Heading) {
            // 見出しテキストを先行確定してアンカーID生成。
            // base_id を ctx->pool 上に構築することで anchor_counts (同じ pool) の try_emplace を真の move にする。
            ctx->FinalizeCurrentNode();
            std::pmr::string base_id{ &ctx->pool };
            GenerateAnchorIdInto(cn->GetText(), base_id);
            auto [it, inserted] = ctx->anchor_counts.try_emplace(std::move(base_id), 0);
            const int count = it->second++;
            auto* hd = cn->ensure_heading();
            hd->anchor_id.assign(it->first.data(), it->first.size());
            if (count > 0) {
                std::format_to(std::back_inserter(hd->anchor_id), "-{}", count);
            }
            ctx->heading_indices.emplace_back(ctx->current_node_index);
        }
        ctx->ClearCurrentNode();
        break;
    case MD_BLOCK_P:
        if (!TryPromoteParagraphToImage(ctx)) {
            TryPromoteParagraphToDisplayMath(ctx);
        }
        ctx->FinalizeCurrentNode();
        ctx->ClearCurrentNode();
        break;
    case MD_BLOCK_LI:
    case MD_BLOCK_HR:
        ctx->FinalizeCurrentNode();
        ctx->ClearCurrentNode();
        break;

    case MD_BLOCK_DOC:
    case MD_BLOCK_HTML:
        break;

    default:
        std::unreachable();
    }

    return 0;
}

int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    ctx->FlushPendingRun();
    // span markup (** _ ` [] 等) は原文にあるが current_text には入らないため、
    // どの span でも view 化は構造的に失敗する。FinalizeCurrentNode の memcmp をスキップさせる。
    ctx->current_node_owned_only = true;

    switch (type) {
    case MD_SPAN_STRONG:
        ++ctx->bold_count;
        ctx->current_run_flags |= TextRun::kBold;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_EM:
        ++ctx->italic_count;
        ctx->current_run_flags |= TextRun::kItalic;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_CODE:
        ++ctx->code_count;
        ctx->current_run_flags |= TextRun::kCode;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_DEL:
        ++ctx->strikethrough_count;
        ctx->current_run_flags |= TextRun::kStrikethrough;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_A: {
        auto* const a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        ctx->ResolveLinkUrlIndex(std::string_view{ a->href.text, static_cast<size_t>(a->href.size) });
        ctx->paragraph_has_other_content = true;
        break;
    }
    case MD_SPAN_IMG: {
        auto* const img = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        if (img->src.text && img->src.size > 0) {
            ctx->pending_image_src.assign(img->src.text, static_cast<size_t>(img->src.size));
        }
        ctx->paragraph_has_other_content = true;
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
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_WIKILINK:
    case MD_SPAN_U:
        ctx->paragraph_has_other_content = true;
        break;
    default:
        std::unreachable();
    }

    return 0;
}

int OnLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    ctx->FlushPendingRun();

    // md4c は enter/leave が常にバランスする契約なのでアンダーフローは起きない。
    switch (type) {
    case MD_SPAN_STRONG:
        if (--ctx->bold_count == 0) {
            ctx->current_run_flags &= static_cast<uint8_t>(~TextRun::kBold);
        }
        break;
    case MD_SPAN_EM:
        if (--ctx->italic_count == 0) {
            ctx->current_run_flags &= static_cast<uint8_t>(~TextRun::kItalic);
        }
        break;
    case MD_SPAN_CODE:
        if (--ctx->code_count == 0) {
            ctx->current_run_flags &= static_cast<uint8_t>(~TextRun::kCode);
        }
        break;
    case MD_SPAN_DEL:
        if (--ctx->strikethrough_count == 0) {
            ctx->current_run_flags &= static_cast<uint8_t>(~TextRun::kStrikethrough);
        }
        break;
    case MD_SPAN_A:
        ctx->current_link_url_index = -1;
        break;
    case MD_SPAN_IMG:
        if (auto* cn = ctx->current_node; cn && !ctx->pending_image_src.empty()) {
            // pending_image_src は pool allocator で、NodeImageData::src は default 。
            // allocator 不一致で std::move しても内部的にコピーされるので、明示的に assign(view) する。
            cn->ensure_image()->src.assign(ctx->pending_image_src.data(), ctx->pending_image_src.size());
        }
        ctx->pending_image_src.clear();
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

    if (!ctx->current_node) {
        return 0;
    }

    // 各ノードの最初のテキストコールバックでソースオフセットを記録（UTF-8 byte）。
    // md4c は MD_TEXT_NULLCHAR / MD_TEXT_BR / MD_TEXT_SOFTBR や、CODE/LATEXMATH/HTML の改行・空白置換で
    // _T(""), _T("\n"), _T(" ") といった内部静的リテラルを text として渡すことがある。
    // 異なる array 同士のポインタ減算は UB なので、範囲判定もオフセット計算も uintptr_t の
    // 整数演算で行う (std::less<T*> の total order はアドレスの数値順と一致する保証がない)。
    // 範囲外マッチ (静的リテラル) のときは flag を立てず、後続の実体テキストで上書きできるようにする。
    if (!ctx->node_source_offset_set) [[unlikely]] {
        const auto text_addr = reinterpret_cast<uintptr_t>(text);
        const auto base_addr = reinterpret_cast<uintptr_t>(ctx->markdown_base);
        const auto end_addr = base_addr + ctx->markdown_size * sizeof(char);
        if (text_addr >= base_addr && text_addr < end_addr) {
            ctx->current_node->source_offset = static_cast<uint32_t>((text_addr - base_addr) / sizeof(char));
            ctx->node_source_offset_set = true;
        }
    }

    const std::string_view chunk{ text, static_cast<size_t>(size) };

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
        ctx->AppendDoc(chunk);
        break;

    case MD_TEXT_LATEXMATH:
        ctx->AppendDoc(chunk);
        if (ctx->in_display_math && ctx->paragraph_display_math_count == 0 &&
            !ctx->paragraph_has_other_content) {
            // size==1 のとき md4c が \n をそのまま渡してくるケースが多いのでスカラ比較で済ませ、
            // size>1 のときだけ std::ranges::count にフォールバックする両対応。
            if (chunk.size() == 1) {
                ctx->display_math_newlines += (chunk[0] == mendo::doc_lf);
            }
            else {
                ctx->display_math_newlines += static_cast<int32_t>(std::ranges::count(chunk, mendo::doc_lf));
            }
            ctx->display_math_buf.append(chunk);
        }
        break;

    case MD_TEXT_ENTITY: {
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
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
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
        // BR / SOFTBR は原文の `<br>` や 2 個の半角空白+改行を `\n`/` ` に置換するため raw_slice 不一致。
        ctx->current_node_owned_only = true;
        ctx->AppendDoc("\n");
        break;

    case MD_TEXT_SOFTBR:
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
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

} // namespace

ParseResult ParseMarkdown(std::string_view markdown_text)
{
    MENDO_PROFILE("ParseMarkdown");
    // 各種予約サイズのヒント定数。実測 (100MB 入力 = 30k ノード相当) を基準に、
    // 初期確保サイズと再確保回数のバランスで決めている。値は「入力 N byte あたり 1 個」を表す:
    //   - kArenaInputBytesPerByte=20  → 入力の 5% を初期 arena に。new_delete 直結回数を削減。
    //   - kScratchInputBytesPerByte=8 → ノード当たりの平均テキスト長 ~8B 想定の scratch。
    //   - kInputBytesPerNode=48       → 1 ノードあたり ~48 入力 byte (realloc 14 回→4 回相当)。
    //   - kInputBytesPerHeading=4096  → 1MB あたり 256 個の見出し相当。実測数十〜数百に収まる。
    //   - kInputBytesPerImage=512     → 画像頻度 ~0.2%。
    //   - kInputBytesPerBlockquote=512 → blockquote 頻度 (image と同程度)。
    //   - kInputBytesPerDiagram=1024  → ダイアグラム頻度 ~0.1%。
    constexpr size_t kArenaInputBytesPerByte = 20;
    constexpr size_t kScratchInputBytesPerByte = 8;
    constexpr size_t kInputBytesPerNode = 48;
    constexpr size_t kInputBytesPerHeading = 4096;
    constexpr size_t kInputBytesPerImage = 512;
    constexpr size_t kInputBytesPerBlockquote = 512;
    constexpr size_t kInputBytesPerDiagram = 1024;
    constexpr size_t kArenaMin = 128 * 1024;
    constexpr size_t kArenaMax = 5 * 1024 * 1024;
    const size_t input_size = markdown_text.size();
    const size_t arena_bytes = std::clamp(input_size / kArenaInputBytesPerByte, kArenaMin, kArenaMax);
    MENDO_STATF("parse_resource arena: input={} arena={}", input_size, arena_bytes);
    ParseContext ctx{ arena_bytes };
    ctx.markdown_base = markdown_text.data();
    ctx.markdown_size = input_size;
    ctx.current_text.reserve(std::clamp(input_size / kScratchInputBytesPerByte, SCRATCH_RESERVE_MIN, SCRATCH_RESERVE_MAX));
    // 上限 256K: 100MB / 48 ≈ 2.18M ノード期待だが Node sizeof × 256K = ~20MB に抑える。
    const size_t nodes_reserve = std::clamp(input_size / kInputBytesPerNode, size_t{ 64 }, size_t{ 262144 });
    ctx.nodes.reserve(nodes_reserve);
    ctx.list_counter.reserve(8);
    MENDO_STATF("nodes.reserve: input={} reserve={}", input_size, nodes_reserve);
    // heading_indices と anchor_counts は 1 見出し 1 エントリで対になるので同じヒントを使う。
    const size_t heading_hint = std::clamp(input_size / kInputBytesPerHeading, size_t{ 8 }, size_t{ 256 });
    ctx.heading_indices.reserve(heading_hint);
    ctx.anchor_counts.reserve(heading_hint);
    ctx.image_indices.reserve(std::clamp(input_size / kInputBytesPerImage, size_t{ 4 }, size_t{ 256 }));
    ctx.diagram_indices.reserve(std::clamp(input_size / kInputBytesPerDiagram, size_t{ 4 }, size_t{ 128 }));
    ctx.blockquote_indices.reserve(std::clamp(input_size / kInputBytesPerBlockquote, size_t{ 4 }, size_t{ 256 }));

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

    {
        MENDO_PROFILE("DetectAlerts");
        DetectAlerts(ctx.nodes, std::span<const size_t>{ ctx.blockquote_indices });
    }

    ParseResult result;
    result.nodes = std::move(ctx.nodes);
    result.heading_indices = std::move(ctx.heading_indices);
    result.image_indices = std::move(ctx.image_indices);
    result.diagram_indices = std::move(ctx.diagram_indices);
    return result;
}

std::string_view GetAlertLabel(AlertType type) noexcept
{
    switch (type) {
    case AlertType::None:
        return "";
    case AlertType::Note:
        return "Note";
    case AlertType::Tip:
        return "Tip";
    case AlertType::Important:
        return "Important";
    case AlertType::Warning:
        return "Warning";
    case AlertType::Caution:
        return "Caution";
    }
    std::unreachable();
}

std::string_view GetAlertIcon(AlertType type) noexcept
{
    switch (type) {
    case AlertType::None:
        return " ";
    case AlertType::Note:
        return "ℹ"; // ℹ Information Source (BMP)
    case AlertType::Tip:
        // 💡 (U+1F4A1) は BMP 外なので UTF-8 4 byte。MENDO_LIT は実体非変更なので直接バイト列で渡す。
        return "\xF0\x9F\x92\xA1";
    case AlertType::Important:
        return "❗"; // ❗ Heavy Exclamation Mark
    case AlertType::Warning:
        return "⚠"; // ⚠ Warning Sign
    case AlertType::Caution:
        return "⛔"; // ⛔ No Entry
    }
    std::unreachable();
}

namespace {

// テキスト先頭から [!TYPE] パターンを検出し、AlertTypeを返す。
// Alert マーカーは GitHub 仕様で ASCII 固定なので大小無視 ASCII 比較でよい。
AlertType DetectAlertMarker(std::string_view text, size_t& marker_end)
{
    if (text.size() < 3 || text[0] != '[' || text[1] != '!') {
        return AlertType::None;
    }
    const auto close = text.find(']');
    if (close == std::string_view::npos || close <= 2) {
        return AlertType::None;
    }

    const auto type_str = text.substr(2, close - 2);

    struct AlertEntry {
        ascii_util::DocLowercaseLiteral name;
        AlertType type;
    };
    static constexpr AlertEntry kAlerts[]{
        { "note",      AlertType::Note      },
        { "tip",       AlertType::Tip       },
        { "important", AlertType::Important },
        { "warning",   AlertType::Warning   },
        { "caution",   AlertType::Caution   },
    };

    AlertType type = AlertType::None;
    for (const auto& [name, t] : kAlerts) {
        if (ascii_util::iequal(type_str, name)) {
            type = t;
            break;
        }
    }
    if (type == AlertType::None) {
        return AlertType::None;
    }

    marker_end = close + 1;
    // マーカー直後のスペースまたは改行を1つスキップ
    if (marker_end < text.size() && (text[marker_end] == mendo::doc_sp || text[marker_end] == mendo::doc_lf)) {
        marker_end++;
    }
    return type;
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, AlertType type, size_t marker_end)
{
    const std::string_view label = GetAlertLabel(type);
    const std::string_view icon = GetAlertIcon(type);
    const auto& current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    // 新しいテキストを構築: "[icon] Label" (+ "\n \n" + 残りテキスト)
    const size_t icon_prefix_len = icon.size() + 1; // アイコン文字列 + スペース
    const size_t full_label_len = icon_prefix_len + label.size();
    std::pmr::string new_text;
    new_text.reserve(full_label_len + 4 + (has_content ? current_text.size() - marker_end : 0));
    new_text.append(icon);
    new_text += mendo::doc_sp;
    new_text.append(label);

    size_t new_content_start = full_label_len;
    if (has_content) {
        new_text += mendo::doc_lf;
        new_content_start = full_label_len + 1;
        new_text.append(current_text.data() + marker_end, current_text.size() - marker_end);
    }

    // TextRun の調整
    const int delta = static_cast<int>(new_content_start) - static_cast<int>(marker_end);

    TextRunList new_runs;
    // ラベル用の太字ラン（アイコン + スペース + ラベルテキスト）
    TextRun label_run;
    label_run.start = 0;
    label_run.length = static_cast<uint32_t>(full_label_len);
    label_run.set_bold(true);
    new_runs.emplace_back(label_run);

    // 元のランを調整（マーカー部分を除外）
    for (const auto& run : node.runs) {
        const uint32_t run_end = run.start + run.length;
        if (run_end <= static_cast<uint32_t>(marker_end)) {
            continue;
        }
        TextRun adjusted = run;
        if (adjusted.start < static_cast<uint32_t>(marker_end)) {
            const uint32_t trim = static_cast<uint32_t>(marker_end) - adjusted.start;
            adjusted.start = static_cast<uint32_t>(marker_end);
            adjusted.length -= trim;
        }
        adjusted.start = static_cast<uint32_t>(static_cast<int>(adjusted.start) + delta);
        new_runs.emplace_back(adjusted);
    }

    // node.SetText の中で line_count を再カウントすると O(text) で改行を数え直すため、
    // ここで差分計算する。マーカー [!TYPE] 本体には改行が入らず、
    // DetectAlertMarker で 1 文字だけスキップする文字が \n の場合のみ改行 1 個。
    // count を走査せず、marker_end 直前の 1 文字だけを見ればよい。
    const int32_t marker_newlines = (marker_end > 0 && current_text[marker_end - 1] == mendo::doc_lf);
    const int32_t new_line_count = node.line_count - marker_newlines + has_content;
    node.SetTextWithLineCount(std::move(new_text), new_line_count);
    node.runs = std::move(new_runs);
    node.alert_type = type;
    node.alert_label_length = static_cast<uint32_t>(full_label_len);
}

// 単一の BlockQuote ノードに対して Alert マーカーを検出・適用し、
// 同一 blockquote_group の後続ノードに alert_type を伝播する。
// 既に alert_type が設定されているノードはスキップする（伝播で当たった先頭等）。
void DetectAlertAt(std::pmr::vector<Node>& nodes, size_t i)
{
    const auto node_count = nodes.size();
    if (i >= node_count) {
        return;
    }
    auto& node = nodes[i];
    if (node.type != NodeType::BlockQuote || node.alert_type != AlertType::None) {
        return;
    }
    // GitHub 仕様: Alert は最外側 blockquote (quote_depth==1) でのみ認識する。
    // ネスト内 (`> > [!NOTE]`) は通常の引用として扱う。
    if (node.quote_depth != 1) {
        return;
    }
    size_t marker_end = 0;
    const AlertType type = DetectAlertMarker(node.GetText(), marker_end);
    if (type == AlertType::None) {
        return;
    }
    const int group = node.blockquote_group;
    TransformAlertNode(node, type, marker_end);

    // 同一 blockquote_group の後続ノードにも同じ alert_type を伝播。
    // ノード種別に依存せず、グループIDで判定する（リスト等も含む）。
    for (size_t j = i + 1; j < node_count; j++) {
        if (nodes[j].blockquote_group != group) {
            break;
        }
        nodes[j].alert_type = type;
    }
}

} // namespace

void DetectAlerts(std::pmr::vector<Node>& nodes, std::span<const size_t> blockquote_indices)
{
    for (const size_t i : blockquote_indices) {
        DetectAlertAt(nodes, i);
    }
}

std::optional<std::string_view> ResolveHtmlEntity(std::string_view entity, char (&buffer)[4])
{
    // 名前付き実体参照はサイズで先に分岐し、比較対象を 1～3 候補に絞る。
    switch (entity.size()) {
    case 4:
        if (entity == "&lt;") {
            return std::string_view{ "<" };
        }
        if (entity == "&gt;") {
            return std::string_view{ ">" };
        }
        break;
    case 5:
        if (entity == "&amp;") {
            return std::string_view{ "&" };
        }
        break;
    case 6:
        if (entity == "&quot;") {
            return std::string_view{ "\"" };
        }
        if (entity == "&apos;") {
            return std::string_view{ "'" };
        }
        if (entity == "&nbsp;") {
            return std::string_view{ " " };
        }
        break;
    default:
        break;
    }

    if (entity.size() >= 4 && entity[0] == '&' && entity[1] == '#' && entity.back() == ';') {
        const char* digits;
        size_t digit_len;
        uint32_t base;
        size_t max_digits;
        if (entity[2] == 'x' || entity[2] == 'X') {
            digits = entity.data() + 3;
            digit_len = entity.size() - 4; // "&#x" と末尾 ';' を除いた残り長
            base = 16;
            max_digits = 6; // U+10FFFF = 6 桁。これより長い hex 入力は overflow の前に弾く。
        }
        else {
            digits = entity.data() + 2;
            digit_len = entity.size() - 3; // "&#" と末尾 ';' を除いた残り長
            base = 10;
            max_digits = 7; // 1114111 = 7 桁。これより長い 10 進入力は overflow の前に弾く。
        }
        // 桁数オーバーは codepoint 型 (uint32_t) のラップを未然に防ぐため弾く。
        if (digit_len == 0 || digit_len > max_digits) {
            return std::nullopt;
        }
        uint32_t codepoint = 0;
        const char* const stop = ascii_util::from_chars(digits, digit_len, codepoint, base);
        // 全桁消費 (stop == digits + digit_len) のみ受理。
        // "&#65x;" のように途中で停止した入力は不正として弾く。
        if (stop != digits + digit_len || codepoint == 0) {
            return std::nullopt;
        }
        // 範囲外/サロゲート判定は EncodeCp 内に集約 (戻り値 0 で不正)。
        const uint32_t len = utf8_codec::EncodeCp(codepoint, buffer);
        if (len == 0) {
            return std::nullopt;
        }
        return std::string_view{ buffer, len };
    }

    return std::nullopt;
}
