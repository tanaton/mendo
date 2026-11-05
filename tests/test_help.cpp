#include <gtest/gtest.h>
#include "document.h"
#include "document_utils.h"
#include "example_files.h"
#include "file_io.h"
#include "mock_text_measurer.h"

// ═══════════════════════════════════════════════
// IsHelpPath
// ═══════════════════════════════════════════════

// 完全一致だけをヘルプとみなす (大文字小文字も区別する)。
TEST(HelpPathTest, MatchesOnlyExactHelpPath)
{
    struct Case {
        std::wstring_view path;
        bool expected;
    };
    constexpr Case kCases[] = {
        { L"mendo://help", true },
        { L"", false },
        { L"C:\\docs\\readme.md", false },
        { L"mendo://help/extra", false },
        { L"mendo://hel", false },
        { L"MENDO://HELP", false },
        { L"mendo://other", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(IsHelpPath(c.path), c.expected);
    }
}

// ═══════════════════════════════════════════════
// ヘルプパスでDocumentを作成
// ═══════════════════════════════════════════════

// 埋め込みの文書はヘルプとサードパーティのライセンスだけ。ヘルプ扱い (言語の切り替えで読み直す) はヘルプだけ。
TEST(HelpPathTest, EmbeddedPathsAreHelpAndLicenses)
{
    EXPECT_TRUE(IsEmbeddedPath(HELP_PATH));
    EXPECT_TRUE(IsEmbeddedPath(LICENSES_PATH));
    EXPECT_FALSE(IsHelpPath(LICENSES_PATH));
    EXPECT_FALSE(IsEmbeddedPath(L"mendo://other"));
    EXPECT_FALSE(IsEmbeddedPath(L"C:\\docs\\THIRD_PARTY_LICENSES.md"));
}

TEST(HelpDocumentTest, HasBackingFileOnlyForRealPath)
{
    EXPECT_FALSE(Document::FromMarkdown("# Help", HELP_PATH).HasBackingFile());
    EXPECT_FALSE(Document::FromMarkdown("# Licenses", LICENSES_PATH).HasBackingFile());
    EXPECT_FALSE(Document::FromMarkdown("# Empty", L"").HasBackingFile());
    EXPECT_TRUE(Document::FromMarkdown("# File", L"C:/docs/readme.md").HasBackingFile());
}

// ═══════════════════════════════════════════════
// ヘルプを開いたときのレイアウト (App::ShowEmbeddedResource と同じ手順)
// ═══════════════════════════════════════════════

class HelpLayoutTest : public MockLayoutTestBase {};

// 実際の埋め込みヘルプで、別文書の表示中に開いても本文が計測されること。
TEST_F(HelpLayoutTest, OpeningHelpAfterAnotherDocumentMeasuresVisibleContent)
{
    constexpr float kWidth = 800.0f;
    constexpr float kViewportBottom = 600.0f;

    for (const auto name : { u8"help_ja.md", u8"help_en.md", u8"help_zh.md", u8"help_zh_hant.md", u8"help_ko.md" }) {
        SCOPED_TRACE(reinterpret_cast<const char*>(name));
        auto text = ReadFileBytes(std::filesystem::path(MENDO_RES_DIR) / name);
        ASSERT_FALSE(text.empty());

        auto prev = Document::FromMarkdown("# Sample\n\nhello", L"C:/docs/sample.md");
        LayoutCache cache(prev.GetNodes().size());
        ComputeLayout(engine_, prev.GetNodes(), cache, kWidth, 0.0f, kViewportBottom);

        auto help = Document::FromMarkdown(std::move(text), HELP_PATH);
        cache = layout::MakeEstimatedLayoutCache(help.GetNodes(), theme_);
        ComputeLayout(engine_, help.GetNodes(), cache, kWidth, 0.0f, kViewportBottom);

        const auto& nodes = help.GetNodes();
        ASSERT_GT(nodes.size(), 1u);
        EXPECT_GT(cache.Top(1), cache.Top(0));
        for (size_t i = 0; i < nodes.size() && cache.Top(i) < kViewportBottom; i++) {
            EXPECT_FALSE(cache[i].layout_dirty) << "ノード " << i;
            EXPECT_GT(cache[i].height, 0.0f) << "ノード " << i;
        }
    }
}

// 配布する zip には exe だけを入れるので、ライブラリと KaTeX の書体 (SIL OFL 1.1) のライセンス表記は、どの言語のヘルプからも
// 埋め込みの THIRD_PARTY_LICENSES.md へのリンクで開けるようにする。
TEST(HelpDocumentTest, EveryHelpLinksToLicenses)
{
    for (const auto name : { u8"help_ja.md", u8"help_en.md", u8"help_zh.md", u8"help_zh_hant.md", u8"help_ko.md" }) {
        SCOPED_TRACE(reinterpret_cast<const char*>(name));
        const auto text = ReadFileBytes(std::filesystem::path(MENDO_RES_DIR) / name);
        EXPECT_NE(text.find("](mendo://licenses)"), std::string::npos);
    }
    const auto licenses = ReadFileBytes(std::filesystem::path(MENDO_RES_DIR) / ".." / "THIRD_PARTY_LICENSES.md");
    EXPECT_NE(licenses.find("SIL OPEN FONT LICENSE Version 1.1"), std::string::npos);
    EXPECT_EQ(ClassifyLinkClick("mendo://licenses").type, LinkClickResult::Type::Embedded);
}
