#include "parser.h"
#include "ascii_util.h"
#include "document_utils.h"
#include "syntax.h"
#include "string_convert.h"
#include "memory_resource.h"
#include "md4c.h"
#include <stack>
#include <map>
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

namespace {

struct ParseContext {
    // パース用 monotonic リソース（一括確保→一括解放）
    MonotonicResource parse_resource{ 128 * 1024 };

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
    int current_link_url_index = -1;

    // 現在ノード用の Wide 蓄積スクラッチ。FinalizeCurrentNode で
    // ノードの text_ にムーブする。
    std::pmr::wstring current_text{ parse_resource.resource() };

    // current_text 内の「未確定 TextRun」の開始位置 (wide unit)。
    // 同じ span 状態で連続する AppendWide は 1 つの TextRun に統合される。
    // span 切替 / ブロック退出時に FlushPendingRun が呼ばれて確定する。
    uint32_t pending_run_start = 0;
    bool has_pending_run = false;

    // ブロックコンテキスト追跡
    int indent_level = 0;
    bool in_code_block = false;
    int blockquote_depth = 0;
    int blockquote_group_counter = 0;           // グループID生成用
    int current_blockquote_group = -1;          // 最外側 blockquote の group ID（ネスト中は共有）
    int outermost_quote_indent = 0;             // 最外側 blockquote 進入時の indent_level（描画時のバー起点）

    // リスト追跡
    std::stack<int, std::pmr::deque<int>> list_counter{ std::pmr::deque<int>{ parse_resource.resource() } }; // 0 = 順序なしリスト, >0 = 順序ありリストのカウンター

    // テーブル追跡
    bool in_table = false;
    bool in_thead = false;
    TableCell* current_cell = nullptr;
    int current_cell_align = 0;

    // 現在構築中のノード
    Node* current_node = nullptr;

    std::pmr::vector<std::pmr::wstring> link_urls{ parse_resource.resource() };

    // BeginNode 毎にクリア。MakeRun から O(1) で重複 URL を検出する
    std::pmr::unordered_map<std::pmr::wstring, int16_t> current_node_url_map{ parse_resource.resource() };

    // アンカーIDの一意性追跡: スラグ -> 出現回数
    std::pmr::map<std::pmr::wstring, int> anchor_counts{ parse_resource.resource() };

    // 画像スパン追跡
    std::pmr::wstring pending_image_src{ parse_resource.resource() };

    // display math スパンが 1 個だけで他の内容が無い段落を LatexMath コードブロックに昇格する状態
    bool in_display_math = false;
    int paragraph_display_math_count = 0;
    bool paragraph_has_other_content = false;
    std::pmr::wstring display_math_buf{ parse_resource.resource() };

    // md_parse() に渡した入力バッファ先頭ポインタ（source_offset 計算用、UTF-16 コード単位オフセット）
    const wchar_t* markdown_base = nullptr;

    // 現在ノードの Wide スクラッチを text_ に書き込み、スクラッチを空にする。
    // BeginNode 直前と各 OnLeaveBlock の current_node 解除直前に呼ぶ。
    void FinalizeCurrentNode()
    {
        FlushPendingRun();
        if (current_node && !current_text.empty()) {
            current_node->SetTextWithLineCount(std::move(current_text), current_node->line_count);
        }
        // move 後 / 元から空 のいずれの経路でも、次ノード用に空状態へ揃える。
        current_text.clear();
    }

    void BeginNode(NodeType type)
    {
        FinalizeCurrentNode();
        nodes.emplace_back();
        current_node = &nodes.back();
        current_node->type = type;
        current_node_index = nodes.size() - 1;
        constexpr int kInt8Max = std::numeric_limits<int8_t>::max();
        current_node->indent_level = static_cast<int8_t>(std::min(indent_level, kInt8Max));
        if (blockquote_depth > 0) {
            current_node->blockquote_group = current_blockquote_group;
            current_node->quote_depth = static_cast<int8_t>(std::min(blockquote_depth, kInt8Max));
            current_node->quote_outer_indent = static_cast<int8_t>(std::min(outermost_quote_indent, kInt8Max));
        }
        current_node_url_map.clear();
    }

    TextRun MakeRun(uint32_t start, uint32_t length)
    {
        TextRun run;
        run.start = start;
        run.length = length;
        run.set_bold(static_cast<bool>(bold_count));
        run.set_italic(static_cast<bool>(italic_count));
        run.set_code(static_cast<bool>(code_count));
        run.set_strikethrough(static_cast<bool>(strikethrough_count));
        if (current_link_url_index >= 0 && current_node) {
            const auto& url = link_urls[static_cast<size_t>(current_link_url_index)];
            int16_t node_idx;
            if (const auto it = current_node_url_map.find(url); it != current_node_url_map.end()) {
                node_idx = it->second;
            }
            else {
                node_idx = static_cast<int16_t>(current_node->link_urls.size());
                current_node->link_urls.emplace_back(url);
                current_node_url_map.emplace(url, node_idx);
            }
            run.link_url_index = node_idx;
        }
        return run;
    }

