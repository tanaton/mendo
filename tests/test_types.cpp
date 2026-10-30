#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "document_types.h"
#include "text_arena.h"

// ---- TextSelection::MakeOrdered ----

// アンカーとキャレットを文書順に並べ、同じ位置どうしなら非アクティブにする。
TEST(TextSelection, MakeOrderedSortsEndpoints)
{
    struct Case {
        int a_node;
        uint32_t a_pos;
        int b_node;
        uint32_t b_pos;
        TextSelection expected;
        bool active;
    };
    constexpr Case kCases[] = {
        { 3, 5, 3, 10, { 3, 5, 3, 10 }, true },
        { 3, 10, 3, 5, { 3, 5, 3, 10 }, true },
        { 1, 0, 5, 3, { 1, 0, 5, 3 }, true },
        { 5, 3, 1, 0, { 1, 0, 5, 3 }, true },
        { 5, 0, 1, 0, { 1, 0, 5, 0 }, true },
        { 0, 5, 0, 6, { 0, 5, 0, 6 }, true },
        { 100000, 50000, 200000, 99999, { 100000, 50000, 200000, 99999 }, true },
        { 2, 7, 2, 7, { 2, 7, 2, 7 }, false },
        { 0, 0, 0, 0, { 0, 0, 0, 0 }, false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::Message() << "(" << c.a_node << ", " << c.a_pos << ") - (" << c.b_node << ", " << c.b_pos << ")");
        const auto s = TextSelection::MakeOrdered(c.a_node, c.a_pos, c.b_node, c.b_pos);
        EXPECT_EQ(s, c.expected);
        EXPECT_EQ(s.active(), c.active);
    }
}

TEST(TextSelection, ClearResetsStateAndAllowsNewSelection)
{
    auto s = TextSelection::MakeOrdered(1, 0, 5, 3);
    s.Clear();
    EXPECT_FALSE(s.active());
    EXPECT_EQ(s.start_node, -1);
    EXPECT_EQ(s.end_node, -1);

    s = TextSelection::MakeOrdered(2, 1, 3, 4);
    EXPECT_TRUE(s.active());
    EXPECT_EQ(s.start_node, 2);
    EXPECT_EQ(s.end_node, 3);
}

// ---- Nodeのデフォルト状態 ----

TEST(NodeTest, DefaultState)
{
    Node node;
    EXPECT_EQ(node.type, NodeType::Paragraph);
    EXPECT_EQ(node.heading_level, 0);
    EXPECT_EQ(node.indent_level, 0);
    EXPECT_EQ(node.line_count, 0);
    EXPECT_EQ(node.list_number(), 0);
    EXPECT_FALSE(node.task_checked());
    EXPECT_FALSE(node.HasText());
    EXPECT_TRUE(node.GetText().empty());
    EXPECT_TRUE(node.runs().empty());
    EXPECT_EQ(node.code_language, SyntaxLanguage::None);
    EXPECT_FALSE(node.has_table());
}

// ---- TextRun ----

TEST(TextRun, DefaultState)
{
    TextRun run;
    EXPECT_EQ(run.start, 0u);
    EXPECT_EQ(run.length, 0u);
    EXPECT_FALSE(run.bold());
    EXPECT_FALSE(run.italic());
    EXPECT_FALSE(run.code());
    EXPECT_FALSE(run.strikethrough());
    EXPECT_FALSE(run.has_link());
}

// 各フラグのビットは自分の getter にだけ映る。
TEST(TextRun, FlagsAreIndependent)
{
    struct Flag {
        const char* name;
        uint8_t bit;
        bool (TextRun::*get)() const noexcept;
    };
    constexpr Flag kFlags[] = {
        { "bold", TextRun::kBold, &TextRun::bold },
        { "italic", TextRun::kItalic, &TextRun::italic },
        { "code", TextRun::kCode, &TextRun::code },
        { "strikethrough", TextRun::kStrikethrough, &TextRun::strikethrough },
        { "double_dollar_math", TextRun::kMathDoubleDollar, &TextRun::double_dollar_math },
    };
    uint8_t all = 0;
    for (const auto& f : kFlags) {
        all |= f.bit;
    }
    for (const auto& target : kFlags) {
        SCOPED_TRACE(target.name);
        TextRun only;
        only.set_raw_flags(target.bit);
        TextRun all_but;
        all_but.set_raw_flags(static_cast<uint8_t>(all & ~target.bit));
        EXPECT_EQ(only.raw_flags(), target.bit);
        for (const auto& f : kFlags) {
            const bool is_target = &f == &target;
            EXPECT_EQ((only.*f.get)(), is_target) << f.name;
            EXPECT_EQ((all_but.*f.get)(), !is_target) << f.name;
        }
    }
}

