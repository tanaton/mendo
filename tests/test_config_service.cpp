#include <gtest/gtest.h>
#include "config_service.h"
#include "example_files.h"
#include "test_helpers.h"
#include <filesystem>
#include <string>

class ConfigServiceTest : public TempDirTestBase {
protected:
    ConfigService config_;

    void SetUp() override
    {
        config_.SetConfigDirOverride(temp_dir_);
    }

    // 同じ temp_dir_ を共有する別インスタンス。Flush 後に Load してラウンドトリップを検証する。
    ConfigService MakeFreshLoadedConfig()
    {
        ConfigService c;
        c.SetConfigDirOverride(temp_dir_);
        c.Load();
        return c;
    }
};

// ---- GetConfigDir / GetConfigPath ----

TEST_F(ConfigServiceTest, GetConfigDirReturnsOverride)
{
    EXPECT_EQ(config_.GetConfigDir(), temp_dir_);
}

TEST_F(ConfigServiceTest, GetConfigDirReturnsDefaultWhenNoOverride)
{
    config_.SetConfigDirOverride({});
    const auto& dir = config_.GetConfigDir();
    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(dir.wstring().contains(L"mendo"));
}

TEST_F(ConfigServiceTest, GetConfigPathCombinesCorrectly)
{
    auto path = config_.GetConfigPath(L"test.txt");
    EXPECT_EQ(path, temp_dir_ / L"test.txt");
}

// ---- 値の保存と読み出し ----

TEST_F(ConfigServiceTest, MissingKeysReturnDefaults)
{
    EXPECT_TRUE(config_.LoadBool("Test", "Missing", true));
    EXPECT_FALSE(config_.LoadBool("Test", "Missing", false));
    EXPECT_EQ(config_.LoadInt("Test", "Missing", 7, 0, 100), 7);
    EXPECT_TRUE(config_.LoadWString("Test", "Missing").empty());
}

// 同じキーへ順に書き、上書きも確かめる。既定値と逆の値を読めれば保存値が返っている。
TEST_F(ConfigServiceTest, SaveBoolOverwrites)
{
    config_.SaveBool("Test", "Flag", true);
    EXPECT_TRUE(config_.LoadBool("Test", "Flag", false));
    config_.SaveBool("Test", "Flag", false);
    EXPECT_FALSE(config_.LoadBool("Test", "Flag", true));
}

// 範囲は両端を含み、範囲外の保存値は既定値に倒す。
TEST_F(ConfigServiceTest, LoadIntReturnsDefaultOutsideRange)
{
    struct Case {
        int stored;
        int min_v;
        int max_v;
        int expected;
    };
    constexpr Case kCases[] = {
        { 42, 0, 100, 42 },
        { 0, 0, 100, 0 },
        { 100, 0, 100, 100 },
        { -10, -20, 20, -10 },
        { -5, 0, 100, 7 },
        { 200, 0, 100, 7 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.stored);
        config_.SaveInt("Test", "Value", c.stored);
        EXPECT_EQ(config_.LoadInt("Test", "Value", 7, c.min_v, c.max_v), c.expected);
    }
}

// 同じキーへ順に書き、上書きと空文字列の保存も確かめる。
TEST_F(ConfigServiceTest, SaveWStringOverwrites)
{
    for (const std::wstring_view value : { L"C:\\Users\\test\\document.md", L"C:\\ユーザー\\テスト\\文書.md", L"" }) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ value }));
        config_.SaveWString("Session", "LastFile", value);
        EXPECT_EQ(config_.LoadWString("Session", "LastFile"), value);
    }
}

// ---- Load / Flush ラウンドトリップ ----

TEST_F(ConfigServiceTest, FlushAndLoadRoundTrip)
{
    config_.SaveBool("View", "DarkMode", true);
    config_.SaveInt("Window", "X", -500);
    config_.SaveWString("Session", "LastFile", L"C:\\テスト\\file.md");
    config_.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool("View", "DarkMode", false));
    EXPECT_EQ(fresh.LoadInt("Window", "X", 0, -100000, 100000), -500);
    EXPECT_EQ(fresh.LoadWString("Session", "LastFile"), L"C:\\テスト\\file.md");
}

TEST_F(ConfigServiceTest, LoadEmptyDirectoryProducesDefaults)
{
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 0);
    EXPECT_FALSE(config_.LoadBool("View", "DarkMode", false));
}

TEST_F(ConfigServiceTest, LoadSkipsUtf8Bom)
{
    // メモ帳等の BOM 付き UTF-8 で手編集された settings.ini でも
    // 先頭セクションが無言で失われないこと
    WriteTempFile(L"settings.ini", "\xEF\xBB\xBF[Window]\r\nX=42\r\n");
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Window", "X", 0, 0, 100), 42);
}

TEST_F(ConfigServiceTest, FlushSkipsWriteWhenUnchanged)
{
    // Serialize と異なる改行コードの内容がそのまま残っていれば書き戻されていない
    const std::string original = "[Window]\r\nX=42\r\n";
    const auto path = WriteTempFile(L"settings.ini", original);
    config_.Load();
    config_.SaveInt("Window", "X", 42);
    config_.Flush();

    EXPECT_EQ(ReadFileBytes(path), original);
}

TEST_F(ConfigServiceTest, FlushWithoutAnySaveCreatesNoFile)
{
    config_.Flush();
    EXPECT_FALSE(std::filesystem::exists(temp_dir_ / L"settings.ini"));
}

TEST_F(ConfigServiceTest, FlushWritesChangedValue)
{
    WriteTempFile(L"settings.ini", "[Window]\r\nX=42\r\n");
    config_.Load();
    config_.SaveInt("Window", "X", 43);
    config_.Flush();

    EXPECT_EQ(MakeFreshLoadedConfig().LoadInt("Window", "X", 0, 0, 100), 43);
}

// ---- INIファイル破損テスト ----

TEST_F(ConfigServiceTest, LoadCorruptedIniFile)
{
    WriteTempFile(L"settings.ini", "garbage content\nno sections\n");

    config_.Load();
    // パースエラーでもクラッシュしないことを確認
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 0);
}

// 数値として読み切れない値 (末尾にゴミが残る部分パースを含む) は既定値に倒す。
TEST_F(ConfigServiceTest, LoadIntReturnsDefaultForNonNumericValue)
{
    WriteTempFile(L"settings.ini", "[Test]\nWord=not_a_number\nSuffix=30px\n");
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Test", "Word", 7, 0, 100), 7);
    EXPECT_EQ(config_.LoadInt("Test", "Suffix", 7, 0, 100), 7);
}
