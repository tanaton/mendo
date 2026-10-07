#include <gtest/gtest.h>
#include "document.h"
#include "example_files.h"
#include "markdown_fragment_gen.h"
#include "parser.h"
#include "selection_html.h"
#include "syntax.h"
#include "utf8_codec.h"
#include <format>
#include <limits>
#include <memory_resource>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// コピー時は CF_HTML とプレーンテキストを同時にクリップボードへ載せるため、貼り付け先によって
// 内容が食い違わないよう「HTML からタグを除いたテキスト == プレーンテキスト」を保つ。
// 表は HTML 側が常に表全体を出す (部分選択を反映しない) 仕様差があるため比較から除外する。

namespace {

struct StrippedHtml {
    std::string text;
    std::string error; // 空 = タグの入れ子・エスケープが正常
};

void Unescape(std::string_view s, std::string& out, std::string& error)
{
    static constexpr std::pair<std::string_view, char> kEntities[]{
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' },
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        bool matched = false;
        for (const auto& [entity, ch] : kEntities) {
            if (s.substr(i).starts_with(entity)) {
                out += ch;
                i += entity.size() - 1;
                matched = true;
                break;
            }
        }
        if (!matched && error.empty()) {
            error = std::format("unescaped '&' at text offset {}", out.size());
        }
    }
}

// タグを除去し <br> を LF、<img alt> を本文として取り出す。タスクの <input> と直後の空白は装飾として捨てる。
StrippedHtml StripHtml(std::string_view html)
{
    StrippedHtml r;
    std::vector<std::string> stack;
    int pre_depth = 0;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            const size_t next = std::min(html.find('<', i), html.size());
            const auto chunk = html.substr(i, next - i);
            if (pre_depth == 0 && chunk.contains('\n') && r.error.empty()) {
                r.error = "raw LF outside <pre> (should be <br>)";
            }
            Unescape(chunk, r.text, r.error);
            i = next;
            continue;
        }
        const size_t close = html.find('>', i);
        if (close == std::string_view::npos) {
            r.error = "unterminated tag";
            return r;
        }
        const auto tag = html.substr(i + 1, close - i - 1);
        i = close + 1;
        if (tag.starts_with('/')) {
            const auto name = tag.substr(1);
            if (stack.empty() || stack.back() != name) {
                if (r.error.empty()) {
                    r.error = std::format("</{}> does not match <{}>", name, stack.empty() ? "" : stack.back());
                }
                continue;
            }
            stack.pop_back();
            pre_depth -= (name == "pre");
            continue;
        }
        const auto name = tag.substr(0, tag.find(' '));
        if (name == "br") {
            r.text += '\n';
        }
        else if (name == "img") {
            constexpr std::string_view kAlt = "alt=\"";
            const size_t a = tag.find(kAlt);
            if (a != std::string_view::npos) {
                const size_t b = tag.find('"', a + kAlt.size());
                Unescape(tag.substr(a + kAlt.size(), b - a - kAlt.size()), r.text, r.error);
            }
        }
        else if (name == "input") {
            if (i < html.size() && html[i] == ' ') {
                ++i;
            }
        }
        else if (name != "hr") {
            stack.emplace_back(name);
            pre_depth += (name == "pre");
        }
    }
    if (!stack.empty() && r.error.empty()) {
        r.error = std::format("unclosed <{}>", stack.back());
    }
    return r;
}

// ノードを単独で選んだときの HTML (タグ除去後) とプレーンテキストの一致を見る。
// 複数ノード選択の HTML は単独 HTML の連結 (リスト枠を除く) と同じ本文になるはずなので、それも確かめる。
void CheckSelectionRoundTrip(const std::pmr::vector<Node>& nodes, const TextSelection& sel, bool dark)
{
    const auto full = StripHtml(ExtractSelectedTextAsHtml(nodes, sel, dark));
    ASSERT_EQ(full.error, "");

    std::string expected_full;
    for (int i = sel.start_node; i <= sel.end_node; ++i) {
        TextSelection one;
        one.active = true;
        one.start_node = one.end_node = i;
        one.start_pos = (i == sel.start_node) ? sel.start_pos : 0;
        one.end_pos = (i == sel.end_node) ? sel.end_pos : std::numeric_limits<uint32_t>::max();
        const auto html = StripHtml(ExtractSelectedTextAsHtml(nodes, one, dark));
        ASSERT_EQ(html.error, "") << "node " << i;
        expected_full += html.text;
        if (nodes[i].type == NodeType::Table) {
            continue;
        }
        const auto plain = ExtractSelectedText(nodes, one);
        ASSERT_EQ(html.text, std::string_view{ plain })
            << "node " << i << " type " << static_cast<int>(nodes[i].type) << " range [" << one.start_pos << ", " << one.end_pos << ")";
    }
    EXPECT_EQ(full.text, expected_full);
}

void TokenizeCodeBlocks(std::pmr::vector<Node>& nodes)
{
    for (auto& node : nodes) {
        if (node.type == NodeType::CodeBlock && node.code_language() != SyntaxLanguage::None) {
            node.syntax_tokens_mut() = Tokenize(node.GetText(), node.code_language());
        }
    }
}

void CheckRandomSelections(std::pmr::vector<Node>& nodes, uint32_t seed, int count)
{
    if (nodes.empty()) {
        return;
    }
    TokenizeCodeBlocks(nodes);
    std::mt19937 rng{ seed };
    const auto pick = [&rng](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const int last = static_cast<int>(nodes.size()) - 1;
    for (int k = 0; k < count; ++k) {
        TextSelection sel;
        sel.active = true;
        sel.start_node = pick(0, last);
        sel.end_node = std::min(last, sel.start_node + pick(0, 4));
        const auto pos_in = [&](int node) {
            const auto text = nodes[node].LinearizedText();
            const auto pos = static_cast<uint32_t>(pick(0, static_cast<int>(text.size())));
            return pos < text.size() ? utf8_codec::SnapToCpStart(text, pos) : pos;
        };
        sel.start_pos = pos_in(sel.start_node);
        sel.end_pos = pos_in(sel.end_node);
        if (sel.start_node == sel.end_node && sel.start_pos > sel.end_pos) {
            std::swap(sel.start_pos, sel.end_pos);
        }
        SCOPED_TRACE(std::format("selection #{}: ({}, {}) - ({}, {})", k, sel.start_node, sel.start_pos, sel.end_node, sel.end_pos));
        CheckSelectionRoundTrip(nodes, sel, (k & 1) != 0);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

} // namespace

TEST(SelectionHtmlProperty, RandomDocumentsHtmlTextMatchesPlainText)
{
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        MarkdownFragmentGen gen{ seed };
        const std::string md = gen.Document(gen.Range(1, 6));
        SCOPED_TRACE(std::format("seed={} md=\n{}", seed, md));
        auto nodes = ParseMarkdown(std::string_view{ md }).nodes;
        CheckRandomSelections(nodes, seed, 8);
        if (HasFatalFailure()) {
            return;
        }
    }
}

TEST(SelectionHtmlProperty, ExampleFilesHtmlTextMatchesPlainText)
{
    for (const std::string_view path : { "example/test.md", "example/nested.md" }) {
        SCOPED_TRACE(path);
        auto bytes = ReadExampleFileBytes(path);
        if (bytes.empty()) {
            GTEST_SKIP() << path << " not found";
        }
        auto doc = Document::FromMarkdown(std::move(bytes), L"example.md");
        CheckRandomSelections(doc.GetNodesMut(), 7, 300);
        if (HasFatalFailure()) {
            return;
        }
    }
}
