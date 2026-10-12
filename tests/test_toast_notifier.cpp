#include <gtest/gtest.h>
#include "toast_notifier.h"

TEST(ToastNotifier, InitiallyHidden)
{
    ToastNotifier toast;
    EXPECT_FALSE(toast.IsVisible());
    EXPECT_TRUE(toast.GetMessage().empty());
}

TEST(ToastNotifier, ShowMakesVisible)
{
    ToastNotifier toast;
    toast.Show(L"テスト");
    EXPECT_TRUE(toast.IsVisible());
    EXPECT_EQ(toast.GetMessage(), L"テスト");
}

TEST(ToastNotifier, ShowOverwritesPrevious)
{
    ToastNotifier toast;
    toast.Show(L"first");
    toast.Show(L"second");
    EXPECT_EQ(toast.GetMessage(), L"second");
}

TEST(ToastNotifier, HideClearsMessage)
{
    ToastNotifier toast;
    toast.Show(L"test");
    toast.Hide();
    EXPECT_FALSE(toast.IsVisible());
    EXPECT_TRUE(toast.GetMessage().empty());
}
