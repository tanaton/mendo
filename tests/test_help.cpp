#include <gtest/gtest.h>
#include "document.h"
#include "document_utils.h"
#include "example_files.h"
#include "mock_text_measurer.h"
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
    EXPECT_FALSE(IsHelpPath(L"mendo://other"));
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

TEST(HelpDocumentTest, HasBackingFileOnlyForRealPath)
{
    EXPECT_FALSE(Document::FromMarkdown("# Help", HELP_PATH).HasBackingFile());
    EXPECT_FALSE(Document::FromMarkdown("# Empty", L"").HasBackingFile());
    EXPECT_TRUE(Document::FromMarkdown("# File", L"C:/docs/readme.md").HasBackingFile());
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
// ヘルプを開いたときのレイアウト (App::LoadHelpDocument と同じ手順)
// ═══════════════════════════════════════════════

class HelpLayoutTest : public MockLayoutTestBase {};

// 実際の埋め込みヘルプで、別文書の表示中に開いても本文が計測されること。
TEST_F(HelpLayoutTest, OpeningHelpAfterAnotherDocumentMeasuresVisibleContent)
{
    constexpr float kWidth = 800.0f;
    constexpr float kViewportBottom = 600.0f;

    for (const auto name : { u8"help_ja.md", u8"help_en.md" }) {
        SCOPED_TRACE(reinterpret_cast<const char*>(name));
        auto text = ReadFileBytes(std::filesystem::path(MENDO_RES_DIR) / name);
        ASSERT_FALSE(text.empty());

        auto prev = Document::FromMarkdown("# Sample\n\nhello", L"C:/docs/sample.md");
        LayoutCache cache;
        cache.Reset(prev.GetNodes().size());
        engine_.ComputeLayout(prev.GetNodesMut(), cache, kWidth, 0.0f, kViewportBottom);

        auto help = Document::FromMarkdown(std::move(text), HELP_PATH);
        cache = mendo::layout::MakeEstimatedLayoutCache(help.GetNodes(), theme_);
        engine_.ComputeLayout(help.GetNodesMut(), cache, kWidth, 0.0f, kViewportBottom);

        const auto& nodes = help.GetNodes();
        ASSERT_GT(nodes.size(), 1u);
        EXPECT_GT(cache.Top(1), cache.Top(0));
        for (size_t i = 0; i < nodes.size() && cache.Top(i) < kViewportBottom; i++) {
            EXPECT_FALSE(cache[i].layout_dirty) << "ノード " << i;
            EXPECT_GT(cache[i].height, 0.0f) << "ノード " << i;
        }
    }
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
