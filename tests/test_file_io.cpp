#include <gtest/gtest.h>
#include "file_io.h"
#include "i18n.h"
#include "test_helpers.h"
#include <algorithm>
#include <filesystem>
#include <format>
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

// 共有のルートの親 (\\server) は列挙できないので、ファイル一覧はそこへ上がらない。
TEST(IsUncShareRootTest, DetectsOnlyServerAndShare)
{
    struct Case {
        std::wstring_view path;
        bool root;
    };
    constexpr Case kCases[] = {
        { L"\\\\srv\\share", true },
        { L"\\\\srv\\share\\", true },
        { L"//srv/share", true },
        { L"\\\\srv\\share\\sub", false },
        { L"\\\\srv", false },
        { L"\\\\srv\\", false },
        { L"\\\\?\\C:\\a", false },
        { L"\\\\.\\pipe", false },
        { L"C:\\", false },
        { L"C:\\dir", false },
        { L"", false },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ c.path }));
        EXPECT_EQ(path_util::IsUncShareRoot(c.path), c.root);
    }
}

// ドライブ文字の無いパスは見ない。ローカルのドライブは割り当てたネットワークドライブではない。
TEST(IsOnNetworkDriveTest, LocalAndNonDrivePathsAreNotNetwork)
{
    wchar_t windows_dir[MAX_PATH]{};
    ASSERT_GT(GetWindowsDirectoryW(windows_dir, MAX_PATH), 0u);
    EXPECT_FALSE(path_util::IsOnNetworkDrive(windows_dir));
    EXPECT_FALSE(path_util::IsOnNetworkDrive(L"relative\\a.md"));
    EXPECT_FALSE(path_util::IsOnNetworkDrive(L"\\\\srv\\share\\a.md"));
    EXPECT_FALSE(path_util::IsOnNetworkDrive(L""));
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

// ═══════════════════════════════════════════════
// AtomicWriteAllBytes: tmp → MoveFileExW(REPLACE_EXISTING) 経路
// ═══════════════════════════════════════════════

// 新しく作るときも上書きするとき (0 バイトも) も中身を書き、tmp は MoveFileExW で消費されて残らない。
TEST_F(FileIoTest, AtomicWrite_CreatesAndOverwritesFile)
{
    const auto path = temp_dir_ / L"atomic.bin";
    for (const std::string_view payload : { "first-content-longer-than-second", "short", "" }) {
        SCOPED_TRACE(payload);
        ASSERT_TRUE(AtomicWriteAllBytes(path, payload.data(), payload.size()));
        EXPECT_TRUE(fs::exists(path));
        EXPECT_EQ(ReadAsString(path), payload);
        EXPECT_FALSE(fs::exists(TmpOf(path)));
    }
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

// 共有を許さずに開かれているファイルは、無いファイルと分けて Locked にする (読み直しで待つ)。
TEST_F(FileIoTest, OpenFileForReadShared_ReportsLockedWhileOpenedExclusively)
{
    const auto path = temp_dir_ / L"locked.md";
    ASSERT_TRUE(Write(path, "body"));
    {
        UniqueHandle exclusive(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        ASSERT_TRUE(exclusive);
        const auto r = OpenFileForReadShared(path.c_str(), path_util::kFileShareRWDelete, 1024);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error(), FileLoadError::Locked);
    }
    EXPECT_TRUE(OpenFileForReadShared(path.c_str(), path_util::kFileShareRWDelete, 1024));
    const auto missing = OpenFileForReadShared((temp_dir_ / L"missing.md").c_str(), path_util::kFileShareRWDelete, 1024);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), FileLoadError::NotFound);
}

// 利用者が選んだ保存先では、同じフォルダにある `<path>.tmp` や一時ファイルと同じ名前のファイルに触れない。
TEST_F(FileIoTest, AtomicWriteUserFile_LeavesUnrelatedFilesAlone)
{
    const auto path = temp_dir_ / L"diagram.png";
    const auto first_tmp = [&] {
        auto p = path;
        p += std::format(L".{}-0.tmp", GetCurrentProcessId());
        return p;
    }();
    ASSERT_TRUE(Write(TmpOf(path), "user-file"));
    ASSERT_TRUE(Write(first_tmp, "user-file-2"));

    for (const std::string_view payload : { "first-content", "second", "" }) {
        SCOPED_TRACE(payload);
        ASSERT_TRUE(AtomicWriteUserFile(path, payload.data(), payload.size()));
        EXPECT_EQ(ReadAsString(path), payload);
    }
    EXPECT_EQ(ReadAsString(TmpOf(path)), "user-file");
    EXPECT_EQ(ReadAsString(first_tmp), "user-file-2");
    // 消費された一時ファイル以外は残らない。
    EXPECT_EQ(std::distance(fs::directory_iterator(temp_dir_), fs::directory_iterator{}), 3);
}

TEST_F(FileIoTest, AtomicWriteUserFile_ToInvalidPath_Fails)
{
    const auto path = temp_dir_ / L"no_such_dir" / L"diagram.png";
    EXPECT_FALSE(AtomicWriteUserFile(path, "data", 4));
    EXPECT_FALSE(fs::exists(path));
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

TEST(FileLoadErrorMessageTest, MapsEachErrorToToastText)
{
    const auto& s = i18n::kJa;
    EXPECT_EQ(FileLoadErrorMessage(FileLoadError::NotFound, s), s.toast_file_not_found);
    EXPECT_EQ(FileLoadErrorMessage(FileLoadError::Locked, s), s.toast_file_locked);
    EXPECT_EQ(FileLoadErrorMessage(FileLoadError::TooLarge, s), s.toast_file_too_large);
    EXPECT_EQ(FileLoadErrorMessage(FileLoadError::ReadFailed, s), s.toast_file_read_failed);
}
