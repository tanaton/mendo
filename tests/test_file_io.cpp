#include <gtest/gtest.h>
#include "file_io.h"
#include "test_helpers.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class FileIoTest : public TempDirTestBase {};

// ═══════════════════════════════════════════════
// WriteAllBytes + ReadAllBytes ラウンドトリップ
// ═══════════════════════════════════════════════

TEST_F(FileIoTest, RoundTripAscii)
{
    const auto path = temp_dir_ / L"ascii.bin";
    const std::string payload = "Hello, mendo!";

    ASSERT_TRUE(WriteAllBytes(path, payload.data(), payload.size()));

    auto [buf, size] = ReadAllBytes(path);
    ASSERT_NE(buf.get(), nullptr);
    ASSERT_EQ(size, payload.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.get()), size), payload);
}

TEST_F(FileIoTest, RoundTripBinary)
{
    const auto path = temp_dir_ / L"binary.bin";
    std::vector<uint8_t> payload;
    for (int i = 0; i < 256; ++i) {
        payload.push_back(static_cast<uint8_t>(i));
    }

    ASSERT_TRUE(WriteAllBytes(path, payload.data(), payload.size()));

    auto [buf, size] = ReadAllBytes(path);
    ASSERT_EQ(size, payload.size());
    EXPECT_EQ(std::memcmp(buf.get(), payload.data(), size), 0);
}

TEST_F(FileIoTest, WriteOverwritesExistingFile)
{
    const auto path = temp_dir_ / L"overwrite.bin";
    const std::string first = "first-content-longer";
    const std::string second = "short";

    ASSERT_TRUE(WriteAllBytes(path, first.data(), first.size()));
    ASSERT_TRUE(WriteAllBytes(path, second.data(), second.size()));

    auto [buf, size] = ReadAllBytes(path);
    ASSERT_EQ(size, second.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.get()), size), second);
}

// ═══════════════════════════════════════════════
// エラーケース
// ═══════════════════════════════════════════════

TEST_F(FileIoTest, ReadMissingFileReturnsEmpty)
{
    const auto path = temp_dir_ / L"does_not_exist.bin";
    DWORD err = 0xDEADBEEF;
    auto [buf, size] = ReadAllBytes(path, &err);
    EXPECT_EQ(buf.get(), nullptr);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(err, static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
}

TEST_F(FileIoTest, ReadMissingFileWithoutErrorPointer)
{
    const auto path = temp_dir_ / L"does_not_exist.bin";
    auto [buf, size] = ReadAllBytes(path);
    EXPECT_EQ(buf.get(), nullptr);
    EXPECT_EQ(size, 0u);
}

TEST_F(FileIoTest, ReadEmptyFileReturnsEmpty)
{
    const auto path = temp_dir_ / L"empty.bin";
    std::ofstream(path, std::ios::binary).close();

    DWORD err = 0xDEADBEEF;
    auto [buf, size] = ReadAllBytes(path, &err);
    EXPECT_EQ(buf.get(), nullptr);
    EXPECT_EQ(size, 0u);
    // 成功時 (CreateFileW OK) の err はクリアされる契約
    EXPECT_EQ(err, 0u);
}

TEST_F(FileIoTest, WriteToInvalidPathFails)
{
    // 存在しないディレクトリ配下のパス（親ディレクトリがない）
    const auto path = temp_dir_ / L"no_such_dir" / L"file.bin";
    const std::string payload = "data";
    EXPECT_FALSE(WriteAllBytes(path, payload.data(), payload.size()));
}

TEST_F(FileIoTest, ErrorPointerResetOnSuccess)
{
    const auto path = temp_dir_ / L"reset.bin";
    const std::string payload = "x";
    ASSERT_TRUE(WriteAllBytes(path, payload.data(), payload.size()));

    DWORD err = 0xDEADBEEF;
    auto [buf, size] = ReadAllBytes(path, &err);
    EXPECT_EQ(err, 0u);
    EXPECT_EQ(size, 1u);
}

TEST_F(FileIoTest, WriteZeroBytesProducesEmptyFile)
{
    const auto path = temp_dir_ / L"zero.bin";
    ASSERT_TRUE(WriteAllBytes(path, "", 0));

    auto [buf, size] = ReadAllBytes(path);
    EXPECT_EQ(size, 0u);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_EQ(fs::file_size(path), 0u);
}
