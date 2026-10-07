#include <gtest/gtest.h>
#include "document.h"
#include "example_files.h"
#include "markdown_fragment_gen.h"
#include "parse_invariants.h"
#include "parser.h"
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// ---- 構造不変条件 ----

TEST(ParserProperty, RandomDocumentsSatisfyStructuralInvariants)
{
    for (uint32_t seed = 1; seed <= 1000; ++seed) {
        MarkdownFragmentGen gen{ seed };
        const std::string md = gen.Document(gen.Range(1, 6));
        SCOPED_TRACE(std::format("seed={} md=\n{}", seed, md));
        const auto result = ParseMarkdown(std::string_view{ md });
        ASSERT_TRUE(ParseInvariantsHold(md, result.nodes));
    }
}

TEST(ParserProperty, ExampleFilesSatisfyStructuralInvariants)
{
    for (const std::string_view path : { "example/test.md", "example/nested.md" }) {
        SCOPED_TRACE(path);
        auto bytes = ReadExampleFileBytes(path);
        if (bytes.empty()) {
            GTEST_SKIP() << path << " not found";
        }
        const auto doc = Document::FromMarkdown(std::move(bytes), L"example.md");
        ASSERT_FALSE(doc.GetNodes().empty());
        EXPECT_TRUE(ParseInvariantsHold(doc.GetRawText(), doc.GetNodes()));
    }
}

// ---- tight / loose リスト ----

namespace {

enum class WordKind : uint8_t {
    Text, // Paragraph / ListItem / BlockQuote (tight は LI に、loose は P に入るため区別しない)
    Heading,
    Code,
    Table,
};

WordKind KindOf(const Node& node)
{
    switch (node.type) {
    case NodeType::Heading:
        return WordKind::Heading;
    case NodeType::CodeBlock:
        return WordKind::Code;
    case NodeType::Table:
        return WordKind::Table;
    default:
        return WordKind::Text;
    }
}

// "w<数字>" トークンを種別ごとに集める。
std::map<std::string, WordKind> CollectWords(const std::pmr::vector<Node>& nodes)
{
    std::map<std::string, WordKind> words;
    for (const auto& node : nodes) {
        const auto text = node.LinearizedText();
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != 'w' || (i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])))) {
                continue;
            }
            size_t j = i + 1;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
                ++j;
            }
            if (j > i + 1) {
                words.emplace(std::string{ text.substr(i, j - i) }, KindOf(node));
            }
            i = j - 1;
        }
    }
    return words;
}

std::string KindName(WordKind k)
{
    static constexpr std::array<const char*, 4> kNames{ "Text", "Heading", "Code", "Table" };
    return kNames[static_cast<size_t>(k)];
}

std::string Describe(const std::map<std::string, WordKind>& words)
{
    std::string out;
    for (const auto& [w, k] : words) {
        out += std::format("{}:{} ", w, KindName(k));
    }
    return out;
}

} // namespace