    // テーブルセルにWideテキストを追加（AppendWideから委譲される）。
    // セルは pending run 方式を使わず、即時 TextRun を生成する。
    void AppendTextToCell(std::wstring_view text)
    {
        const uint32_t start = static_cast<uint32_t>(current_cell->text.size());
        current_cell->text.append(text);
        current_cell->runs.emplace_back(MakeRun(start, static_cast<uint32_t>(text.size())));
    }

    // Wide テキストを現在のノードまたはセルに追加する。
    // セル内なら AppendTextToCell に委譲。
    // ノードなら current_text スクラッチに直接 append し、未確定 TextRun として保留する。
    // 同じ span 状態で連続して呼ばれると 1 つの TextRun に統合される。
    void AppendWide(std::wstring_view text)
    {
        if (current_cell) {
            AppendTextToCell(text);
            return;
        }
        if (!current_node || text.empty()) {
            return;
        }
        if (!has_pending_run) {
            pending_run_start = static_cast<uint32_t>(current_text.size());
            has_pending_run = true;
        }
        current_text.append(text);
        current_node->line_count += static_cast<int>(std::ranges::count(text, L'\n'));
    }

    // 未確定 TextRun を確定して current_node->runs に push する。
    // span 状態が変わる直前 (OnEnter/Leave Span) と OnLeaveBlock の冒頭で呼ぶ。
    void FlushPendingRun()
    {
        if (has_pending_run && current_node && current_text.size() > pending_run_start) {
            const uint32_t length = static_cast<uint32_t>(current_text.size() - pending_run_start);
            current_node->runs.emplace_back(MakeRun(pending_run_start, length));
        }
        has_pending_run = false;
    }
};

// MD_BLOCK_P 終了時、画像 span を含む段落 / 引用ブロックを Image ノードへ昇格させる。
// 戻り値 true なら他の昇格処理はスキップしてよい。
bool TryPromoteParagraphToImage(ParseContext* ctx)
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
bool TryPromoteParagraphToDisplayMath(ParseContext* ctx)
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
    ctx->current_text.assign(ctx->display_math_buf.data(), ctx->display_math_buf.size());
    node->runs.clear();
    node->line_count = static_cast<int>(std::ranges::count(ctx->current_text, L'\n'));
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
        ctx->in_display_math = false;
        break;

