#include "parser.h"
#include "syntax.h"
#include "string_convert.h"
#include "memory_resource.h"
#include "md4c.h"
#include <stack>
#include <unordered_map>
#include <charconv>
#include <format>
#include <iterator>
#include <algorithm>

std::pmr::wstring GenerateAnchorId(std::wstring_view text)
{
    std::pmr::wstring slug;
    slug.reserve(text.size());
    for (wchar_t c : text) {
        if (c >= L'A' && c <= L'Z') {
            slug += static_cast<wchar_t>(c - L'A' + L'a');
        }
        else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_') {
            slug += c;
        }
        else if (c == L' ' || c == L'\t') {
            slug += L'-';
        }
        // CJK文字: そのまま保持するが、句読点・記号はスキップ
        else if (c >= 0x3000) {
            bool skip = false;
            // CJK記号と句読点 (U+3000-U+303F): 、。「」【】〈〉 等
            if (c <= 0x303F) {
                skip = true;
            }
            // 全角ASCII対応の句読点
            else if (c >= 0xFF01 && c <= 0xFF0F) {
                skip = true; // ！＂＃…（）＊＋，－．／
            }
            else if (c >= 0xFF1A && c <= 0xFF20) {
                skip = true; // ：；＜＝＞？＠
            }
            else if (c >= 0xFF3B && c <= 0xFF40) {
                skip = true; // ［＼］＾＿｀
            }
            else if (c >= 0xFF5B && c <= 0xFF65) {
                skip = true; // ｛｜｝～…･
            }
            if (!skip) {
                slug += c;
            }
        }
        // その他の文字: スキップ
    }
    return slug;
}

namespace {

using string_convert::Utf8ToWide;

struct SpanState {
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strikethrough = false;
    int link_url_index = -1; // -1 = リンクなし, >= 0 = link_urls へのインデックス
};

struct ParseContext {
    // パース用 monotonic リソース（一括確保→一括解放）
    MonotonicResource parse_resource{ 64 * 1024 };

    std::pmr::vector<Node> nodes;

    // パース中に構築する特殊ノードインデックス（BuildIndicesのO(n)走査を除去）
    std::pmr::vector<size_t> heading_indices;
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> mermaid_indices;
    size_t current_node_index = 0;

    // パース一時データには monotonic リソースを使用
    std::stack<SpanState, std::pmr::deque<SpanState>> span_stack{
        std::pmr::deque<SpanState>{parse_resource.resource()} };
    SpanState current_span;

    // UTF-8 → Wide変換用の再利用可能バッファ
    std::pmr::wstring text_buffer;

    std::pmr::string utf8_accum{ parse_resource.resource() };

    // ブロックコンテキスト追跡
    int indent_level = 0;
    bool in_code_block = false;
    int blockquote_depth = 0;
    int blockquote_group_counter = 0;           // グループID生成用
    std::stack<int, std::pmr::deque<int>> blockquote_group_stack{
        std::pmr::deque<int>{parse_resource.resource()} }; // ネスト追跡用

    // リスト追跡
    std::stack<int, std::pmr::deque<int>> list_counter{
        std::pmr::deque<int>{parse_resource.resource()} }; // 0 = 順序なしリスト, >0 = 順序ありリストのカウンター

    // テーブル追跡
    bool in_table = false;
    bool in_thead = false;
    TableCell* current_cell = nullptr;
    int current_cell_align = 0;

    // 現在構築中のノード
    Node* current_node = nullptr;
    uint32_t node_wide_offset = 0; // 現在ノードのWide文字オフセット（TextRun用）

    // リンクURL格納: SpanStateではインデックスのみ保持し、push/popでの文字列コピーを回避
    std::pmr::vector<std::pmr::wstring> link_urls{ parse_resource.resource() };

    // アンカーIDの一意性追跡: スラグ -> 出現回数
    std::pmr::unordered_map<std::pmr::wstring, int> anchor_counts{ parse_resource.resource() };

    // 画像スパン追跡
    std::pmr::wstring pending_image_src{ parse_resource.resource() };

    // md_parse() に渡した入力バッファ先頭ポインタ（source_offset 計算用）
    const char* markdown_base = nullptr;

    void BeginNode(NodeType type)
    {
        nodes.emplace_back();
        current_node = &nodes.back();
        current_node->type = type;
        current_node_index = nodes.size() - 1;
        // text_valid_ はデフォルト false なので明示的な無効化は不要
        current_node->indent_level = indent_level;
        node_wide_offset = 0;
        if (blockquote_depth > 0 && !blockquote_group_stack.empty()) {
            current_node->blockquote_group = blockquote_group_stack.top();
        }
    }