TEST(TextRun, HasLinkWhenIndexIsNonNegative)
{
    TextRun run;
    run.link_url_index = 0;
    EXPECT_TRUE(run.has_link());
    run.link_url_index = -1;
    EXPECT_FALSE(run.has_link());
}

// ---- AlertColorIndex ----

// Note から順に 0 始まり。None は色を持たない番兵 ALERT_TYPE_COUNT。
TEST(AlertColorIndex, MapsTypesInDeclarationOrder)
{
    EXPECT_EQ(AlertColorIndex(AlertType::None), ALERT_TYPE_COUNT);
    EXPECT_EQ(AlertColorIndex(AlertType::Note), 0u);
    EXPECT_EQ(AlertColorIndex(AlertType::Tip), 1u);
    EXPECT_EQ(AlertColorIndex(AlertType::Important), 2u);
    EXPECT_EQ(AlertColorIndex(AlertType::Warning), 3u);
    EXPECT_EQ(AlertColorIndex(AlertType::Caution), 4u);
}

// ---- Node::line_count ----

// line_count は呼び出し側が渡した値をそのまま持ち、テキストから数え直さない
// (パーサがマーカー除去後に差分計算で渡すため)。
TEST(NodeLineCount, SetTextStoresGivenValue)
{
    Node node;
    node.SetText("a\nb\nc", 2);
    EXPECT_EQ(node.GetText(), "a\nb\nc");
    EXPECT_EQ(node.line_count, 2);
    node.SetText("a\nb\nc\nd\ne", 7);
    EXPECT_EQ(node.line_count, 7);
    node.SetText("a\nb", 0);
    EXPECT_EQ(node.line_count, 0);
}

// テキストと runs はコピーせず、渡した領域をそのまま指す (置き場所の寿命は呼び出し側が持つ)。
TEST(NodeTest, SetTextAndRunsDoNotCopy)
{
    static constexpr std::string_view text = "bold text";
    TextRun run;
    run.length = 4;
    run.set_raw_flags(TextRun::kBold);
    const TextRun runs[]{ run };

    Node node;
    node.SetText(text, 0);
    node.SetRuns(runs);
    EXPECT_EQ(node.GetText().data(), text.data());
    ASSERT_EQ(node.runs().size(), 1u);
    EXPECT_EQ(node.runs().data(), runs);
}

TEST(NodeTest, SourceOffsetUnsetByDefault)
{
    Node node;
    EXPECT_FALSE(node.HasSourceOffset());
    EXPECT_EQ(node.SourceOffset(), kUnsetSourceOffset);
    node.SetSourceOffset(0);
    EXPECT_TRUE(node.HasSourceOffset());
    EXPECT_EQ(node.SourceOffset(), 0u);
}

// payload は種別ごとに使い回すので、種別が合わないノードからは見えない。
TEST(NodeTest, TableDataRequiresTableType)
{
    NodeTableData table;
    Node node;
    node.set_table_data(&table);
    EXPECT_FALSE(node.has_table());
    EXPECT_EQ(node.table_data(), nullptr);
    node.type = NodeType::Table;
    EXPECT_TRUE(node.has_table());
    EXPECT_EQ(node.table_data(), &table);
    EXPECT_EQ(std::as_const(node).table_data(), &table);
}

TEST(NodeTest, ImageSrcRequiresImageType)
{
    static constexpr std::string_view kSrc = "a.png";
    Node node;
    node.set_image_src(&kSrc);
    EXPECT_TRUE(node.image_src().empty());
    node.type = NodeType::Image;
    EXPECT_EQ(node.image_src(), "a.png");
}