    case MD_BLOCK_CODE: {
        ctx->in_code_block = true;
        ctx->BeginNode(NodeType::CodeBlock);
        auto* const code_detail = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        if (code_detail && code_detail->lang.text && code_detail->lang.size > 0) {
            ctx->current_node->code_language = DetectLanguage(std::wstring_view{ code_detail->lang.text, static_cast<size_t>(code_detail->lang.size) });
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
        ctx->list_counter.push(0);
        ctx->indent_level++;
        break;

    case MD_BLOCK_OL: {
        auto* const ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        ctx->list_counter.push(static_cast<int>(ol->start));
        ctx->indent_level++;
        break;
    }

    case MD_BLOCK_LI: {
        auto* const li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (li->is_task) {
            ctx->BeginNode(NodeType::TaskListItem);
            ctx->current_node->task_checked = (li->task_mark == L'x' || li->task_mark == L'X');
        }
        else {
            ctx->BeginNode(NodeType::ListItem);
        }
        if (!ctx->list_counter.empty()) {
            const int counter = ctx->list_counter.top();
            ctx->current_node->list_number = counter;
            if (counter > 0) {
                ctx->list_counter.top()++;
            }
        }
        break;
    }

    case MD_BLOCK_HR:
        ctx->BeginNode(NodeType::HorizontalRule);
        break;

    case MD_BLOCK_TABLE:
        ctx->BeginNode(NodeType::Table);
        ctx->in_table = true;
        break;

    case MD_BLOCK_THEAD:
        ctx->in_thead = true;
        break;

    case MD_BLOCK_TBODY:
        ctx->in_thead = false;
        break;

    case MD_BLOCK_TR:
        if (auto* cn = ctx->current_node; cn && cn->type == NodeType::Table) {
            cn->ensure_table();
            cn->table_rows().emplace_back();
        }
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        if (auto* cn = ctx->current_node; cn && cn->type == NodeType::Table && cn->has_table() && !cn->table_rows().empty()) {
            auto& row = cn->table_rows().back();
            ctx->current_cell = &row.cells.emplace_back();
            ctx->current_cell->is_header = (type == MD_BLOCK_TH);
            if (detail) {
                auto* const td = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
                ctx->current_cell->align = static_cast<TableAlign>(td->align);
            }
        }
        break;
    }
    case MD_BLOCK_HTML:
        break;
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
        if (cn && !ctx->current_text.empty() && ctx->current_text.back() == L'\n') {
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
            ctx->list_counter.pop();
        }
        if (ctx->indent_level > 0) {
            ctx->indent_level--;
        }
        break;

    case MD_BLOCK_TABLE:
        ctx->in_table = false;
        ctx->FinalizeCurrentNode();
        ctx->current_node = nullptr;
        break;

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
    case MD_BLOCK_TR:
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        ctx->current_cell = nullptr;
        break;

    case MD_BLOCK_H:
        if (auto* cn = ctx->current_node; cn && cn->type == NodeType::Heading) {
            // 見出しテキストを先行確定してアンカーID生成
            ctx->FinalizeCurrentNode();
            std::pmr::wstring base_id = GenerateAnchorId(cn->GetText());
            auto [it, inserted] = ctx->anchor_counts.try_emplace(std::move(base_id), 0);
            const int count = it->second++;
            auto* hd = cn->ensure_heading();
            hd->anchor_id.assign(it->first.data(), it->first.size());
            if (count > 0) {
                std::format_to(std::back_inserter(hd->anchor_id), L"-{}", count);
            }
            ctx->heading_indices.emplace_back(ctx->current_node_index);
        }
        ctx->current_node = nullptr;
        break;
    case MD_BLOCK_P:
        if (!TryPromoteParagraphToImage(ctx)) {
            TryPromoteParagraphToDisplayMath(ctx);
        }
        ctx->FinalizeCurrentNode();
        ctx->current_node = nullptr;
        break;
    case MD_BLOCK_LI:
    case MD_BLOCK_HR:
        ctx->FinalizeCurrentNode();
        ctx->current_node = nullptr;
        break;

    default:
        break;
    }

    return 0;
}

int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    ctx->FlushPendingRun();

    switch (type) {
    case MD_SPAN_STRONG:
        ++ctx->bold_count;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_EM:
        ++ctx->italic_count;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_CODE:
        ++ctx->code_count;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_DEL:
        ++ctx->strikethrough_count;
        ctx->paragraph_has_other_content = true;
        break;
    case MD_SPAN_A: {
        auto* const a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        if (a->href.text && a->href.size > 0) {
            ctx->link_urls.emplace_back();
            ctx->link_urls.back().assign(a->href.text, static_cast<size_t>(a->href.size));
            ctx->current_link_url_index = static_cast<int>(ctx->link_urls.size()) - 1;
        }
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
        ctx->AppendWide(L"$$");
        if (ctx->paragraph_display_math_count == 0 && !ctx->paragraph_has_other_content) {
            ctx->display_math_buf.clear();
        }
        else {
            ctx->paragraph_has_other_content = true;
        }
        break;
    case MD_SPAN_LATEXMATH:
        // インライン $...$ は昇格対象外。元の "$" を復元してテキストとして残す
        ctx->AppendWide(L"$");
        ctx->paragraph_has_other_content = true;
        break;
    default:
        ctx->paragraph_has_other_content = true;
        break;
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
        --ctx->bold_count;
        break;
    case MD_SPAN_EM:
        --ctx->italic_count;
        break;
    case MD_SPAN_CODE:
        --ctx->code_count;
        break;
    case MD_SPAN_DEL:
        --ctx->strikethrough_count;
        break;
    case MD_SPAN_A:
        ctx->current_link_url_index = -1;
        break;
    case MD_SPAN_IMG:
        if (auto* cn = ctx->current_node; cn && !ctx->pending_image_src.empty()) {
            cn->ensure_image()->src = std::move(ctx->pending_image_src);
        }
        ctx->pending_image_src.clear();
        break;
    case MD_SPAN_LATEXMATH_DISPLAY:
        ctx->in_display_math = false;
        ctx->AppendWide(L"$$");
        ctx->paragraph_display_math_count++;
        break;
    case MD_SPAN_LATEXMATH:
        ctx->AppendWide(L"$");
        break;
    default:
        break;
    }

    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    if (!ctx->current_node) {
        return 0;
    }

    // 各ノードの最初のテキストコールバックでソースオフセットを記録（UTF-16 コード単位）
    if (ctx->current_node->source_offset == kUnsetSourceOffset && ctx->markdown_base) {
        ctx->current_node->source_offset = static_cast<uint32_t>(text - ctx->markdown_base);
    }

    const std::wstring_view chunk{ text, static_cast<size_t>(size) };

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
        ctx->AppendWide(chunk);
        break;

    case MD_TEXT_LATEXMATH:
        ctx->AppendWide(chunk);
        if (ctx->in_display_math && ctx->paragraph_display_math_count == 0 &&
            !ctx->paragraph_has_other_content) {
            ctx->display_math_buf.append(chunk);
        }
        break;

    case MD_TEXT_ENTITY: {
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
        wchar_t entity_buf[2];
        if (const auto resolved = ResolveHtmlEntity(chunk, entity_buf)) {
            ctx->AppendWide(*resolved);
        }
        else {
            ctx->AppendWide(chunk);
        }
        break;
    }

    case MD_TEXT_BR:
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
        ctx->AppendWide(L"\n");
        break;

    case MD_TEXT_SOFTBR:
        if (!ctx->in_display_math) {
            ctx->paragraph_has_other_content = true;
        }
        ctx->AppendWide(L" ");
        break;

    default:
        break;
    }

    return 0;
}

} // namespace

