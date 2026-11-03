#include <gtest/gtest.h>
#include "win32_util.h"
#include <shellapi.h>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

namespace {

// exe の後ろに付けた引数が、CommandLineToArgvW で 1 つの引数としてそのまま戻るか。
std::wstring RoundTrip(std::wstring_view arg)
{
    std::wstring cmd = L"mendo.exe " + QuoteCommandLineArg(arg);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmd.c_str(), &argc);
    std::wstring result = argc == 2 ? argv[1] : L"<argc mismatch>";
    LocalFree(argv);
    return result;
}

constexpr auto Tie(const RECT& r) noexcept
{
    return std::tie(r.left, r.top, r.right, r.bottom);
}

} // namespace

TEST(Win32UtilTest, QuoteCommandLineArgRoundTrips)
{
    constexpr std::wstring_view kCases[] = {
        L"C:\\docs\\a.md",
        L"C:\\my docs\\a b.md",
        L"C:\\",
        L"\\\\server\\share\\",
        L"C:\\dir\\\\",
    };
    for (const auto arg : kCases) {
        SCOPED_TRACE(std::wstring{ arg });
        EXPECT_EQ(RoundTrip(arg), arg);
    }
}

TEST(Win32UtilTest, CascadeRect)
{
    constexpr RECT kWork{ 0, 0, 1000, 800 };
    struct Case {
        const char* name;
        RECT work;
        RECT rect;
        RECT expected;
    };
    constexpr Case kCases[] = {
        { "Shifted", kWork, { 100, 100, 600, 500 }, { 132, 132, 632, 532 } },
        { "FitsExactly", kWork, { 468, 368, 968, 768 }, { 500, 400, 1000, 800 } },
        { "OverflowRightWrapsToTopLeft", kWork, { 469, 100, 969, 500 }, { 0, 0, 500, 400 } },
        { "OverflowBottomWrapsToTopLeft", kWork, { 100, 369, 600, 769 }, { 0, 0, 500, 400 } },
        { "LargerThanWork", kWork, { 0, 0, 1000, 800 }, { 0, 0, 1000, 800 } },
        // 左の画面は座標が負になる。
        { "NegativeCoordinates", { -1920, 0, 0, 1040 }, { -1800, 50, -1000, 650 }, { -1768, 82, -968, 682 } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(Tie(CascadeRect(c.rect, c.work, 32)), Tie(c.expected));
    }
}

// 同じウィンドウから続けて開いても、開いているウィンドウと左上が重ならない所まで進める。
TEST(Win32UtilTest, CascadeRectSkipsOccupiedOrigins)
{
    constexpr RECT kWork{ 0, 0, 1000, 800 };
    constexpr RECT kRect{ 100, 100, 600, 500 };
    constexpr POINT kOne[] = { { 100, 100 }, { 132, 132 } };
    constexpr POINT kTwo[] = { { 164, 164 }, { 100, 100 }, { 132, 132 } };
    constexpr POINT kUnrelated[] = { { 100, 100 }, { 133, 132 } };
    struct Case {
        const char* name;
        RECT rect;
        std::span<const POINT> occupied;
        RECT expected;
    };
    const Case kCases[] = {
        { "SkipOne", kRect, kOne, { 164, 164, 664, 564 } },
        { "SkipChainInAnyOrder", kRect, kTwo, { 196, 196, 696, 596 } },
        { "OnlyExactOriginCounts", kRect, kUnrelated, { 132, 132, 632, 532 } },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(Tie(CascadeRect(c.rect, kWork, 32, c.occupied)), Tie(c.expected));
    }

    // 作業領域いっぱいの窓は左上へ戻るたびに同じ所に来る。空きが無くても抜ける。
    constexpr POINT kFull[] = { { 0, 0 } };
    EXPECT_EQ(Tie(CascadeRect(kWork, kWork, 32, kFull)), Tie(kWork));
}
