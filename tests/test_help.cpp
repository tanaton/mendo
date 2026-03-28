#include <gtest/gtest.h>
#include "document.h"
#include "document_utils.h"
#include "navigation_service.h"

// ═══════════════════════════════════════════════
// IsHelpPath
// ═══════════════════════════════════════════════

TEST(HelpPathTest, MatchesHelpPath) {
    EXPECT_TRUE(IsHelpPath(L"mendo://help"));
}

TEST(HelpPathTest, RejectsEmptyPath) {
    EXPECT_FALSE(IsHelpPath(L""));
}

TEST(HelpPathTest, RejectsNormalFilePath) {
    EXPECT_FALSE(IsHelpPath(L"C:\\docs\\readme.md"));
}

TEST(HelpPathTest, RejectsSimilarPath) {
    EXPECT_FALSE(IsHelpPath(L"mendo://help/extra"));
    EXPECT_FALSE(IsHelpPath(L"mendo://hel"));
    EXPECT_FALSE(IsHelpPath(L"MENDO://HELP"));
}

TEST(HelpPathTest, ConstantValueIsCorrect) {
    EXPECT_EQ(HELP_PATH, L"mendo://help");
}

// ═══════════════════════════════════════════════
// ヘルプパスでDocumentを作成
// ═══════════════════════════════════════════════

TEST(HelpDocumentTest, FromMarkdownWithHelpPath) {
    auto doc = Document::FromMarkdown("# Help\ntext", HELP_PATH);
    EXPECT_FALSE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), HELP_PATH);
    EXPECT_TRUE(IsHelpPath(doc.GetFilePath()));
}

TEST(HelpDocumentTest, HelpDocumentHasToc) {
    auto doc = Document::FromMarkdown("# Title\n## Section", HELP_PATH);
    EXPECT_GE(doc.GetToc().GetEntries().size(), 1u);
}

TEST(HelpDocumentTest, HelpPathIsNotMarkdownFile) {
    EXPECT_FALSE(IsMarkdownFile(HELP_PATH));
}

// ═══════════════════════════════════════════════
// ナビゲーション履歴とヘルプパス
// ═══════════════════════════════════════════════

class HelpNavigationTest : public ::testing::Test {
protected:
    NavHistory history_;
    NavigationService service_{history_};
};

TEST_F(HelpNavigationTest, GoBackFromHelpToFile) {
    service_.PushHistory(L"C:\\file.md", 50.0f);

    auto result = service_.GoBack(HELP_PATH, 0.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(result.target, L"C:\\file.md");
    EXPECT_FLOAT_EQ(result.scroll_y, 50.0f);
}

TEST_F(HelpNavigationTest, GoForwardFromFileToHelp) {
    service_.PushHistory(L"C:\\file.md", 50.0f);

    service_.GoBack(HELP_PATH, 0.0f);

    auto result = service_.GoForward(L"C:\\file.md", 50.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(std::wstring_view(result.target), HELP_PATH);
    EXPECT_FLOAT_EQ(result.scroll_y, 0.0f);
}

TEST_F(HelpNavigationTest, PushHelpThenGoBack) {
    service_.PushHistory(HELP_PATH, 0.0f);

    auto result = service_.GoBack(L"C:\\file.md", 100.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(std::wstring_view(result.target), HELP_PATH);
}

TEST_F(HelpNavigationTest, HelpPathInHistoryCanGoBack) {
    service_.PushHistory(HELP_PATH, 0.0f);
    EXPECT_TRUE(service_.CanGoBack());
}

// ═══════════════════════════════════════════════
// BuildTitleString でヘルプパスを扱う
// ═══════════════════════════════════════════════

TEST(HelpDocumentTest, BuildTitleStringWithHelpPath) {
    auto title = BuildTitleString(HELP_PATH);
    // ヘルプパスでもクラッシュせずタイトルが生成される
    EXPECT_FALSE(title.empty());
    EXPECT_NE(title.find(L"mendo"), std::pmr::wstring::npos);
}