// tight list では md4c が LI 直下の段落を MD_BLOCK_P で囲まないので、見出し / HR / フェンスの後に
// 続く本文の受け皿が無い。全ての語が意図した種別のノードに 1 回ずつ現れることを tight / loose 双方で確かめる。
TEST(ParserProperty, TightAndLooseListsKeepEveryWordInItsBlockKind)
{
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        std::mt19937 rng{ seed };
        const auto pick = [&rng](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        int next_word = 0;
        std::map<std::string, WordKind> expected;
        const auto word = [&](WordKind kind) {
            std::string w = "w" + std::to_string(next_word++);
            expected.emplace(w, kind);
            return w;
        };

        // タスクマーカー直後のフェンス / 見出しはブロックにならないので通常マーカーだけを使う。
        static constexpr std::array<std::string_view, 3> kMarkers{ "- ", "1. ", "* " };
        const auto marker = kMarkers[pick(0, 2)];
        const std::string indent(marker.size(), ' ');
        std::vector<std::vector<std::string>> items(pick(1, 3));
        for (auto& blocks : items) {
            const int n = pick(1, 4);
            bool prev_paragraph = false;
            for (int b = 0; b < n; ++b) {
                const bool last = (b == n - 1);
                // md4c の表は段落を中断できず、後続行も行として吸収するため末尾・非段落直後に限る。
                const int kind = pick(0, (last && !prev_paragraph) ? 4 : 3);
                prev_paragraph = (kind == 0);
                switch (kind) {
                case 0:
                    blocks.push_back(word(WordKind::Text) + " **" + word(WordKind::Text) + "**");
                    break;
                case 1:
                    blocks.push_back("## " + word(WordKind::Heading));
                    break;
                case 2:
                    blocks.push_back("```\n" + word(WordKind::Code) + "\n```");
                    break;
                case 3:
                    blocks.push_back("***");
                    break;
                default: {
                    const auto h = word(WordKind::Table);
                    const auto c = word(WordKind::Table);
                    blocks.push_back("| " + h + " |\n| - |\n| " + c + " |");
                    break;
                }
                }
            }
        }
        const auto build = [&](std::string_view block_sep, std::string_view item_sep) {
            std::string md;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i > 0) {
                    md += item_sep;
                }
                std::string body;
                for (size_t b = 0; b < items[i].size(); ++b) {
                    if (b > 0) {
                        body += block_sep;
                    }
                    body += items[i][b];
                }
                md += MarkdownFragmentGen::PrefixLines(body, marker, indent, "");
            }
            return md;
        };

        for (const auto& md : { build("\n", "\n"), build("\n\n", "\n\n") }) {
            SCOPED_TRACE(std::format("seed={} md=\n{}", seed, md));
            const auto result = ParseMarkdown(std::string_view{ md });
            const auto actual = CollectWords(result.nodes);
            ASSERT_EQ(actual, expected) << "actual:   " << Describe(actual) << "\nexpected: " << Describe(expected);
            ASSERT_TRUE(ParseInvariantsHold(md, result.nodes));
        }
    }
}

// ---- 見出しアンカー ----

// TOC / 内部リンクは anchor_id → ノードの逆引きなので、衝突すると後続見出しへ飛べなくなる。
TEST(ParserProperty, HeadingAnchorsAreUniqueAndResolveToTheirHeading)
{
    static constexpr std::array<std::string_view, 9> kVocab{ "A", "A-1", "A 1", "a", "A-1-1", "a-2", "!!!", "日本", "日本-1" };
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        std::mt19937 rng{ seed };
        const int n = std::uniform_int_distribution<int>(1, 8)(rng);
        std::string md;
        for (int i = 0; i < n; ++i) {
            md += "## ";
            md += kVocab[std::uniform_int_distribution<size_t>(0, kVocab.size() - 1)(rng)];
            md += "\n\n";
        }
        SCOPED_TRACE(std::format("seed={} md=\n{}", seed, md));
        const auto doc = Document::FromMarkdown(std::pmr::string{ md }, L"");
        const auto& nodes = doc.GetNodes();
        ASSERT_EQ(nodes.size(), static_cast<size_t>(n));
        std::set<std::string_view> seen;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto anchor = nodes[i].anchor_id();
            if (anchor.empty()) {
                continue;
            }
            EXPECT_TRUE(seen.insert(anchor).second) << "duplicate anchor '" << anchor << "' at node " << i;
            EXPECT_EQ(doc.FindAnchorIndex(anchor), static_cast<int>(i)) << "anchor '" << anchor << "'";
        }
    }
}

// ---- Alert 変換 ----