    TextRun MakeRun(uint32_t start, uint32_t length) const
    {
        TextRun run;
        run.start = start;
        run.length = length;
        run.bold = current_span.bold;
        run.italic = current_span.italic;
        run.code = current_span.code;
        run.strikethrough = current_span.strikethrough;
        if (current_span.link_url_index >= 0 && current_node) {
            const auto& url = link_urls[static_cast<size_t>(current_span.link_url_index)];
            // ノードのURLプール内で重複を検索し、なければ追加
            int16_t node_idx = -1;
            const auto url_count = current_node->link_urls.size();
            for (size_t i = 0; i < url_count; i++) {
                if (current_node->link_urls[i] == url) {
                    node_idx = static_cast<int16_t>(i);
                    break;
                }
            }
            if (node_idx < 0) {
                node_idx = static_cast<int16_t>(current_node->link_urls.size());
                current_node->link_urls.emplace_back(url);
            }
            run.link_url_index = node_idx;
        }
        return run;
    }

    // テーブルセルにWideテキストを追加（セルはWideのまま維持）
    void AppendTextToCell(std::wstring_view text)
    {
        const uint32_t start = static_cast<uint32_t>(current_cell->text.size());
        current_cell->text.append(text);
        current_cell->runs.emplace_back(MakeRun(start, static_cast<uint32_t>(text.size())));
    }

    // ノードにWideテキストを追加（Wide→UTF-8変換してtext_utf8に蓄積、BR/SOFTBR/Entity用）
    void AppendText(std::wstring_view text)
    {
        if (current_cell) {
            AppendTextToCell(text);
            return;
        }
        if (!current_node) {
            return;
        }
        const auto utf8 = string_convert::WideToUtf8(text);
        const uint32_t start = node_wide_offset;
        current_node->text_utf8.append(utf8);
        node_wide_offset += static_cast<uint32_t>(text.size());
        current_node->runs.emplace_back(MakeRun(start, static_cast<uint32_t>(text.size())));
        for (const wchar_t c : text) {
            if (c == L'\n') {
                current_node->line_count++;
            }
        }
    }

    // 蓄積UTF-8をフラッシュ
    void FlushUtf8()
    {
        if (!utf8_accum.empty()) {
            AppendUtf8(utf8_accum);
            utf8_accum.clear();
        }
    }

