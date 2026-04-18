#include <gtest/gtest.h>
#include "utility.h"
#include <memory_resource>
#include <variant>
#include <string>

// ═══════════════════════════════════════════════
// PmrFormat
// ═══════════════════════════════════════════════

TEST(Utility, PmrFormatPlainText)
{
    auto s = PmrFormat(L"hello");
    EXPECT_EQ(s, L"hello");
}

TEST(Utility, PmrFormatIntegerFormatting)
{
    auto s = PmrFormat(L"count={}", 42);
    EXPECT_EQ(s, L"count=42");
}

TEST(Utility, PmrFormatMultipleArgs)
{
    auto s = PmrFormat(L"{}+{}={}", 1, 2, 3);
    EXPECT_EQ(s, L"1+2=3");
}

TEST(Utility, PmrFormatWideStringArgument)
{
    std::wstring name = L"mendo";
    auto s = PmrFormat(L"hi {}!", name);
    EXPECT_EQ(s, L"hi mendo!");
}

TEST(Utility, PmrFormatReturnsPmrString)
{
    auto s = PmrFormat(L"x={}", 5);
    static_assert(std::is_same_v<decltype(s), std::pmr::wstring>,
        "PmrFormat must return std::pmr::wstring");
    EXPECT_EQ(s.size(), 3u); // "x=5"
}

TEST(Utility, PmrFormatEmptyFormatString)
{
    auto s = PmrFormat(L"");
    EXPECT_TRUE(s.empty());
}

TEST(Utility, PmrFormatFloatingPoint)
{
    auto s = PmrFormat(L"pi={:.2f}", 3.14159);
    EXPECT_EQ(s, L"pi=3.14");
}

// ═══════════════════════════════════════════════
// overloaded (std::visit 用ヘルパー)
// ═══════════════════════════════════════════════

TEST(Utility, OverloadedDispatchesByType)
{
    std::variant<int, double, std::wstring> v;

    v = 42;
    int tag = std::visit(overloaded{
        [](int)           { return 1; },
        [](double)        { return 2; },
        [](const std::wstring&) { return 3; },
    }, v);
    EXPECT_EQ(tag, 1);

    v = 1.5;
    tag = std::visit(overloaded{
        [](int)           { return 1; },
        [](double)        { return 2; },
        [](const std::wstring&) { return 3; },
    }, v);
    EXPECT_EQ(tag, 2);

    v = std::wstring(L"hi");
    tag = std::visit(overloaded{
        [](int)           { return 1; },
        [](double)        { return 2; },
        [](const std::wstring&) { return 3; },
    }, v);
    EXPECT_EQ(tag, 3);
}
