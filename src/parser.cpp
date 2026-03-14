#include "parser.h"
#include "md4c.h"
#include <stack>
#include <windows.h>

std::wstring GenerateAnchorId(const std::wstring& text) {
    std::wstring slug;
    slug.reserve(text.size());
    for (wchar_t c : text) {
        if (c >= L'A' && c <= L'Z') {
            slug += static_cast<wchar_t>(c - L'A' + L'a');
        } else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_') {
            slug += c;
        } else if (c == L' ' || c == L'\t') {
            slug += L'-';
        }
        // CJK characters: keep as-is
        else if (c >= 0x3000) {
            slug += c;
        }
        // Other characters: skip
    }
    return slug;
}

namespace {

std::wstring Utf8ToWide(const char* str, size_t len) {
    if (len == 0) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, static_cast<int>(len), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, static_cast<int>(len), result.data(), wlen);
    return result;
}

struct SpanState {
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strikethrough = false;
    std::optional<std::wstring> link_url;
};

struct ParseContext {
    std::vector<RenderNode> nodes;
    std::stack<SpanState> span_stack;
    SpanState current_span;

    // Block context tracking
    int indent_level = 0;
    bool in_code_block = false;
    bool in_blockquote = false;

    // List tracking
    std::stack<int> list_counter; // 0 = unordered, >0 = ordered counter

    // Table tracking
    bool in_table = false;
    bool in_thead = false;
    TableCell* current_cell = nullptr;
    int current_cell_align = 0;

    // Current node being built
    RenderNode* current_node = nullptr;

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
        // If inside a table cell, append to cell instead
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
                if (ctx->in_blockquote) {
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
                std::wstring lang_str = Utf8ToWide(code_detail->lang.text, code_detail->lang.size);
                ctx->current_node->code_language = DetectLanguage(lang_str);
            }
            break;
        }

        case MD_BLOCK_QUOTE:
            ctx->in_blockquote = true;
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
            // Remove trailing newline if present
            if (ctx->current_node && !ctx->current_node->text.empty()
                && ctx->current_node->text.back() == L'\n') {
                ctx->current_node->text.pop_back();
                if (!ctx->current_node->runs.empty()) {
                    auto& last = ctx->current_node->runs.back();
                    if (last.length > 0) last.length--;
                }
            }
            break;

        case MD_BLOCK_QUOTE:
            ctx->in_blockquote = false;
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
                ctx->current_node->anchor_id = GenerateAnchorId(ctx->current_node->text);
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
                ctx->current_span.link_url = Utf8ToWide(a->href.text, a->href.size);
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
        case MD_TEXT_ENTITY: {
            std::wstring wtext;
            if (type == MD_TEXT_ENTITY) {
                // Handle common HTML entities
                std::string entity(text, size);
                if (entity == "&amp;") wtext = L"&";
                else if (entity == "&lt;") wtext = L"<";
                else if (entity == "&gt;") wtext = L">";
                else if (entity == "&quot;") wtext = L"\"";
                else if (entity == "&apos;") wtext = L"'";
                else if (entity == "&nbsp;") wtext = L"\u00A0";
                else wtext = Utf8ToWide(text, size);
            } else {
                wtext = Utf8ToWide(text, size);
            }
            ctx->AppendText(wtext.c_str(), wtext.size());
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

std::vector<RenderNode> ParseMarkdown(const std::string& markdown_text) {
    ParseContext ctx;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;

    md_parse(markdown_text.c_str(), static_cast<MD_SIZE>(markdown_text.size()), &parser, &ctx);

    return std::move(ctx.nodes);
}