namespace {

struct ByteStyle {
    uint8_t flags = 0xFF; // run 外
    std::string url;
    bool operator==(const ByteStyle&) const = default;
};

// text[from..] の各 byte に掛かる (書式, リンク先) を並べる。run の分割位置に依存しない比較用。
std::vector<ByteStyle> StylesFrom(const Node& node, size_t from)
{
    const auto text = node.GetText();
    std::vector<ByteStyle> styles(text.size() - from);
    const auto urls = node.view_link_urls();
    for (const auto& r : node.runs) {
        for (size_t p = std::max<size_t>(r.start, from); p < static_cast<size_t>(r.start) + r.length && p < text.size(); ++p) {
            auto& s = styles[p - from];
            s.flags = static_cast<uint8_t>((r.bold() ? TextRun::kBold : 0) | (r.italic() ? TextRun::kItalic : 0) |
                                           (r.code() ? TextRun::kCode : 0) | (r.strikethrough() ? TextRun::kStrikethrough : 0));
            s.url = r.has_link() ? std::string{ urls[static_cast<size_t>(r.link_url_index)] } : std::string{};
        }
    }
    return styles;
}

} // namespace

// [!NOTE] と、Alert として認識されない同じ長さの [!NOTX] を比べ、変換後の本文テキストと
// 各 byte の書式が「マーカーを除いた元の本文」と一致することを確かめる。
TEST(ParserProperty, AlertTransformKeepsBodyTextAndStyles)
{
    static constexpr std::array<std::string_view, 5> kTypes{ "NOTE", "tip", "Important", "WARNING", "caution" };
    static constexpr std::array<AlertType, 5> kAlertTypes{ AlertType::Note, AlertType::Tip, AlertType::Important, AlertType::Warning, AlertType::Caution };
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        MarkdownFragmentGen gen{ seed };
        const size_t t = static_cast<size_t>(gen.Range(0, 4));
        std::string type{ kTypes[t] };
        std::string body;
        const int same_line = gen.Range(0, 2);
        if (same_line == 1) {
            body += " " + gen.InlineLine();
        }
        const int lines = gen.Range(0, 2);
        for (int i = 0; i < lines; ++i) {
            body += "\n> " + gen.InlineLine();
        }
        // 画像を含む引用は Image ノードへ昇格し Alert 判定対象外になる (既存仕様) ため除く。
        if (body.contains("![")) {
            continue;
        }
        const std::string alert_md = "> [!" + type + "]" + body;
        type.back() = (type.back() == 'X') ? 'Y' : 'X';
        const std::string plain_md = "> [!" + type + "]" + body;
        SCOPED_TRACE(std::format("seed={} md=\n{}", seed, alert_md));

        const auto alert = ParseMarkdown(std::string_view{ alert_md });
        const auto plain = ParseMarkdown(std::string_view{ plain_md });
        ASSERT_TRUE(ParseInvariantsHold(alert_md, alert.nodes));
        ASSERT_FALSE(alert.nodes.empty());
        ASSERT_EQ(alert.nodes.size(), plain.nodes.size());
        const auto& a = alert.nodes[0];
        const auto& p = plain.nodes[0];
        ASSERT_EQ(a.alert_type, kAlertTypes[t]);
        ASSERT_EQ(p.alert_type, AlertType::None);

        const auto ptext = p.GetText();
        size_t marker_end = type.size() + 3;
        if (marker_end < ptext.size() && (ptext[marker_end] == ' ' || ptext[marker_end] == '\n')) {
            ++marker_end;
        }
        const auto label = std::string{ GetAlertIcon(a.alert_type) } + " " + std::string{ GetAlertLabel(a.alert_type) };
        const auto atext = a.GetText();
        ASSERT_TRUE(atext.starts_with(label));
        ASSERT_EQ(a.alert_label_length(), label.size());
        if (marker_end >= ptext.size()) {
            EXPECT_EQ(atext, label);
            continue;
        }
        ASSERT_EQ(atext.substr(label.size()), "\n" + std::string{ ptext.substr(marker_end) });
        EXPECT_EQ(StylesFrom(a, label.size() + 1), StylesFrom(p, marker_end));
    }
}
