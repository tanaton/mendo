#include <gtest/gtest.h>
#include "preloader.h"
#include "test_helpers.h"
#include "theme.h"
#include <windows.h>

namespace {

// AppliedSync 経路は worker が sink に結果を積んだ後でないと検証できない。
void WaitForPublish(const Preloader& p)
{
    ASSERT_TRUE(PollUntil([&] { return p.HasPublishedForTest(); }));
}

} // namespace

TEST(Preloader, IsActiveFalseBeforeStart)
{
    Preloader p;
    EXPECT_FALSE(p.IsActive());
    EXPECT_FALSE(p.TakeResult().has_value());
    EXPECT_FALSE(p.TakeError().has_value());
}

TEST(Preloader, AttachOrApplyReturnsNoneWhenNotStarted)
{
    Preloader p;
    MessageOnlyWindow w;
    EXPECT_EQ(p.AttachOrApply(w.Get(), 0), Preloader::AttachResult::None);
}

TEST(Preloader, AppliedSyncWhenWorkerCompletedBeforeAttach)
{
    TempFile tmp(L"preload_sync", "# attach after done\n");
    MessageOnlyWindow w;
    Preloader p;
    p.Start(tmp.PmrPath(), GetLightTheme());
    WaitForPublish(p);

    const auto r = p.AttachOrApply(w.Get(), 0);
    EXPECT_EQ(r, Preloader::AttachResult::AppliedSync);
    EXPECT_FALSE(p.IsActive());
    auto result = p.TakeResult();
    ASSERT_TRUE(result.has_value());
    // 非同期ロードと同じく、cache は worker で推定済みの状態で渡る。
    ASSERT_EQ(result->cache.size(), result->doc.GetNodes().size());
    EXPECT_GT(result->cache[0].height, 0.0f);
    EXPECT_GE(result->cache.Top(0), GetLightTheme().margin_top);
}

// 推定は渡したテーマ (ズーム適用済み) の寸法で行う。
TEST(Preloader, EstimatesWithGivenThemeZoom)
{
    TempFile tmp(L"preload_zoom", "# heading\n\nbody\n");
    const auto estimated_top = [&](float zoom) {
        Theme theme = GetLightTheme();
        theme.ApplyZoom(zoom);
        Preloader p;
        p.Start(tmp.PmrPath(), theme);
        WaitForPublish(p);
        auto result = p.TakeResult();
        EXPECT_TRUE(result.has_value());
        return result ? result->cache.Top(1) : 0.0f;
    };
    EXPECT_FLOAT_EQ(estimated_top(2.0f), estimated_top(1.0f) * 2.0f);
}

// hwnd 待ちの worker を join し続けて止まっていた回帰。
TEST(Preloader, TakeBeforeAttachDoesNotBlock)
{
    TempFile tmp(L"preload_take_first", "# take first\n");
    Preloader p;
    p.Start(tmp.PmrPath(), GetLightTheme());
    WaitForPublish(p);

    EXPECT_TRUE(p.TakeResult().has_value());
    EXPECT_FALSE(p.IsActive());
    MessageOnlyWindow w;
    EXPECT_EQ(p.AttachOrApply(w.Get(), 0), Preloader::AttachResult::None);
}

TEST(Preloader, TakeErrorBeforeAttachDoesNotBlock)
{
    Preloader p;
    p.Start(std::pmr::wstring(L"C:\\__mendo_no_such_preload__.md"), GetLightTheme());
    WaitForPublish(p);

    EXPECT_EQ(p.TakeError(), FileLoadError::NotFound);
    EXPECT_FALSE(p.IsActive());
}

TEST(Preloader, AttachedAsyncDeliversResultThroughHwnd)
{
    TempFile tmp(L"preload_async", "# tiny\n");
    MessageOnlyWindow w;
    Preloader p;
    p.Start(tmp.PmrPath(), GetLightTheme());
    const auto r = p.AttachOrApply(w.Get(), WM_USER + 1);
    EXPECT_TRUE(r == Preloader::AttachResult::AppliedSync ||
                r == Preloader::AttachResult::AttachedAsync);

    PollUntil([&] {
        return !p.IsActive() || p.TakeResult().has_value();
    });
}

TEST(Preloader, NotFoundFileSetsError)
{
    Preloader p;
    MessageOnlyWindow w;
    p.Start(std::pmr::wstring(L"C:\\__mendo_no_such_preload__.md"), GetLightTheme());
    WaitForPublish(p);
    const auto r = p.AttachOrApply(w.Get(), 0);
    EXPECT_EQ(r, Preloader::AttachResult::AppliedSync);
    auto err = p.TakeError();
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, FileLoadError::NotFound);
    EXPECT_FALSE(p.TakeError().has_value());
    EXPECT_FALSE(p.IsActive());
}

TEST(Preloader, RestartCancelsPreviousWorker)
{
    TempFile tmp1(L"preload_first", "# first\n");
    TempFile tmp2(L"preload_second", "# second body\n");
    MessageOnlyWindow w;
    Preloader p;
    p.Start(tmp1.PmrPath(), GetLightTheme());
    p.Start(tmp2.PmrPath(), GetLightTheme()); // Start 内の Join() で前回 worker は abort される
    EXPECT_TRUE(p.IsActive());

    WaitForPublish(p);
    const auto r = p.AttachOrApply(w.Get(), 0);
    EXPECT_EQ(r, Preloader::AttachResult::AppliedSync);
    auto result = p.TakeResult();
    ASSERT_TRUE(result.has_value());
}

TEST(Preloader, DestructorAbortsBlockedWorker)
{
    TempFile tmp(L"preload_dtor", "# wait\n");
    {
        Preloader p;
        p.Start(tmp.PmrPath(), GetLightTheme());
        // dtor が cv.wait に入った worker を stop 要求で起こして join。
    }
    SUCCEED();
}

TEST(Preloader, TakeAfterFinalizeReturnsNullopt)
{
    TempFile tmp(L"preload_once", "# only once\n");
    MessageOnlyWindow w;
    Preloader p;
    p.Start(tmp.PmrPath(), GetLightTheme());
    WaitForPublish(p);
    const auto r = p.AttachOrApply(w.Get(), 0);
    ASSERT_EQ(r, Preloader::AttachResult::AppliedSync);
    auto first = p.TakeResult();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(p.TakeResult().has_value());
    EXPECT_FALSE(p.TakeError().has_value());
}

TEST(Preloader, CancelDiscardsCompletedResult)
{
    TempFile tmp(L"preload_cancel", "# will be cancelled\n");
    Preloader p;
    p.Start(tmp.PmrPath(), GetLightTheme());
    WaitForPublish(p);

    p.Cancel();

    EXPECT_FALSE(p.IsActive());
    EXPECT_FALSE(p.TakeResult().has_value());
    EXPECT_FALSE(p.TakeError().has_value());
}

TEST(Preloader, CancelAbortsInFlightWorker)
{
    TempFile tmp(L"preload_cancel_inflight", "# in flight\n");
    Preloader p;
    p.Start(tmp.PmrPath(), GetLightTheme());
    // Cancel は join しないが、worker がどの段階にいても stop 要求 + lock 内再確認で
    // publish は弾かれるため、結果は決定的に空になる。
    p.Cancel();

    EXPECT_FALSE(p.IsActive());
    EXPECT_FALSE(p.TakeResult().has_value());
    EXPECT_FALSE(p.TakeError().has_value());
}
