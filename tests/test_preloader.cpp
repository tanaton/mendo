#include <gtest/gtest.h>
#include "preloader.h"
#include "test_helpers.h"
#include <windows.h>

namespace {

// worker の cv.wait predicate `hwnd != nullptr` を満たす最小の HWND 提供。
// HWND_MESSAGE 親で可視化されず、メッセージループ不要。
class MessageOnlyWindow {
public:
    MessageOnlyWindow()
    {
        const wchar_t* class_name = L"MendoPreloaderTestMsgWnd";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = class_name;
        ::RegisterClassExW(&wc);
        hwnd_ = ::CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }
    ~MessageOnlyWindow()
    {
        if (hwnd_) {
            ::DestroyWindow(hwnd_);
        }
    }
    MessageOnlyWindow(const MessageOnlyWindow&) = delete;
    MessageOnlyWindow& operator=(const MessageOnlyWindow&) = delete;

    HWND Get() const noexcept { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
};

// worker が「ファイル read + Document 構築 + sink emplace + cv.wait 入り」まで
// 到達するのに 100ms 程度を見込む保守値。これより短いと AppliedSync 経路が
// race で AttachedAsync に転ぶ可能性がある。
constexpr auto kWorkerCompleteWait = std::chrono::milliseconds(150);

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
    p.Start(tmp.PmrPath());
    std::this_thread::sleep_for(kWorkerCompleteWait);

    const auto r = p.AttachOrApply(w.Get(), 0);
    EXPECT_EQ(r, Preloader::AttachResult::AppliedSync);
    EXPECT_FALSE(p.IsActive());
    auto result = p.TakeResult();
    ASSERT_TRUE(result.has_value());
    // preloader は Theme 不在で EstimateNodeHeights を skip する規約。
    EXPECT_FALSE(result->heights_estimated);
}

TEST(Preloader, AttachedAsyncDeliversResultThroughHwnd)
{
    TempFile tmp(L"preload_async", "# tiny\n");
    MessageOnlyWindow w;
    Preloader p;
    p.Start(tmp.PmrPath());
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
    p.Start(std::pmr::wstring(L"C:\\__mendo_no_such_preload__.md"));
    std::this_thread::sleep_for(kWorkerCompleteWait);
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
    p.Start(tmp1.PmrPath());
    p.Start(tmp2.PmrPath()); // Start 内の Join() で前回 worker は abort される
    EXPECT_TRUE(p.IsActive());

    std::this_thread::sleep_for(kWorkerCompleteWait);
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
        p.Start(tmp.PmrPath());
        // dtor が cv.wait に入った worker を SignalAbort で起こして join。
    }
    SUCCEED();
}

TEST(Preloader, TakeAfterFinalizeReturnsNullopt)
{
    TempFile tmp(L"preload_once", "# only once\n");
    MessageOnlyWindow w;
    Preloader p;
    p.Start(tmp.PmrPath());
    std::this_thread::sleep_for(kWorkerCompleteWait);
    const auto r = p.AttachOrApply(w.Get(), 0);
    ASSERT_EQ(r, Preloader::AttachResult::AppliedSync);
    auto first = p.TakeResult();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(p.TakeResult().has_value());
    EXPECT_FALSE(p.TakeError().has_value());
}
