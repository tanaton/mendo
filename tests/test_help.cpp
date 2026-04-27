#include <gtest/gtest.h>
#include "document.h"
#include "document_utils.h"
#include "nav.h"

// ═══════════════════════════════════════════════
// IsHelpPath
// ═══════════════════════════════════════════════

TEST(HelpPathTest, MatchesHelpPath)
{
    EXPECT_TRUE(IsHelpPath(L"mendo://help"));
}

TEST(HelpPathTest, RejectsEmptyPath)
{
    EXPECT_FALSE(IsHelpPath(L""));
}

TEST(HelpPathTest, RejectsNormalFilePath)
{
    EXPECT_FALSE(IsHelpPath(L"C:\\docs\\readme.md"));
}

TEST(HelpPathTest, RejectsSimilarPath)
{
    EXPECT_FALSE(IsHelpPath(L"mendo://help/extra"));
    EXPECT_FALSE(IsHelpPath(L"mendo://hel"));
    EXPECT_FALSE(IsHelpPath(L"MENDO://HELP"));
}

TEST(HelpPathTest, ConstantValueIsCorrect)
{
    EXPECT_EQ(HELP_PATH, L"mendo://help");
}

// ═══════════════════════════════════════════════
// ヘルプパスでDocumentを作成
// ═══════════════════════════════════════════════

TEST(HelpDocumentTest, FromMarkdownWithHelpPath)
{
    auto doc = Document::FromMarkdown("# Help\ntext", HELP_PATH);
    EXPECT_FALSE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), HELP_PATH);
    EXPECT_TRUE(IsHelpPath(doc.GetFilePath()));
}

TEST(HelpDocumentTest, HelpDocumentHasToc)
{
    auto doc = Document::FromMarkdown("# Title\n## Section", HELP_PATH);
    EXPECT_GE(doc.GetToc().GetEntries().size(), 1u);
}

TEST(HelpDocumentTest, HelpPathIsNotMarkdownFile)
{
    EXPECT_FALSE(IsMarkdownFile(HELP_PATH));
}

// ═══════════════════════════════════════════════
// ナビゲーション履歴とヘルプパス
// ═══════════════════════════════════════════════

class HelpNavigationTest : public ::testing::Test {
protected:
    NavHistory history_;
};

TEST_F(HelpNavigationTest, GoBackFromHelpToFile)
{
    history_.Push(NavEntry{ L"C:\\file.md", 5, 10.0f });

    NavEntry out;
    ASSERT_TRUE(history_.GoBack(NavEntry{ HELP_PATH, 0, 0.0f }, out));
    EXPECT_EQ(out.file_path, L"C:\\file.md");
    EXPECT_EQ(out.node, 5);
    EXPECT_FLOAT_EQ(out.offset, 10.0f);
}

TEST_F(HelpNavigationTest, GoForwardFromFileToHelp)
{
    history_.Push(NavEntry{ L"C:\\file.md", 5, 10.0f });

    NavEntry back_out;
    history_.GoBack(NavEntry{ HELP_PATH, 0, 0.0f }, back_out);

    NavEntry fwd_out;
    ASSERT_TRUE(history_.GoForward(NavEntry{ L"C:\\file.md", 5, 10.0f }, fwd_out));
    EXPECT_EQ(fwd_out.file_path, HELP_PATH);
    EXPECT_EQ(fwd_out.node, 0);
    EXPECT_FLOAT_EQ(fwd_out.offset, 0.0f);
}

TEST_F(HelpNavigationTest, PushHelpThenGoBack)
{
    history_.Push(NavEntry{ HELP_PATH, 0, 0.0f });

    NavEntry out;
    ASSERT_TRUE(history_.GoBack(NavEntry{ L"C:\\file.md", 3, 40.0f }, out));
    EXPECT_EQ(out.file_path, HELP_PATH);
}

TEST_F(HelpNavigationTest, HelpPathInHistoryCanGoBack)
{
    history_.Push(NavEntry{ HELP_PATH, 0, 0.0f });
    EXPECT_TRUE(history_.CanGoBack());
}

// ═══════════════════════════════════════════════
// BuildTitleString でヘルプパスを扱う
// ═══════════════════════════════════════════════

TEST(HelpDocumentTest, BuildTitleStringWithHelpPath)
{
    auto title = BuildTitleString(HELP_PATH);
    // ヘルプパスでもクラッシュせずタイトルが生成される
    EXPECT_FALSE(title.empty());
    EXPECT_NE(title.find(L"mendo"), std::pmr::wstring::npos);
}