ParseResult ParseMarkdown(std::wstring_view markdown_text)
{
    ParseContext ctx;
    ctx.markdown_base = markdown_text.data();
    // ノード単位のスクラッチを 1 回確保しておくと、長段落・コードブロックでの再確保を抑えられる。
    ctx.current_text.reserve(4096);
    ctx.nodes.reserve(std::clamp(markdown_text.size() / 64, size_t{ 64 }, size_t{ 8192 }));
    ctx.heading_indices.reserve(std::clamp(markdown_text.size() / 256, size_t{ 8 }, size_t{ 512 }));
    ctx.image_indices.reserve(std::clamp(markdown_text.size() / 512, size_t{ 4 }, size_t{ 256 }));
    ctx.diagram_indices.reserve(std::clamp(markdown_text.size() / 1024, size_t{ 4 }, size_t{ 128 }));
    ctx.blockquote_indices.reserve(std::clamp(markdown_text.size() / 512, size_t{ 4 }, size_t{ 256 }));

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;

    md_parse(markdown_text.data(), static_cast<MD_SIZE>(markdown_text.size()), &parser, &ctx);

    // 最後の current_node が残っていれば（典型的には全 OnLeaveBlock で処理済みだが
    // 安全のため）テキストを確定する。
    ctx.FinalizeCurrentNode();

    DetectAlerts(ctx.nodes, std::span<const size_t>{ ctx.blockquote_indices });

    ParseResult result;
    result.nodes = std::move(ctx.nodes);
    result.heading_indices = std::move(ctx.heading_indices);
    result.image_indices = std::move(ctx.image_indices);
    result.diagram_indices = std::move(ctx.diagram_indices);
    return result;
}

ParseResult ParseMarkdown(std::string_view utf8_markdown_text)
{
    std::pmr::wstring wide;
    string_convert::Utf8ToWide(utf8_markdown_text, wide);
    return ParseMarkdown(std::wstring_view{ wide });
}

std::wstring_view GetAlertLabel(AlertType type) noexcept
{
    switch (type) {
    case AlertType::Note:      return L"Note";
    case AlertType::Tip:       return L"Tip";
    case AlertType::Important: return L"Important";
    case AlertType::Warning:   return L"Warning";
    case AlertType::Caution:   return L"Caution";
    default:                   return L"";
    }
}

std::wstring_view GetAlertIcon(AlertType type) noexcept
{
    switch (type) {
    case AlertType::Note:      return L"ℹ";         // ℹ Information Source
    case AlertType::Tip:       return L"\xD83D\xDCA1";   // 💡 Light Bulb (surrogate pair)
    case AlertType::Important: return L"❗";         // ❗ Heavy Exclamation Mark
    case AlertType::Warning:   return L"⚠";         // ⚠ Warning Sign
    case AlertType::Caution:   return L"⛔";         // ⛔ No Entry
    default:                   return L" ";
    }
}

