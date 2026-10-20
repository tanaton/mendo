#include <gtest/gtest.h>
#include "file_io.h"
#include "test_helpers.h"
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

class FileIoTest : public TempDirTestBase {
protected:
    static std::string ReadAsString(const fs::path& path)
    {
        const auto buf = ReadAllBytes(path.c_str());
        return { reinterpret_cast<const char*>(buf.data()), buf.size() };
    }

    static bool Write(const fs::path& path, std::string_view payload)
    {
        return WriteAllBytes(path, payload.data(), payload.size());
    }

    // AtomicWriteAllBytes が使う一時ファイル。
    static fs::path TmpOf(fs::path path)
    {
        path += L".tmp";
        return path;
    }
};

TEST(IsLocalPathTest, RejectsNetworkAndDevicePaths)
{
    struct Case {
        std::wstring_view path;
        bool local;
    };
    constexpr Case kCases[] = {
        { L"", true },
        { L"a.png", true },
        { L"C:\\img\\a.png", true },
        { L"/img/a.png", true },
        { L"?\\a.png", true },
        { L"\\?a.png", true },

        { L"\\\\host\\share\\a.png", false },
        { L"//host/share/a.png", false },
        { L"\\/host/share/a.png", false },
        { L"\\\\?\\UNC\\host\\share\\a.png", false },
        { L"\\\\.\\pipe\\x", false },
        { L"\\??\\UNC\\host\\share\\a.png", false },
        { L"/??/UNC/host/share/a.png", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::PrintToString(std::wstring(c.path)));
        EXPECT_EQ(path_util::IsLocalPath(c.path), c.local);
    }
}

// ドライブと UNC のデバイスパスは Win32 形式に直す。直せないデバイスパスは相対パスの基準にしない。
TEST(ToWin32PathTest, ConvertsDriveAndUncDevicePaths)
{
    struct Case {
        std::wstring_view path;
        std::optional<std::wstring_view> expected;
    };
    constexpr Case kCases[] = {
        { L"C:\\dir", L"C:\\dir" },
        { L"\\\\host\\share", L"\\\\host\\share" },
        { L"\\\\?\\C:\\dir", L"C:\\dir" },
        { L"\\\\.\\d:", L"d:" },
        { L"\\??\\C:\\dir", L"C:\\dir" },
        { L"\\\\?\\unc\\host\\share\\dir", L"\\\\host\\share\\dir" },
        { L"\\\\.\\pipe\\x", std::nullopt },
        { L"\\\\?\\Volume{0}\\dir", std::nullopt },
        { L"\\\\?\\CC:\\dir", std::nullopt },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::PrintToString(std::wstring(c.path)));
        EXPECT_EQ(path_util::ToWin32Path(c.path), c.expected);
    }
}

// ═══════════════════════════════════════════════
// WriteAllBytes + ReadAllBytes ラウンドトリップ
// ═══════════════════════════════════════════════

TEST_F(FileIoTest, RoundTripAscii)
{
    const auto path = temp_dir_ / L"ascii.bin";
    ASSERT_TRUE(Write(path, "Hello, mendo!"));
    EXPECT_EQ(ReadAsString(path), "Hello, mendo!");
}

TEST_F(FileIoTest, RoundTripBinary)
{
    const auto path = temp_dir_ / L"binary.bin";
    std::vector<uint8_t> payload(256);
    std::ranges::iota(payload, uint8_t{ 0 });

    ASSERT_TRUE(WriteAllBytes(path, payload.data(), payload.size()));

    const auto buf = ReadAllBytes(path.c_str());
    ASSERT_EQ(buf.size(), payload.size());
    EXPECT_TRUE(std::ranges::equal(buf, payload));
}

TEST_F(FileIoTest, WriteOverwritesExistingFile)
{
    const auto path = temp_dir_ / L"overwrite.bin";
    ASSERT_TRUE(Write(path, "first-content-longer"));
    ASSERT_TRUE(Write(path, "short"));
    EXPECT_EQ(ReadAsString(path), "short");
}

