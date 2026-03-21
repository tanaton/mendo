#include "parser.h"
#include "syntax.h"
#include "memory_resource.h"
#include "md4c.h"
#include <stack>
#include <unordered_map>
#include <windows.h>

std::pmr::wstring GenerateAnchorId(std::wstring_view text) {
    std::pmr::wstring slug;
    slug.reserve(text.size());
    for (wchar_t c : text) {
        if (c >= L'A' && c <= L'Z') {
            slug += static_cast<wchar_t>(c - L'A' + L'a');
        } else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_') {
            slug += c;
        } else if (c == L' ' || c == L'\t') {
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

std::pmr::wstring Utf8ToWide(const char* str, size_t len) {
    if (len == 0) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, static_cast<int>(len), nullptr, 0);
    if (wlen <= 0) return {};
    std::pmr::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, static_cast<int>(len), result.data(), wlen);
    return result;
}

struct SpanState {
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strikethrough = false;
    std::optional<std::pmr::wstring> link_url;
};

struct ParseContext {
    // パース用 monotonic リソース（一括確保→一括解放）
    ScopedMonotonicResource parse_resource{64 * 1024};

    std::pmr::vector<Node> nodes;

    // パース一時データには monotonic リソースを使用
    std::stack<SpanState, std::pmr::deque<SpanState>> span_stack{
        std::pmr::deque<SpanState>{parse_resource.resource()}};
    SpanState current_span;

    // UTF-8 → Wide変換用の再利用可能バッファ
    std::pmr::wstring text_buffer;

    // ブロックコンテキスト追跡
    int indent_level = 0;
    bool in_code_block = false;
    int blockquote_depth = 0;

    // リスト追跡
    std::stack<int, std::pmr::deque<int>> list_counter{
        std::pmr::deque<int>{parse_resource.resource()}}; // 0 = 順序なしリスト, >0 = 順序ありリストのカウンター

    // テーブル追跡
    bool in_table = false;
    bool in_thead = false;
    TableCell* current_cell = nullptr;
    int current_cell_align = 0;

    // 現在構築中のノード
    Node* current_node = nullptr;

    // アンカーIDの一意性追跡: スラグ -> 出現回数
    std::pmr::unordered_map<std::pmr::wstring, int> anchor_counts{parse_resource.resource()};

    void BeginNode(NodeType type) {
        nodes.emplace_back();
        current_node = &nodes.back();
        current_node->type = type;
        current_node->indent_level = indent_level;
    }

    TextRun MakeRun(uint32_t start, uint32_t length) const {
        TextRun run;
        run.start = start;
        run.length = length;
        run.bold = current_span.bold;
        run.italic = current_span.italic;
        run.code = current_span.code;
        run.strikethrough = current_span.strikethrough;
        run.link_url = current_span.link_url;
        return run;
    }

    void AppendText(const wchar_t* text, size_t len) {
        // テーブルセル内の場合、ノードではなくセルに追加
        if (current_cell) {
            uint32_t start = static_cast<uint32_t>(current_cell->text.size());
            current_cell->text.append(text, len);
            current_cell->runs.push_back(MakeRun(start, static_cast<uint32_t>(len)));
            return;
        }

        if (!current_node) return;

        uint32_t start = static_cast<uint32_t>(current_node->text.size());
        current_node->text.append(text, len);
        current_node->runs.push_back(MakeRun(start, static_cast<uint32_t>(len)));
    }