namespace {

// テキスト先頭から [!TYPE] パターンを検出し、AlertTypeを返す。
// Alert マーカーは GitHub 仕様で ASCII 固定なので大小無視 ASCII 比較でよい。
AlertType DetectAlertMarker(std::wstring_view text, size_t& marker_end)
{
    if (text.size() < 3 || text[0] != L'[' || text[1] != L'!') {
        return AlertType::None;
    }
    const auto close = text.find(L']');
    if (close == std::wstring_view::npos || close <= 2) {
        return AlertType::None;
    }

    const auto type_str = text.substr(2, close - 2);

    AlertType type = AlertType::None;
    if (ascii_util::iequal(type_str, L"NOTE")) {
        type = AlertType::Note;
    }
    else if (ascii_util::iequal(type_str, L"TIP")) {
        type = AlertType::Tip;
    }
    else if (ascii_util::iequal(type_str, L"IMPORTANT")) {
        type = AlertType::Important;
    }
    else if (ascii_util::iequal(type_str, L"WARNING")) {
        type = AlertType::Warning;
    }
    else if (ascii_util::iequal(type_str, L"CAUTION")) {
        type = AlertType::Caution;
    }

    if (type == AlertType::None) {
        return AlertType::None;
    }

    marker_end = close + 1;
    // マーカー直後のスペースまたは改行を1つスキップ
    if (marker_end < text.size() && (text[marker_end] == L' ' || text[marker_end] == L'\n')) {
        marker_end++;
    }
    return type;
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, AlertType type, size_t marker_end)
{
    const std::wstring_view label = GetAlertLabel(type);
    const std::wstring_view icon = GetAlertIcon(type);
    const auto& current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    // 新しいテキストを構築: "[icon] Label" (+ "\n \n" + 残りテキスト)
    const size_t icon_prefix_len = icon.size() + 1; // アイコン文字列 + スペース
    const size_t full_label_len = icon_prefix_len + label.size();
    std::pmr::wstring new_text;
    new_text.reserve(full_label_len + 4 + (has_content ? current_text.size() - marker_end : 0));
    new_text.append(icon);
    new_text += L' ';
    new_text.append(label);

    size_t new_content_start = full_label_len;
    if (has_content) {
        new_text += L'\n';
        new_content_start = full_label_len + 1;
        new_text.append(current_text.c_str() + marker_end, current_text.size() - marker_end);
    }

    // TextRun の調整
    const int delta = static_cast<int>(new_content_start) - static_cast<int>(marker_end);

    std::pmr::vector<TextRun> new_runs;
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

    node.SetText(std::move(new_text));
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

std::optional<std::wstring_view> ResolveHtmlEntity(std::wstring_view entity, wchar_t(&buffer)[2])
{
    {
        std::wstring_view literal;
        if (entity == L"&amp;") {
            literal = L"&";
        }
        else if (entity == L"&lt;") {
            literal = L"<";
        }
        else if (entity == L"&gt;") {
            literal = L">";
        }
        else if (entity == L"&quot;") {
            literal = L"\"";
        }
        else if (entity == L"&apos;") {
            literal = L"'";
        }
        else if (entity == L"&nbsp;") {
            literal = L" ";
        }
        if (!literal.empty()) {
            return literal;
        }
    }

    if (entity.size() >= 4 && entity[0] == L'&' && entity[1] == L'#' && entity.back() == L';') {
        unsigned long codepoint = 0;
        const wchar_t* digits;
        size_t digit_len;
        unsigned long base;
        if (entity[2] == L'x' || entity[2] == L'X') {
            digits = entity.data() + 3;
            digit_len = entity.size() - 4; // "&#x" と末尾 ';' を除いた残り長
            base = 16;
        }
        else {
            digits = entity.data() + 2;
            digit_len = entity.size() - 3; // "&#" と末尾 ';' を除いた残り長
            base = 10;
        }
        const wchar_t* const stop = ascii_util::from_chars(digits, digit_len, codepoint, base);
        // 全桁消費 (stop == digits + digit_len) かつ 1 桁以上 (stop > digits) のみ受理。
        // "&#65x;" のように途中で停止した入力は不正として弾く。
        const bool fully_consumed = (stop == digits + digit_len) && (stop > digits);
        // サロゲート範囲 (U+D800-U+DFFF) は単独で UTF-16 として不正なので除外し、
        // 呼び出し側で元の入力をそのままテキストとして再投入させる。
        if (fully_consumed && codepoint > 0 && codepoint <= 0xFFFF &&
            !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            buffer[0] = static_cast<wchar_t>(codepoint);
            return std::wstring_view{ buffer, 1 };
        }
        if (fully_consumed && codepoint > 0xFFFF && codepoint <= 0x10FFFF) {
            // 補助面: UTF-16 サロゲートペア
            const unsigned long adj = codepoint - 0x10000;
            buffer[0] = static_cast<wchar_t>(0xD800 + (adj >> 10));
            buffer[1] = static_cast<wchar_t>(0xDC00 + (adj & 0x3FF));
            return std::wstring_view{ buffer, 2 };
        }
    }

    return std::nullopt;
}