// テキストの置き場所は種別や言語に依存しない。
TEST(NodeTest, TextStoredRegardlessOfType)
{
    struct Case {
        NodeType type;
        SyntaxLanguage language;
        std::string_view text;
    };
    constexpr Case kCases[] = {
        { NodeType::Diagram, SyntaxLanguage::Mermaid, "graph TD;A-->B" },
        { NodeType::CodeBlock, SyntaxLanguage::Cpp, "int main() { return 0; }" },
        { NodeType::Paragraph, SyntaxLanguage::None, "paragraph content" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.text);
        Node node;
        node.type = c.type;
        node.code_language = c.language;
        node.SetText(c.text, 0);
        EXPECT_EQ(node.GetText(), c.text);
        EXPECT_EQ(node.code_language, c.language);
    }
}

// alert_type は伝播ロジック (parser.cpp の DetectAlertAt) で後続ノードにも付く。
// ラベル長は先頭のノードだけが持ち、伝播先ノードは 0 のはず。
TEST(NodeTest, AlertLabelLengthOnlyOnHead)
{
    Node body;
    body.type = NodeType::BlockQuote;
    body.alert_type = AlertType::Warning;
    body.set_has_alert_label();
    EXPECT_EQ(body.alert_type, AlertType::Warning);
    EXPECT_EQ(body.alert_label_length(), 11u); // ⚠ 3 + ' ' 1 + "Warning" 7

    Node propagated;
    propagated.type = NodeType::Paragraph;
    propagated.alert_type = AlertType::Warning;
    EXPECT_EQ(propagated.alert_type, AlertType::Warning);
    EXPECT_EQ(propagated.alert_label_length(), 0u);
}

// 番号を持たないノードは list_ordered=false, list_number=0。
TEST(NodeTest, ListGettersDefaultsWhenNoData)
{
    Node node;
    EXPECT_FALSE(node.list_ordered());
    EXPECT_EQ(node.list_number(), 0);
    EXPECT_FALSE(node.task_checked());

    node.set_list_number(5);
    node.set_task_checked();
    EXPECT_TRUE(node.list_ordered());
    EXPECT_EQ(node.list_number(), 5);
    EXPECT_TRUE(node.task_checked());
}

// 0 始まりの順序付きリストも番号を持つ。
TEST(NodeTest, ListNumberZeroIsOrdered)
{
    Node node;
    node.set_list_number(0);
    EXPECT_TRUE(node.list_ordered());
    EXPECT_EQ(node.list_number(), 0);
}

// チャンクをまたいでも、専用に確保する大きな要求を挟んでも、先に置いたものは動かず壊れない。
TEST(TextArenaTest, CopiesSurviveChunkGrowthAndLargeRequests)
{
    TextArena arena;
    std::vector<std::string> sources;
    std::vector<std::string_view> copies;
    for (size_t i = 0; i < 4000; ++i) {
        const size_t len = (i % 1000 == 999) ? 300 * 1024 : 1 + (i * 37) % 2000;
        sources.emplace_back(len, static_cast<char>('a' + i % 26));
        copies.push_back(arena.CopyText(sources.back()));
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        ASSERT_EQ(copies[i], sources[i]) << "i=" << i;
        ASSERT_NE(copies[i].data(), sources[i].data());
    }
}

// 移し先が配った領域を持ち続け、移し元に書いても移し先のチャンクを上書きしない。
TEST(TextArenaTest, MoveKeepsCopiesAndDetachesSource)
{
    TextArena source;
    const auto hello = source.CopyText("hello");
    TextArena moved = std::move(source);
    const auto world = source.CopyText("world");
    const auto after = moved.CopyText("after");
    EXPECT_EQ(hello, "hello");
    EXPECT_EQ(world, "world");
    EXPECT_EQ(after, "after");
    EXPECT_EQ(after.data(), hello.data() + hello.size()) << "移し先は続きから積む";
}

TEST(TextArenaTest, CopyKeepsAlignmentAfterOddSizedText)
{
    TextArena arena;
    (void)arena.CopyText("abc");
    TextRun run;
    run.start = 7;
    run.length = 3;
    const std::vector<TextRun> runs{ run, run };
    const auto copied = arena.Copy(runs);
    ASSERT_EQ(copied.size(), 2u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(copied.data()) % alignof(TextRun), 0u);
    EXPECT_EQ(copied[1].start, 7u);
    EXPECT_EQ(copied[1].length, 3u);
}

TEST(TextArenaTest, EmptyInputsReturnEmpty)
{
    TextArena arena;
    EXPECT_TRUE(arena.CopyText("").empty());
    EXPECT_TRUE(arena.Copy(std::vector<TextRun>{}).empty());
}
