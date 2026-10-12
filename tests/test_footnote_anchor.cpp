#include <gtest/gtest.h>
#include "footnote_anchor.h"
#include <array>
#include <span>

namespace {

constexpr std::array<int32_t, 2> kDefs{ 10, 11 };
constexpr std::array<FootnoteRef, 3> kRefs{ {
    { 1, 1, 2 },
    { 2, 1, 4 },
    { 1, 2, 7 },
} };

int Find(std::string_view anchor)
{
    return FindFootnoteAnchor(anchor, kDefs, kRefs);
}

} // namespace

TEST(FootnoteAnchor, FormatsGitHubCompatibleUrls)
{
    EXPECT_EQ(FootnoteDefUrl(12), "#user-content-fn-12");
    EXPECT_EQ(FootnoteRefUrl(3, 1), "#user-content-fnref-3");
    EXPECT_EQ(FootnoteRefUrl(3, 2), "#user-content-fnref-3-2");
}

TEST(FootnoteAnchor, FindsDefinition)
{
    EXPECT_EQ(Find("user-content-fn-1"), 10);
    EXPECT_EQ(Find("user-content-fn-2"), 11);
    EXPECT_EQ(Find("user-content-fn-3"), -1);
    EXPECT_EQ(Find("user-content-fn-0"), -1);
}

TEST(FootnoteAnchor, FindsReference)
{
    EXPECT_EQ(Find("user-content-fnref-1"), 2);
    EXPECT_EQ(Find("user-content-fnref-2"), 4);
    EXPECT_EQ(Find("user-content-fnref-1-2"), 7);
    EXPECT_EQ(Find("user-content-fnref-1-3"), -1);
}

TEST(FootnoteAnchor, RejectsMalformedAnchors)
{
    for (const char* anchor : { "", "fn-1", "user-content-fn-", "user-content-fn-1x", "user-content-fn--1", "user-content-fnref-",
                                "user-content-fnref-1-", "user-content-fnref-a", "user-content-fn-99999999999" }) {
        EXPECT_EQ(Find(anchor), -1) << anchor;
    }
}