    // UTF-8テキストをワイド文字に変換し、現在のノード/セルに追加する。
    void AppendUtf8(const char* text, int size) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text, size, nullptr, 0);
        if (wlen > 0) {
            text_buffer.resize(static_cast<size_t>(wlen));
            MultiByteToWideChar(CP_UTF8, 0, text, size, text_buffer.data(), wlen);
            AppendText(text_buffer.data(), static_cast<size_t>(wlen));
        }
    }
};

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
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
                } else {
                    ctx->BeginNode(NodeType::Paragraph);
                }
            }
            break;

        case MD_BLOCK_CODE: {
            ctx->in_code_block = true;
            ctx->BeginNode(NodeType::CodeBlock);
            auto* code_detail = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            if (code_detail && code_detail->lang.text && code_detail->lang.size > 0) {
                std::pmr::wstring lang_str = Utf8ToWide(code_detail->lang.text, code_detail->lang.size);
                ctx->current_node->code_language = DetectLanguage(lang_str);
            }
            break;
        }

        case MD_BLOCK_QUOTE:
            ctx->blockquote_depth++;
            ctx->indent_level++;
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
            } else {
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

int OnLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
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
            if (ctx->blockquote_depth > 0) ctx->blockquote_depth--;
            if (ctx->indent_level > 0) ctx->indent_level--;
            break;

        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (!ctx->list_counter.empty()) ctx->list_counter.pop();
            if (ctx->indent_level > 0) ctx->indent_level--;
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
                std::pmr::wstring base_id_pmr{base_id};
                auto it = ctx->anchor_counts.find(base_id_pmr);
                int count = 0;
                if (it != ctx->anchor_counts.end()) {
                    count = it->second;
                }
                if (count > 0) {
                    ctx->current_node->anchor_id = base_id_pmr;
                    ctx->current_node->anchor_id += L"-";
                    ctx->current_node->anchor_id += std::to_wstring(count);
                } else {
                    ctx->current_node->anchor_id = base_id_pmr;
                }
                ctx->anchor_counts[base_id_pmr] = count + 1;
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

int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
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
                std::pmr::wstring url_str = Utf8ToWide(a->href.text, a->href.size);
                ctx->current_span.link_url = std::pmr::wstring{url_str};
            }
            break;
        }
        default:
            break;
    }

    return 0;
}

int OnLeaveSpan(MD_SPANTYPE /*type*/, void* /*detail*/, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);

    if (!ctx->span_stack.empty()) {
        ctx->current_span = ctx->span_stack.top();
        ctx->span_stack.pop();
    }

    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);

    if (!ctx->current_node) return 0;

    switch (type) {
        case MD_TEXT_NORMAL:
        case MD_TEXT_CODE:
            ctx->AppendUtf8(text, static_cast<int>(size));
            break;

        case MD_TEXT_ENTITY: {
            std::string entity(text, size);
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
                    try { codepoint = std::stoul(entity.substr(3, entity.size() - 4), nullptr, 16); valid = true; }
                    catch (...) {}
                } else {
                    try { codepoint = std::stoul(entity.substr(2, entity.size() - 3), nullptr, 10); valid = true; }
                    catch (...) {}
                }
                if (valid && codepoint > 0 && codepoint <= 0xFFFF) {
                    single_char = static_cast<wchar_t>(codepoint);
                    resolved = &single_char;
                } else if (valid && codepoint > 0xFFFF && codepoint <= 0x10FFFF) {
                    // 補助面: UTF-16サロゲートペアを出力
                    unsigned long adj = codepoint - 0x10000;
                    wchar_t surrogate[2];
                    surrogate[0] = static_cast<wchar_t>(0xD800 + (adj >> 10));
                    surrogate[1] = static_cast<wchar_t>(0xDC00 + (adj & 0x3FF));
                    ctx->AppendText(surrogate, 2);
                    break;
                }
            }
            if (resolved) {
                size_t rlen = (single_char != 0) ? 1 : std::wcslen(resolved);
                ctx->AppendText(resolved, rlen);
            } else {
                ctx->AppendUtf8(text, static_cast<int>(size));
            }
            break;
        }

        case MD_TEXT_BR:
            ctx->AppendText(L"\n", 1);
            break;

        case MD_TEXT_SOFTBR:
            ctx->AppendText(L" ", 1);
            break;

        default:
            break;
    }

    return 0;
}

} // namespace

std::pmr::vector<Node> ParseMarkdown(std::string_view markdown_text) {
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

    return std::move(ctx.nodes);
}