    // テーブルセルにUTF-8テキストをWide変換して追加
    void AppendUtf8ToCell(std::string_view text)
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (wlen > 0) {
            text_buffer.resize_and_overwrite(static_cast<size_t>(wlen), [text](wchar_t* buf, size_t count) -> size_t {
                const int written = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buf, static_cast<int>(count));
                return (written > 0) ? static_cast<size_t>(written) : 0;
            });
            AppendTextToCell(text_buffer);
        }
    }

    // UTF-8テキストをワイド文字に変換し、現在のノード/セルに追加する（エンティティ等用）
    void AppendUtf8(std::string_view text)
    {
        if (current_cell) {
            AppendUtf8ToCell(text);
            return;
        }
        if (!current_node) {
            return;
        }
        // ノード: Wide長のみ計算してtext_utf8に蓄積
        // ASCII高速パス: 非ASCII（0x80以上）バイトが無ければバイト長＝ワイド長
        int wlen;
        int newline_count = 0;
        size_t scan = 0;
        for (; scan < text.size(); ++scan) {
            if (static_cast<unsigned char>(text[scan]) >= 0x80) {
                break;
            }
            if (text[scan] == '\n') {
                newline_count++;
            }
        }
        if (scan == text.size()) {
            wlen = static_cast<int>(text.size());
        }
        else {
            wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            for (; scan < text.size(); ++scan) {
                if (text[scan] == '\n') {
                    newline_count++;
                }
            }
        }
        if (wlen > 0) {
            const uint32_t start = node_wide_offset;
            current_node->text_utf8.append(text);
            node_wide_offset += static_cast<uint32_t>(wlen);
            current_node->runs.emplace_back(MakeRun(start, static_cast<uint32_t>(wlen)));
            current_node->line_count += newline_count;
        }
    }
};

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_H: {
        auto* const h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        ctx->BeginNode(NodeType::Heading);
        ctx->current_node->heading_level = static_cast<int>(h->level);
        break;
    }

    case MD_BLOCK_P:
        if (!ctx->in_code_block) {
            if (ctx->blockquote_depth > 0) {
                ctx->BeginNode(NodeType::BlockQuote);
            }
            else {
                ctx->BeginNode(NodeType::Paragraph);
            }
        }
        break;

    case MD_BLOCK_CODE: {
        ctx->in_code_block = true;
        ctx->BeginNode(NodeType::CodeBlock);
        auto* const code_detail = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        if (code_detail && code_detail->lang.text && code_detail->lang.size > 0) {
            const std::pmr::wstring lang_str = Utf8ToWide(std::string_view{ code_detail->lang.text, static_cast<size_t>(code_detail->lang.size) });
            ctx->current_node->code_language = DetectLanguage(lang_str);
        }
        break;
    }

    case MD_BLOCK_QUOTE:
        ctx->blockquote_depth++;
        ctx->indent_level++;
        ctx->blockquote_group_stack.push(++ctx->blockquote_group_counter);
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
            ctx->current_node->task_checked = (li->task_mark == 'x' || li->task_mark == 'X');
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
        if (ctx->current_node && ctx->current_node->type == NodeType::Table) {
            ctx->current_node->ensure_table();
            ctx->current_node->table_rows().emplace_back();
        }
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        if (ctx->current_node && ctx->current_node->type == NodeType::Table && ctx->current_node->has_table() && !ctx->current_node->table_rows().empty()) {
            auto& row = ctx->current_node->table_rows().back();
            row.cells.emplace_back();
            ctx->current_cell = &row.cells.back();
            ctx->current_cell->is_header = (type == MD_BLOCK_TH);
            if (detail) {
                auto* const td = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
                ctx->current_cell->align = static_cast<int>(td->align);
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

    ctx->FlushUtf8();

    switch (type) {
    case MD_BLOCK_CODE:
        ctx->in_code_block = false;
        // 末尾の改行があれば除去（text_utf8に対して操作）
        if (ctx->current_node && !ctx->current_node->text_utf8.empty() && ctx->current_node->text_utf8.back() == '\n') {
            ctx->current_node->text_utf8.pop_back();
            ctx->current_node->line_count--;
            ctx->node_wide_offset--;
            if (!ctx->current_node->runs.empty()) {
                auto& last = ctx->current_node->runs.back();
                if (last.length > 0) {
                    last.length--;
                }
            }
        }
        if (ctx->current_node && ctx->current_node->code_language == SyntaxLanguage::Mermaid) {
            ctx->mermaid_indices.emplace_back(ctx->current_node_index);
        }
        break;

    case MD_BLOCK_QUOTE:
        if (ctx->blockquote_depth > 0) {
            ctx->blockquote_depth--;
        }
        if (ctx->indent_level > 0) {
            ctx->indent_level--;
        }
        if (!ctx->blockquote_group_stack.empty()) {
            ctx->blockquote_group_stack.pop();
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
        if (ctx->current_node && ctx->current_node->type == NodeType::Heading) {
            // 見出しテキストをWideに変換してアンカーID生成（見出しは少数なのでコスト小）
            std::pmr::wstring base_id = GenerateAnchorId(ctx->current_node->GetText());
            const auto it = ctx->anchor_counts.find(base_id);
            const int count = (it != ctx->anchor_counts.end()) ? it->second : 0;
            if (count > 0) {
                ctx->current_node->anchor_id = base_id;
                std::format_to(std::back_inserter(ctx->current_node->anchor_id), L"-{}", count);
            }
            else {
                ctx->current_node->anchor_id = base_id;
            }
            ctx->anchor_counts[std::move(base_id)] = count + 1;
            ctx->heading_indices.emplace_back(ctx->current_node_index);
        }
        ctx->current_node = nullptr;
        break;
    case MD_BLOCK_P:
        // 画像を含む段落/引用ブロックを Image ノードに変換
        if (ctx->current_node && ctx->current_node->has_image() && !ctx->current_node->image_data->src.empty()
            && (ctx->current_node->type == NodeType::Paragraph
                || ctx->current_node->type == NodeType::BlockQuote)) {
            ctx->current_node->type = NodeType::Image;
            ctx->image_indices.emplace_back(ctx->current_node_index);
        }
        ctx->current_node = nullptr;
        break;
    case MD_BLOCK_LI:
    case MD_BLOCK_HR:
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

    ctx->FlushUtf8();
    ctx->span_stack.push(ctx->current_span);

    switch (type) {
    case MD_SPAN_STRONG:
        ctx->current_span.bold = true;
        break;
    case MD_SPAN_EM:
        ctx->current_span.italic = true;
        break;
    case MD_SPAN_CODE:
        ctx->current_span.code = true;
        break;
    case MD_SPAN_DEL:
        ctx->current_span.strikethrough = true;
        break;
    case MD_SPAN_A: {
        auto* const a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        if (a->href.text && a->href.size > 0) {
            ctx->link_urls.emplace_back(Utf8ToWide(std::string_view{ a->href.text, static_cast<size_t>(a->href.size) }));
            ctx->current_span.link_url_index = static_cast<int>(ctx->link_urls.size()) - 1;
        }
        break;
    }
    case MD_SPAN_IMG: {
        auto* const img = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        if (img->src.text && img->src.size > 0) {
            ctx->pending_image_src = Utf8ToWide(std::string_view{ img->src.text, static_cast<size_t>(img->src.size) });
        }
        break;
    }
    default:
        break;
    }

    return 0;
}

int OnLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    ctx->FlushUtf8();

    if (type == MD_SPAN_IMG) {
        if (ctx->current_node && !ctx->pending_image_src.empty()) {
            ctx->current_node->ensure_image();
            ctx->current_node->image_data->src = ctx->pending_image_src;
        }
        ctx->pending_image_src.clear();
    }

    if (!ctx->span_stack.empty()) {
        ctx->current_span = ctx->span_stack.top();
        ctx->span_stack.pop();
    }

    return 0;
}

void ResolveHtmlEntity(ParseContext* ctx, std::string_view entity)
{
    const wchar_t* resolved = nullptr;
    wchar_t single_char = 0;
    if (entity == "&amp;") {
        resolved = L"&";
    }
    else if (entity == "&lt;") {
        resolved = L"<";
    }
    else if (entity == "&gt;") {
        resolved = L">";
    }
    else if (entity == "&quot;") {
        resolved = L"\"";
    }
    else if (entity == "&apos;") {
        resolved = L"'";
    }
    else if (entity == "&nbsp;") {
        resolved = L"\u00A0";
    }
    else if (entity.size() >= 4 && entity[0] == '&' && entity[1] == '#') {
        unsigned long codepoint = 0;
        bool valid = false;
        if (entity[2] == 'x' || entity[2] == 'X') {
            const auto result = std::from_chars(entity.data() + 3, entity.data() + entity.size() - 1, codepoint, 16);
            valid = (result.ec == std::errc());
        }
        else {
            const auto result = std::from_chars(entity.data() + 2, entity.data() + entity.size() - 1, codepoint, 10);
            valid = (result.ec == std::errc());
        }
        if (valid && codepoint > 0 && codepoint <= 0xFFFF) {
            single_char = static_cast<wchar_t>(codepoint);
            resolved = &single_char;
        }
        else if (valid && codepoint > 0xFFFF && codepoint <= 0x10FFFF) {
            // 補助面: UTF-16サロゲートペアを出力
            const unsigned long adj = codepoint - 0x10000;
            wchar_t surrogate[2];
            surrogate[0] = static_cast<wchar_t>(0xD800 + (adj >> 10));
            surrogate[1] = static_cast<wchar_t>(0xDC00 + (adj & 0x3FF));
            ctx->AppendText(std::wstring_view{ surrogate, 2 });
            return;
        }
    }
    if (resolved) {
        const size_t rlen = (single_char != 0) ? 1 : std::wcslen(resolved);
        ctx->AppendText(std::wstring_view{ resolved, rlen });
    }
    else {
        ctx->AppendUtf8(entity);
    }
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);

    if (!ctx->current_node) {
        return 0;
    }

    // 各ノードの最初のテキストコールバックでソースオフセットを記録
    if (ctx->current_node->source_offset == UINT32_MAX && ctx->markdown_base) {
        ctx->current_node->source_offset = static_cast<uint32_t>(text - ctx->markdown_base);
    }

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        ctx->utf8_accum.append(text, static_cast<size_t>(size));
        break;

    case MD_TEXT_ENTITY:
        ctx->FlushUtf8();
        ResolveHtmlEntity(ctx, std::string_view{ text, static_cast<size_t>(size) });
        break;

    case MD_TEXT_BR:
        ctx->FlushUtf8();
        ctx->AppendText(std::wstring_view{ L"\n", 1 });
        break;

    case MD_TEXT_SOFTBR:
        ctx->FlushUtf8();
        ctx->AppendText(std::wstring_view{ L" ", 1 });
        break;

    default:
        break;
    }

    return 0;
}

} // namespace

