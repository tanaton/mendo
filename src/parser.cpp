#include "parser.h"
#include "syntax.h"
#include "memory_resource.h"
#include "md4c.h"
#include <stack>
#include <unordered_map>
#include <charconv>
#include <windows.h>

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
            if (c <= 0x303F) skip = true;
            // 全角ASCII対応の句読点
            else if (c >= 0xFF01 && c <= 0xFF0F) skip = true; // ！＂＃…（）＊＋，－．／
            else if (c >= 0xFF1A && c <= 0xFF20) skip = true; // ：；＜＝＞？＠
            else if (c >= 0xFF3B && c <= 0xFF40) skip = true; // ［＼］＾＿｀
            else if (c >= 0xFF5B && c <= 0xFF65) skip = true; // ｛｜｝～…･
            if (!skip) {
                slug += c;
            }
        }
        // その他の文字: スキップ
    }
    return slug;
}

namespace {

std::pmr::wstring Utf8ToWide(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::pmr::wstring result;
    result.resize_and_overwrite(static_cast<size_t>(wlen), [utf8](wchar_t* buf, size_t count) -> size_t {
        return static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), buf, static_cast<int>(count)));
    });
    return result;
}

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

    // パース一時データには monotonic リソースを使用
    std::stack<SpanState, std::pmr::deque<SpanState>> span_stack{
        std::pmr::deque<SpanState>{parse_resource.resource()} };
    SpanState current_span;

    // UTF-8 → Wide変換用の再利用可能バッファ
    std::pmr::wstring text_buffer;

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

    // リンクURL格納: SpanStateではインデックスのみ保持し、push/popでの文字列コピーを回避
    std::pmr::vector<std::pmr::wstring> link_urls{ parse_resource.resource() };

    // アンカーIDの一意性追跡: スラグ -> 出現回数
    std::pmr::unordered_map<std::pmr::wstring, int> anchor_counts{ parse_resource.resource() };

    void BeginNode(NodeType type)
    {
        nodes.emplace_back();
        current_node = &nodes.back();
        current_node->type = type;
        current_node->indent_level = indent_level;
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
            for (size_t i = 0; i < current_node->link_urls.size(); i++) {
                if (current_node->link_urls[i] == url) {
                    node_idx = static_cast<int16_t>(i);
                    break;
                }
            }
            if (node_idx < 0) {
                node_idx = static_cast<int16_t>(current_node->link_urls.size());
                current_node->link_urls.push_back(url);
            }
            run.link_url_index = node_idx;
        }
        return run;
    }

    void AppendText(std::wstring_view text)
    {
        // テーブルセル内の場合、ノードではなくセルに追加
        if (current_cell) {
            uint32_t start = static_cast<uint32_t>(current_cell->text.size());
            current_cell->text.append(text);
            current_cell->runs.push_back(MakeRun(start, static_cast<uint32_t>(text.size())));
            return;
        }

        if (!current_node) {
            return;
        }

        uint32_t start = static_cast<uint32_t>(current_node->text.size());
        current_node->text.append(text);
        current_node->runs.push_back(MakeRun(start, static_cast<uint32_t>(text.size())));
    }

    // UTF-8テキストをワイド文字に変換し、現在のノード/セルに追加する。
    void AppendUtf8(std::string_view text)
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (wlen > 0) {
            text_buffer.resize_and_overwrite(static_cast<size_t>(wlen), [text](wchar_t* buf, size_t count) -> size_t {
                int written = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buf, static_cast<int>(count));
                return (written > 0) ? static_cast<size_t>(written) : 0;
            });
            AppendText(text_buffer);
        }
    }
};

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    auto* ctx = static_cast<ParseContext*>(userdata);

    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_H: {
        auto* h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
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
        auto* code_detail = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        if (code_detail && code_detail->lang.text && code_detail->lang.size > 0) {
            std::pmr::wstring lang_str = Utf8ToWide(std::string_view{ code_detail->lang.text, static_cast<size_t>(code_detail->lang.size) });
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
        auto* ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        ctx->list_counter.push(static_cast<int>(ol->start));
        ctx->indent_level++;
        break;
    }

    case MD_BLOCK_LI: {
        auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (li->is_task) {
            ctx->BeginNode(NodeType::TaskListItem);
            ctx->current_node->task_checked = (li->task_mark == 'x' || li->task_mark == 'X');
        }
        else {
            ctx->BeginNode(NodeType::ListItem);
        }
        if (!ctx->list_counter.empty()) {
            int counter = ctx->list_counter.top();
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
            ctx->current_node->table_rows.emplace_back();
        }
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        if (ctx->current_node && ctx->current_node->type == NodeType::Table
            && !ctx->current_node->table_rows.empty()) {
            auto& row = ctx->current_node->table_rows.back();
            row.cells.emplace_back();
            ctx->current_cell = &row.cells.back();
            ctx->current_cell->is_header = (type == MD_BLOCK_TH);
            if (detail) {
                auto* td = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
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
    auto* ctx = static_cast<ParseContext*>(userdata);

    switch (type) {
    case MD_BLOCK_CODE:
        ctx->in_code_block = false;
        // 末尾の改行があれば除去
        if (ctx->current_node && !ctx->current_node->text.empty()
            && ctx->current_node->text.back() == L'\n') {
            ctx->current_node->text.pop_back();
            if (!ctx->current_node->runs.empty()) {
                auto& last = ctx->current_node->runs.back();
                if (last.length > 0) last.length--;
            }
        }
        // レイアウトパスの度にではなく、パース時に一度だけトークン化する
        if (ctx->current_node && ctx->current_node->code_language != SyntaxLanguage::None) {
            ctx->current_node->syntax_tokens = Tokenize(
                ctx->current_node->text, ctx->current_node->code_language);
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
            std::pmr::wstring base_id = GenerateAnchorId(ctx->current_node->text);
            auto it = ctx->anchor_counts.find(base_id);
            int count = (it != ctx->anchor_counts.end()) ? it->second : 0;
            if (count > 0) {
                ctx->current_node->anchor_id = base_id;
                ctx->current_node->anchor_id += L"-";
                ctx->current_node->anchor_id += std::to_wstring(count);
            }
            else {
                ctx->current_node->anchor_id = base_id;
            }
            ctx->anchor_counts[std::move(base_id)] = count + 1;
        }
        ctx->current_node = nullptr;
        break;
    case MD_BLOCK_P:
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
    auto* ctx = static_cast<ParseContext*>(userdata);

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
        auto* a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        if (a->href.text && a->href.size > 0) {
            ctx->link_urls.push_back(Utf8ToWide(std::string_view{ a->href.text, static_cast<size_t>(a->href.size) }));
            ctx->current_span.link_url_index = static_cast<int>(ctx->link_urls.size()) - 1;
        }
        break;
    }
    default:
        break;
    }

    return 0;
}

int OnLeaveSpan(MD_SPANTYPE /*type*/, void* /*detail*/, void* userdata)
{
    auto* ctx = static_cast<ParseContext*>(userdata);

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
    if (entity == "&amp;")  resolved = L"&";
    else if (entity == "&lt;")   resolved = L"<";
    else if (entity == "&gt;")   resolved = L">";
    else if (entity == "&quot;") resolved = L"\"";
    else if (entity == "&apos;") resolved = L"'";
    else if (entity == "&nbsp;") resolved = L"\u00A0";
    else if (entity.size() >= 4 && entity[0] == '&' && entity[1] == '#') {
        unsigned long codepoint = 0;
        bool valid = false;
        if (entity[2] == 'x' || entity[2] == 'X') {
            auto result = std::from_chars(entity.data() + 3, entity.data() + entity.size() - 1, codepoint, 16);
            valid = (result.ec == std::errc());
        }
        else {
            auto result = std::from_chars(entity.data() + 2, entity.data() + entity.size() - 1, codepoint, 10);
            valid = (result.ec == std::errc());
        }
        if (valid && codepoint > 0 && codepoint <= 0xFFFF) {
            single_char = static_cast<wchar_t>(codepoint);
            resolved = &single_char;
        }
        else if (valid && codepoint > 0xFFFF && codepoint <= 0x10FFFF) {
            // 補助面: UTF-16サロゲートペアを出力
            unsigned long adj = codepoint - 0x10000;
            wchar_t surrogate[2];
            surrogate[0] = static_cast<wchar_t>(0xD800 + (adj >> 10));
            surrogate[1] = static_cast<wchar_t>(0xDC00 + (adj & 0x3FF));
            ctx->AppendText(std::wstring_view{ surrogate, 2 });
            return;
        }
    }
    if (resolved) {
        size_t rlen = (single_char != 0) ? 1 : std::wcslen(resolved);
        ctx->AppendText(std::wstring_view{ resolved, rlen });
    }
    else {
        ctx->AppendUtf8(entity);
    }
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    auto* ctx = static_cast<ParseContext*>(userdata);

    if (!ctx->current_node) {
        return 0;
    }

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        ctx->AppendUtf8(std::string_view{ text, static_cast<size_t>(size) });
        break;

    case MD_TEXT_ENTITY:
        ResolveHtmlEntity(ctx, std::string_view{ text, static_cast<size_t>(size) });
        break;

    case MD_TEXT_BR:
        ctx->AppendText(std::wstring_view{ L"\n", 1 });
        break;

    case MD_TEXT_SOFTBR:
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

namespace {

// 大文字小文字を無視して wstring_view を比較する（ASCII範囲のみ）
bool AsciiCaseEqual(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        wchar_t ca = (a[i] >= L'a' && a[i] <= L'z') ? (a[i] - L'a' + L'A') : a[i];
        wchar_t cb = (b[i] >= L'a' && b[i] <= L'z') ? (b[i] - L'a' + L'A') : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// テキスト先頭から [!TYPE] パターンを検出し、AlertTypeを返す。
// marker_end には ']' の次の位置（スペース/改行をスキップ済み）を設定する。
AlertType DetectAlertMarker(std::wstring_view text, size_t& marker_end)
{
    if (text.size() < 3 || text[0] != L'[' || text[1] != L'!') {
        return AlertType::None;
    }
    auto close = text.find(L']');
    if (close == std::wstring_view::npos || close <= 2) {
        return AlertType::None;
    }

    auto type_str = text.substr(2, close - 2);

    AlertType type = AlertType::None;
    if (AsciiCaseEqual(type_str, L"NOTE"))      type = AlertType::Note;
    else if (AsciiCaseEqual(type_str, L"TIP"))       type = AlertType::Tip;
    else if (AsciiCaseEqual(type_str, L"IMPORTANT")) type = AlertType::Important;
    else if (AsciiCaseEqual(type_str, L"WARNING"))   type = AlertType::Warning;
    else if (AsciiCaseEqual(type_str, L"CAUTION"))   type = AlertType::Caution;

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

// マーカーを除去しラベルを挿入する。TextRunも調整する。
void TransformAlertNode(Node& node, AlertType type, size_t marker_end)
{
    const wchar_t* label = GetAlertLabel(type);
    size_t label_len = std::wcslen(label);

    bool has_content = (marker_end < node.text.size());

    // 新しいテキストを構築: "Label" (+ "\n" + 残りテキスト)
    std::pmr::wstring new_text;
    new_text.reserve(label_len + 1 + (has_content ? node.text.size() - marker_end : 0));
    new_text.append(label, label_len);

    size_t new_content_start = label_len;
    if (has_content) {
        new_text += L'\n';
        new_content_start = label_len + 1;
        new_text.append(node.text.c_str() + marker_end, node.text.size() - marker_end);
    }

    // TextRun の調整
    int delta = static_cast<int>(new_content_start) - static_cast<int>(marker_end);

    std::pmr::vector<TextRun> new_runs;
    // ラベル用の太字ラン
    TextRun label_run;
    label_run.start = 0;
    label_run.length = static_cast<uint32_t>(label_len);
    label_run.bold = true;
    new_runs.push_back(label_run);

    // 元のランを調整（マーカー部分を除外）
    for (const auto& run : node.runs) {
        uint32_t run_end = run.start + run.length;
        if (run_end <= static_cast<uint32_t>(marker_end)) continue;

        TextRun adjusted = run;
        if (adjusted.start < static_cast<uint32_t>(marker_end)) {
            uint32_t trim = static_cast<uint32_t>(marker_end) - adjusted.start;
            adjusted.start = static_cast<uint32_t>(marker_end);
            adjusted.length -= trim;
        }
        adjusted.start = static_cast<uint32_t>(static_cast<int>(adjusted.start) + delta);
        new_runs.push_back(adjusted);
    }

    node.text = std::move(new_text);
    node.runs = std::move(new_runs);
    node.alert_type = type;
    node.alert_label_length = static_cast<uint32_t>(label_len);
}

} // namespace

void DetectAlerts(std::pmr::vector<Node>& nodes)
{
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].type != NodeType::BlockQuote) continue;

        size_t marker_end = 0;
        AlertType type = DetectAlertMarker(nodes[i].text, marker_end);
        if (type == AlertType::None) continue;

        int group = nodes[i].blockquote_group;
        TransformAlertNode(nodes[i], type, marker_end);

        // 同一 blockquote_group の後続ノードにも同じ alert_type を伝播
        // ノード種別に依存せず、グループIDで判定する（リスト等も含む）
        size_t j = i + 1;
        for (; j < nodes.size(); j++) {
            if (nodes[j].blockquote_group != group) break;
            nodes[j].alert_type = type;
        }
        i = j - 1; // 伝播済みノードをスキップ
    }
}

std::pmr::vector<Node> ParseMarkdown(std::string_view markdown_text)
{
    ParseContext ctx;

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

    return std::move(ctx.nodes);
}
