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
        const auto buf = ReadAllBytes(path.c_str(), kMaxTestFileBytes);
        return { reinterpret_cast<const char*>(buf.data()), buf.size() };
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
        { L"\\", true },
        // \??\ は 4 文字そろって初めて NT パスになる。
        { L"\\??", true },
        { L"\\??a", true },
        { L"\\?x\\a", true },
        { L"\\x?\\a", true },

        { L"\\\\", false },
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
        { L"\\\\?host\\share", false },
        { L"\\\\.host\\share", false },
        // サーバー名が空、先頭の区切りが 1 つだけ。
        { L"\\\\\\share", false },
        { L"\\srv\\share", false },
        { L"s\\\\srv\\share", false },
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
        // ドライブは「英字 + :」の後ろが区切りか終端のときだけ。
        { L"\\\\?\\C:", L"C:" },
        { L"\\\\?\\C", std::nullopt },
        { L"\\\\?\\C:dir", std::nullopt },
        { L"\\\\?\\1:\\dir", std::nullopt },
        // UNC は「UNC + 区切り」のときだけ。
        { L"\\\\?\\UNC\\", L"\\\\" },
        { L"\\\\?\\UNC", std::nullopt },
        { L"\\\\?\\UNCx\\host", std::nullopt },
        { L"\\\\?\\abc\\host", std::nullopt },
        // デバイスパスの接頭辞は 4 文字そろって初めて外す。1 文字違えばそのまま返す。
        { L"\\\\?C:\\dir", L"\\\\?C:\\dir" },
        { L"\\\\h\\C:\\dir", L"\\\\h\\C:\\dir" },
        { L"\\x?\\C:\\dir", L"\\x?\\C:\\dir" },
        { L"x\\?\\C:\\dir", L"x\\?\\C:\\dir" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::PrintToString(std::wstring(c.path)));
        EXPECT_EQ(path_util::ToWin32Path(c.path), c.expected);
    }
}

TEST_F(FileIoTest, ReadAllBytesReturnsBinaryAsIs)
{
    const auto path = temp_dir_ / L"binary.bin";
    std::vector<uint8_t> payload(256);
    std::ranges::iota(payload, uint8_t{ 0 });

    ASSERT_TRUE(WriteBinaryFile(path, payload.data(), payload.size()));

    const auto buf = ReadAllBytes(path.c_str(), kMaxTestFileBytes);
    ASSERT_EQ(buf.size(), payload.size());
    EXPECT_TRUE(std::ranges::equal(buf, payload));
}

TEST_F(FileIoTest, ReadMissingFileReturnsEmpty)
{
    const auto path = temp_dir_ / L"does_not_exist.bin";
    EXPECT_TRUE(ReadAllBytes(path.c_str(), kMaxTestFileBytes).empty());
    // 空ファイルは失敗と同じく空を返す。
    EXPECT_TRUE(ReadAllBytes(WriteTempFile(L"empty.bin").c_str(), kMaxTestFileBytes).empty());
}

// ═══════════════════════════════════════════════
// IsFileLargerThan: エディタの書き込み中検知
// ═══════════════════════════════════════════════

// 読んだ 1000 バイトより、許容差 16 バイトを超えて大きいときだけ true。
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

// 共有を許さずに開かれているファイルは、無いファイルと分けて Locked にする (読み直しで待つ)。
TEST_F(FileIoTest, OpenFileForReadShared_ReportsLockedWhileOpenedExclusively)
{
    const auto path = WriteTempFile(L"locked.md", "body");
    {
        UniqueHandle exclusive(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        ASSERT_TRUE(exclusive);
        const auto r = OpenFileForReadShared(path.c_str(), 1024);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error(), FileLoadError::Locked);
    }
    EXPECT_TRUE(OpenFileForReadShared(path.c_str(), 1024));
    const auto missing = OpenFileForReadShared((temp_dir_ / L"missing.md").c_str(), 1024);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), FileLoadError::NotFound);
}

// 利用者が選んだ保存先では、同じフォルダにある `<path>.tmp` や一時ファイルと同じ名前のファイルに触れない。
TEST_F(FileIoTest, AtomicWriteUserFile_LeavesUnrelatedFilesAlone)
{
    const auto path = temp_dir_ / L"diagram.png";
    // 一時ファイルによくある名前と、自分が最初に作る一時ファイルの名前。
    const auto user_tmp = WriteTempFile(L"diagram.png.tmp", "user-file");
    const auto first_tmp = WriteTempFile(std::format(L"diagram.png.{}-0.tmp", GetCurrentProcessId()), "user-file-2");

    for (const std::string_view payload : { "first-content", "second", "" }) {
        SCOPED_TRACE(payload);
        ASSERT_TRUE(AtomicWriteUserFile(path, payload.data(), payload.size()));
        EXPECT_EQ(ReadAsString(path), payload);
    }
    EXPECT_EQ(ReadAsString(user_tmp), "user-file");
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