// ---- GitHub Alerts 検出 ----

const wchar_t* GetAlertLabel(AlertType type) noexcept
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

const wchar_t* GetAlertIcon(AlertType type) noexcept
{
    switch (type) {
    case AlertType::Note:      return L"\u2139";         // ℹ Information Source
    case AlertType::Tip:       return L"\xD83D\xDCA1";   // 💡 Light Bulb (surrogate pair)
    case AlertType::Important: return L"\u2757";         // ❗ Heavy Exclamation Mark
    case AlertType::Warning:   return L"\u26A0";         // ⚠ Warning Sign
    case AlertType::Caution:   return L"\u26D4";         // ⛔ No Entry
    default:                   return L" ";
    }
}

namespace {

// 大文字小文字を無視して string_view を比較する（ASCII範囲のみ）
bool AsciiCaseEqual(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    const auto len = a.size();
    for (size_t i = 0; i < len; i++) {
        const char ca = (a[i] >= 'a' && a[i] <= 'z') ? (a[i] - 'a' + 'A') : a[i];
        const char cb = (b[i] >= 'a' && b[i] <= 'z') ? (b[i] - 'a' + 'A') : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// テキスト先頭から [!TYPE] パターンを検出し、AlertTypeを返す（UTF-8版）。
// marker_end には ']' の次の位置（スペース/改行をスキップ済み）を設定する。
// マーカーは全てASCIIなので、バイトオフセット＝ワイド文字オフセット。
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

    AlertType type = AlertType::None;
    if (AsciiCaseEqual(type_str, "NOTE")) {
        type = AlertType::Note;
    }
    else if (AsciiCaseEqual(type_str, "TIP")) {
        type = AlertType::Tip;
    }
    else if (AsciiCaseEqual(type_str, "IMPORTANT")) {
        type = AlertType::Important;
    }
    else if (AsciiCaseEqual(type_str, "WARNING")) {
        type = AlertType::Warning;
    }
    else if (AsciiCaseEqual(type_str, "CAUTION")) {
        type = AlertType::Caution;
    }

    if (type == AlertType::None) {
        return AlertType::None;
    }

    marker_end = close + 1;
    // マーカー直後のスペースまたは改行を1つスキップ
    if (marker_end < text.size() && (text[marker_end] == ' ' || text[marker_end] == '\n')) {
        marker_end++;
    }
    return type;
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, AlertType type, size_t marker_end)
{
    const wchar_t* const label = GetAlertLabel(type);
    const size_t label_len = std::wcslen(label);
    const wchar_t* const icon = GetAlertIcon(type);
    const size_t icon_len = std::wcslen(icon);

    const auto& current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    // 新しいテキストを構築: "[icon] Label" (+ "\n \n" + 残りテキスト)
    const size_t icon_prefix_len = icon_len + 1; // アイコン文字列 + スペース
    const size_t full_label_len = icon_prefix_len + label_len;
    std::pmr::wstring new_text;
    new_text.reserve(full_label_len + 4 + (has_content ? current_text.size() - marker_end : 0));
    new_text.append(icon, icon_len);
    new_text += L' ';
    new_text.append(label, label_len);

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
    label_run.bold = true;
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

    node.SetText(new_text);
    node.runs = std::move(new_runs);
    node.alert_type = type;
    node.alert_label_length = static_cast<uint32_t>(full_label_len);
}

} // namespace

void DetectAlerts(std::pmr::vector<Node>& nodes)
{
    const auto node_count = nodes.size();
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].type != NodeType::BlockQuote) {
            continue;
        }
        size_t marker_end = 0;
        const AlertType type = DetectAlertMarker(nodes[i].text_utf8, marker_end);
        if (type == AlertType::None) {
            continue;
        }
        const int group = nodes[i].blockquote_group;
        TransformAlertNode(nodes[i], type, marker_end);

        // 同一 blockquote_group の後続ノードにも同じ alert_type を伝播
        // ノード種別に依存せず、グループIDで判定する（リスト等も含む）
        size_t j = i + 1;
        for (; j < node_count; j++) {
            if (nodes[j].blockquote_group != group) {
                break;
            }
            nodes[j].alert_type = type;
        }
        i = j - 1; // 伝播済みノードをスキップ
    }
}

ParseResult ParseMarkdown(std::string_view markdown_text)
{
    ParseContext ctx;
    ctx.markdown_base = markdown_text.data();
    ctx.utf8_accum.reserve(4096);

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;

    md_parse(markdown_text.data(), static_cast<MD_SIZE>(markdown_text.size()), &parser, &ctx);

    DetectAlerts(ctx.nodes);

    ParseResult result;
    result.nodes = std::move(ctx.nodes);
    result.heading_indices = std::move(ctx.heading_indices);
    result.image_indices = std::move(ctx.image_indices);
    result.mermaid_indices = std::move(ctx.mermaid_indices);
    return result;
}
