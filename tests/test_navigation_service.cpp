#include <gtest/gtest.h>
#include "navigation_service.h"

class NavigationServiceTest : public ::testing::Test {
protected:
    NavHistory history_;
    NavigationService service_{ history_ };
};

TEST_F(NavigationServiceTest, HandleAnchorLink)
{
    auto result = service_.HandleLinkClick(L"#section-1", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_EQ(result.target, L"section-1");
}

TEST_F(NavigationServiceTest, HandleExternalLink)
{
    auto result = service_.HandleLinkClick(L"https://example.com", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
    EXPECT_EQ(result.target, L"https://example.com");
}

TEST_F(NavigationServiceTest, HandleHttpLink)
{
    auto result = service_.HandleLinkClick(L"http://example.com", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
}

TEST_F(NavigationServiceTest, HandleMailtoLink)
{
    auto result = service_.HandleLinkClick(L"mailto:user@example.com", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
}

TEST_F(NavigationServiceTest, BlockFileScheme)
{
    auto result = service_.HandleLinkClick(L"file:///C:/Windows/System32/cmd.exe", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, BlockJavascriptScheme)
{
    auto result = service_.HandleLinkClick(L"javascript:alert(1)", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, BlockUnknownScheme)
{
    auto result = service_.HandleLinkClick(L"ftp://example.com/file", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, BlockBareRelativePath)
{
    auto result = service_.HandleLinkClick(L"other.md", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, HttpsCaseInsensitive)
{
    auto result = service_.HandleLinkClick(L"HTTPS://EXAMPLE.COM", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::ExternalUrl);
}

TEST_F(NavigationServiceTest, HandleEmptyLink)
{
    auto result = service_.HandleLinkClick(L"", L"C:\\file.md");
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, GoBackNoHistory)
{
    auto result = service_.GoBack(L"C:\\file.md", 100.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, GoBackSameFile)
{
    service_.PushHistory(L"C:\\file.md", 50.0f);

    auto result = service_.GoBack(L"C:\\file.md", 200.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_FLOAT_EQ(result.scroll_y, 50.0f);
}

TEST_F(NavigationServiceTest, GoBackDifferentFile)
{
    service_.PushHistory(L"C:\\first.md", 50.0f);

    auto result = service_.GoBack(L"C:\\second.md", 200.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(result.target, L"C:\\first.md");
    EXPECT_FLOAT_EQ(result.scroll_y, 50.0f);
}

TEST_F(NavigationServiceTest, GoForwardNoHistory)
{
    auto result = service_.GoForward(L"C:\\file.md", 100.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::None);
}

TEST_F(NavigationServiceTest, GoBackThenForward)
{
    service_.PushHistory(L"C:\\first.md", 10.0f);

    // 戻る
    auto back_result = service_.GoBack(L"C:\\first.md", 100.0f);
    EXPECT_EQ(back_result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_FLOAT_EQ(back_result.scroll_y, 10.0f);

    // 進む
    auto fwd_result = service_.GoForward(L"C:\\first.md", 10.0f);
    EXPECT_EQ(fwd_result.type, NavigationService::NavigateResult::Type::Anchor);
    EXPECT_FLOAT_EQ(fwd_result.scroll_y, 100.0f);
}

TEST_F(NavigationServiceTest, CanGoBackForward)
{
    EXPECT_FALSE(service_.CanGoBack());
    EXPECT_FALSE(service_.CanGoForward());

    service_.PushHistory(L"C:\\file.md", 0.0f);
    EXPECT_TRUE(service_.CanGoBack());
    EXPECT_FALSE(service_.CanGoForward());
}

// ═══════════════════════════════════════════════
// 異なるファイル間でのスクロール位置保持
// （戻る/進むでファイルロードが発生するケース）
// ═══════════════════════════════════════════════

TEST_F(NavigationServiceTest, GoForwardDifferentFileReturnsLoadFile)
{
    service_.PushHistory(L"C:\\first.md", 50.0f);

    // first.md → second.md へ移動した後、戻る
    auto back = service_.GoBack(L"C:\\second.md", 200.0f);
    ASSERT_EQ(back.type, NavigationService::NavigateResult::Type::LoadFile);

    // first.md から second.md へ進む
    auto fwd = service_.GoForward(L"C:\\first.md", 50.0f);
    EXPECT_EQ(fwd.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(fwd.target, L"C:\\second.md");
    EXPECT_FLOAT_EQ(fwd.scroll_y, 200.0f);
}

TEST_F(NavigationServiceTest, BackForwardCyclePreservesScrollPositions)
{
    // A(scroll=100) → B(scroll=300) → C(scroll=500)
    service_.PushHistory(L"C:\\a.md", 100.0f);
    service_.PushHistory(L"C:\\b.md", 300.0f);

    // C から B へ戻る → LoadFile + scroll_y=300
    auto r1 = service_.GoBack(L"C:\\c.md", 500.0f);
    EXPECT_EQ(r1.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(r1.target, L"C:\\b.md");
    EXPECT_FLOAT_EQ(r1.scroll_y, 300.0f);

    // B から A へ戻る → LoadFile + scroll_y=100
    auto r2 = service_.GoBack(L"C:\\b.md", 300.0f);
    EXPECT_EQ(r2.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(r2.target, L"C:\\a.md");
    EXPECT_FLOAT_EQ(r2.scroll_y, 100.0f);

    // A から B へ進む → LoadFile + scroll_y=300
    auto r3 = service_.GoForward(L"C:\\a.md", 100.0f);
    EXPECT_EQ(r3.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(r3.target, L"C:\\b.md");
    EXPECT_FLOAT_EQ(r3.scroll_y, 300.0f);

    // B から C へ進む → LoadFile + scroll_y=500
    auto r4 = service_.GoForward(L"C:\\b.md", 300.0f);
    EXPECT_EQ(r4.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(r4.target, L"C:\\c.md");
    EXPECT_FLOAT_EQ(r4.scroll_y, 500.0f);
}

TEST_F(NavigationServiceTest, BackFromDifferentFilePreservesCurrentScroll)
{
    // ファイルAで scroll_y=150 の状態を記録してBへ遷移
    service_.PushHistory(L"C:\\a.md", 150.0f);

    // B(scroll_y=400) から戻る → Aのファイルロード
    auto back = service_.GoBack(L"C:\\b.md", 400.0f);
    EXPECT_EQ(back.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_FLOAT_EQ(back.scroll_y, 150.0f);

    // 進む → B(scroll_y=400) へ戻る
    auto fwd = service_.GoForward(L"C:\\a.md", 150.0f);
    EXPECT_EQ(fwd.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_EQ(fwd.target, L"C:\\b.md");
    EXPECT_FLOAT_EQ(fwd.scroll_y, 400.0f);
}

TEST_F(NavigationServiceTest, LoadFileResultAlwaysHasScrollY)
{
    // scroll_y=0 でもLoadFile結果に含まれることを確認
    service_.PushHistory(L"C:\\a.md", 0.0f);
    auto result = service_.GoBack(L"C:\\b.md", 0.0f);
    EXPECT_EQ(result.type, NavigationService::NavigateResult::Type::LoadFile);
    EXPECT_FLOAT_EQ(result.scroll_y, 0.0f);
}