TEST_F(FileIoTest, WriteZeroBytesProducesEmptyFile)
{
    const auto path = temp_dir_ / L"zero.bin";
    ASSERT_TRUE(WriteAllBytes(path, "", 0));

    EXPECT_TRUE(ReadAllBytes(path.c_str()).empty());
    EXPECT_TRUE(fs::exists(path));
    EXPECT_EQ(fs::file_size(path), 0u);
}

// ═══════════════════════════════════════════════
// エラーケース
// ═══════════════════════════════════════════════

TEST_F(FileIoTest, ReadMissingFileReturnsEmpty)
{
    const auto path = temp_dir_ / L"does_not_exist.bin";
    EXPECT_TRUE(ReadAllBytes(path.c_str()).empty());

    DWORD err = 0xDEADBEEF;
    EXPECT_TRUE(ReadAllBytes(path.c_str(), &err).empty());
    EXPECT_EQ(err, static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
}

// 開けたときの err はクリアされる契約。空ファイルは失敗と同じく空を返す。
TEST_F(FileIoTest, ErrorPointerResetOnSuccess)
{
    const auto empty = WriteTempFile(L"empty.bin");
    DWORD err = 0xDEADBEEF;
    EXPECT_TRUE(ReadAllBytes(empty.c_str(), &err).empty());
    EXPECT_EQ(err, 0u);

    const auto one = WriteTempFile(L"reset.bin", "x");
    err = 0xDEADBEEF;
    EXPECT_EQ(ReadAllBytes(one.c_str(), &err).size(), 1u);
    EXPECT_EQ(err, 0u);
}

TEST_F(FileIoTest, WriteToInvalidPathFails)
{
    // 存在しないディレクトリ配下のパス（親ディレクトリがない）
    EXPECT_FALSE(Write(temp_dir_ / L"no_such_dir" / L"file.bin", "data"));
}

// ═══════════════════════════════════════════════
// IsFileLargerThan: エディタの書き込み中検知
// ═══════════════════════════════════════════════

// 読んだ 1000 バイトより、既定の許容差 16 バイトを超えて大きいときだけ true。
TEST_F(FileIoTest, IsFileLargerThanBeyondTolerance)
{
    struct Case {
        size_t file_size;
        bool expected;
    };
    constexpr Case kCases[] = {
        { 1000, false },
        // BOM 3 バイト分のずれは partial-write ではなく BOM 由来として扱う。
        { 1003, false },
        { 1016, false },
        { 1017, true },
        // partial-read race の中核ケース: 読んだ後にファイルが倍に伸びている
        { 2000, true },
        // 削除直後の truncate 状態は partial-write race ではない (別パスの DeferPrefixShrink が処理する)
        { 500, false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.file_size);
        const auto path = WriteTempFile(L"size.bin", std::string(c.file_size, 'a'));
        EXPECT_EQ(IsFileLargerThan(path.c_str(), 1000), c.expected);
    }
}

TEST_F(FileIoTest, IsFileLargerThan_MissingFile_ReturnsFalse)
{
    const auto path = temp_dir_ / L"does_not_exist.bin";
    EXPECT_FALSE(IsFileLargerThan(path.c_str(), 1000));
}

TEST_F(FileIoTest, IsFileLargerThan_CustomTolerance)
{
    const auto path = WriteTempFile(L"custom_tol.bin", std::string(1100, 'f'));

    // tolerance=50: 100 バイト差は超過 → true
    EXPECT_TRUE(IsFileLargerThan(path.c_str(), 1000, 50));
    // tolerance=200: 100 バイト差は範囲内 → false
    EXPECT_FALSE(IsFileLargerThan(path.c_str(), 1000, 200));
}

// ═══════════════════════════════════════════════
// AtomicWriteAllBytes: tmp → MoveFileExW(REPLACE_EXISTING) 経路
// ═══════════════════════════════════════════════

TEST_F(FileIoTest, AtomicWrite_NewFile_WritesContentAndCleansUpTmp)
{
    const auto path = temp_dir_ / L"atomic_new.bin";
    const std::string_view payload = "atomic-new-content";

    ASSERT_TRUE(AtomicWriteAllBytes(path, payload.data(), payload.size()));

    EXPECT_EQ(ReadAsString(path), payload);
    // tmp は MoveFileExW で消費される (残らない)
    EXPECT_FALSE(fs::exists(TmpOf(path)));
}

TEST_F(FileIoTest, AtomicWrite_OverwritesExistingFile)
{
    const auto path = temp_dir_ / L"atomic_overwrite.bin";
    const std::string_view first = "first-content-longer-than-second";
    const std::string_view second = "short";

    ASSERT_TRUE(AtomicWriteAllBytes(path, first.data(), first.size()));
    ASSERT_TRUE(AtomicWriteAllBytes(path, second.data(), second.size()));

    EXPECT_EQ(ReadAsString(path), second);
    EXPECT_FALSE(fs::exists(TmpOf(path)));
}

TEST_F(FileIoTest, AtomicWrite_ToInvalidPath_FailsAndLeavesNoTmp)
{
    // 親ディレクトリが存在しないので tmp 書き込みも失敗する。
    // 直接書き込み fallback も到達しないので path も tmp も生成されない。
    const auto path = temp_dir_ / L"no_such_dir" / L"atomic.bin";
    const std::string_view payload = "data";

    EXPECT_FALSE(AtomicWriteAllBytes(path, payload.data(), payload.size()));
    EXPECT_FALSE(fs::exists(path));
    EXPECT_FALSE(fs::exists(TmpOf(path)));
}

TEST_F(FileIoTest, AtomicWrite_DoesNotLeavePreviousTmpBehind)
{
    // 既存の `<path>.tmp` がたとえ残っていても、新しい AtomicWriteAllBytes は
    // 上書きで作り直し、MoveFileExW で消費するため最終的に残らない。
    const auto path = temp_dir_ / L"atomic_stale_tmp.bin";
    const std::string_view payload = "fresh-payload";
    ASSERT_TRUE(Write(TmpOf(path), "stale-tmp-leftover"));

    ASSERT_TRUE(AtomicWriteAllBytes(path, payload.data(), payload.size()));

    EXPECT_EQ(ReadAsString(path), payload);
    EXPECT_FALSE(fs::exists(TmpOf(path)));
}

TEST_F(FileIoTest, AtomicWrite_ZeroBytesProducesEmptyFile)
{
    const auto path = temp_dir_ / L"atomic_zero.bin";

    ASSERT_TRUE(AtomicWriteAllBytes(path, "", 0));

    EXPECT_TRUE(fs::exists(path));
    EXPECT_EQ(fs::file_size(path), 0u);
    EXPECT_FALSE(fs::exists(TmpOf(path)));
}

// ---- path_util::TrimTrailingSeparators ----

TEST(TrimTrailingSeparatorsTest, RemovesTrailingSeparatorsButKeepsDriveRoot)
{
    struct Case {
        std::wstring_view path;
        std::wstring_view expected;
    };
    constexpr Case kCases[] = {
        { L"C:\\docs", L"C:\\docs" },
        { L"C:\\docs\\", L"C:\\docs" },
        { L"C:\\docs\\\\", L"C:\\docs" },
        { L"C:/docs/", L"C:/docs" },
        { L"C:\\docs/\\", L"C:\\docs" },
        // ルートの区切りは残す ("C:" はカレントディレクトリ相対の別の意味になる)
        { L"C:\\", L"C:\\" },
        { L"C:\\\\", L"C:\\" },
        { L"", L"" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(path_util::TrimTrailingSeparators(c.path), c.expected);
    }
}
