#include <gtest/gtest.h>
#include "config_service.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <Windows.h>

namespace fs = std::filesystem;

class ConfigServiceTest : public ::testing::Test {
protected:
    fs::path temp_dir_;
    ConfigService config_;

    void SetUp() override
    {
        temp_dir_ = fs::temp_directory_path() / (L"mendo_test_config_" + std::to_wstring(GetCurrentProcessId()));
        fs::remove_all(temp_dir_);
        fs::create_directories(temp_dir_);
        config_.SetConfigDirOverride(temp_dir_);
    }

    void TearDown() override
    {
        fs::remove_all(temp_dir_);
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
    auto dir = config_.GetConfigDir();
    EXPECT_FALSE(dir.empty());
    EXPECT_NE(dir.wstring().find(L"mendo"), std::wstring::npos);
}

TEST_F(ConfigServiceTest, GetConfigPathCombinesCorrectly)
{
    auto path = config_.GetConfigPath(L"test.txt");
    EXPECT_EQ(path, temp_dir_ / L"test.txt");
}

// ---- SaveBool / LoadBool ----

TEST_F(ConfigServiceTest, SetAndGetBoolTrue)
{
    config_.SaveBool("Test", "Flag", true);
    EXPECT_TRUE(config_.LoadBool("Test", "Flag", false));
}

TEST_F(ConfigServiceTest, SetAndGetBoolFalse)
{
    config_.SaveBool("Test", "Flag", false);
    EXPECT_FALSE(config_.LoadBool("Test", "Flag", true));
}

TEST_F(ConfigServiceTest, GetBoolReturnsDefaultWhenMissing)
{
    EXPECT_TRUE(config_.LoadBool("Test", "Missing", true));
    EXPECT_FALSE(config_.LoadBool("Test", "Missing", false));
}

TEST_F(ConfigServiceTest, SetBoolOverwrite)
{
    config_.SaveBool("Test", "Flag", true);
    EXPECT_TRUE(config_.LoadBool("Test", "Flag"));
    config_.SaveBool("Test", "Flag", false);
    EXPECT_FALSE(config_.LoadBool("Test", "Flag"));
}

// ---- SaveInt / LoadInt ----

TEST_F(ConfigServiceTest, SetAndGetInt)
{
    config_.SaveInt("Test", "Value", 42);
    EXPECT_EQ(config_.LoadInt("Test", "Value", 0, 0, 100), 42);
}

TEST_F(ConfigServiceTest, GetIntReturnsDefaultWhenMissing)
{
    EXPECT_EQ(config_.LoadInt("Test", "Missing", 7, 0, 100), 7);
}

TEST_F(ConfigServiceTest, GetIntReturnsDefaultWhenBelowMin)
{
    config_.SaveInt("Test", "Value", -5);
    EXPECT_EQ(config_.LoadInt("Test", "Value", 7, 0, 100), 7);
}

TEST_F(ConfigServiceTest, GetIntReturnsDefaultWhenAboveMax)
{
    config_.SaveInt("Test", "Value", 200);
    EXPECT_EQ(config_.LoadInt("Test", "Value", 7, 0, 100), 7);
}

TEST_F(ConfigServiceTest, GetIntBoundaryValues)
{
    config_.SaveInt("Test", "Min", 0);
    EXPECT_EQ(config_.LoadInt("Test", "Min", 99, 0, 100), 0);

    config_.SaveInt("Test", "Max", 100);
    EXPECT_EQ(config_.LoadInt("Test", "Max", 99, 0, 100), 100);
}

TEST_F(ConfigServiceTest, SetIntNegativeValues)
{
    config_.SaveInt("Test", "Neg", -10);
    EXPECT_EQ(config_.LoadInt("Test", "Neg", 0, -20, 20), -10);
}

// ---- SaveWString / LoadWString ----

TEST_F(ConfigServiceTest, SetAndGetWString)
{
    std::wstring_view test_path = L"C:\\Users\\test\\document.md";
    config_.SaveWString("Session", "LastFile", test_path);
    EXPECT_EQ(config_.LoadWString("Session", "LastFile"), test_path);
}

TEST_F(ConfigServiceTest, SetAndGetWStringJapanese)
{
    std::wstring_view jp = L"C:\\ユーザー\\テスト\\文書.md";
    config_.SaveWString("Session", "Path", jp);
    EXPECT_EQ(config_.LoadWString("Session", "Path"), jp);
}

TEST_F(ConfigServiceTest, GetWStringReturnsEmptyWhenMissing)
{
    EXPECT_TRUE(config_.LoadWString("Test", "Missing").empty());
}

TEST_F(ConfigServiceTest, SetWStringEmptyStoresEmpty)
{
    config_.SaveWString("Test", "Key", L"value");
    EXPECT_EQ(config_.LoadWString("Test", "Key"), L"value");
    config_.SaveWString("Test", "Key", L"");
    EXPECT_TRUE(config_.LoadWString("Test", "Key").empty());
}

TEST_F(ConfigServiceTest, SetWStringOverwrite)
{
    config_.SaveWString("Test", "Key", L"first");
    EXPECT_EQ(config_.LoadWString("Test", "Key"), L"first");
    config_.SaveWString("Test", "Key", L"second");
    EXPECT_EQ(config_.LoadWString("Test", "Key"), L"second");
}

// ---- Load / Flush ラウンドトリップ ----

TEST_F(ConfigServiceTest, FlushAndLoadRoundTrip)
{
    config_.SaveBool("View", "DarkMode", true);
    config_.SaveInt("Window", "X", -500);
    config_.SaveWString("Session", "LastFile", L"C:\\テスト\\file.md");
    config_.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool("View", "DarkMode"));
    EXPECT_EQ(fresh.LoadInt("Window", "X", 0, -100000, 100000), -500);
    EXPECT_EQ(fresh.LoadWString("Session", "LastFile"), L"C:\\テスト\\file.md");
}

TEST_F(ConfigServiceTest, LoadEmptyDirectoryProducesDefaults)
{
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 0);
    EXPECT_FALSE(config_.LoadBool("View", "DarkMode"));
}

TEST_F(ConfigServiceTest, LoadSkipsUtf8Bom)
{
    // メモ帳等の BOM 付き UTF-8 で手編集された settings.ini でも
    // 先頭セクションが無言で失われないこと
    {
        std::ofstream out(temp_dir_ / L"settings.ini", std::ios::binary);
        out << "\xEF\xBB\xBF[Window]\r\nX=42\r\n";
    }
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Window", "X", 0, 0, 100), 42);
}

// ---- 複数セクションの独立性 ----

TEST_F(ConfigServiceTest, MultipleSectionsIndependent)
{
    config_.SaveBool("View", "DarkMode", true);
    config_.SaveInt("Window", "X", 42);
    config_.SaveWString("Session", "LastFile", L"hello");

    EXPECT_TRUE(config_.LoadBool("View", "DarkMode"));
    EXPECT_EQ(config_.LoadInt("Window", "X", 0, 0, 100), 42);
    EXPECT_EQ(config_.LoadWString("Session", "LastFile"), L"hello");
}

// ---- ウィンドウ配置設定テスト ----

TEST_F(ConfigServiceTest, WindowPlacementRoundTrip)
{
    config_.SaveInt("Window", "X", -500);
    config_.SaveInt("Window", "Y", 200);
    config_.SaveInt("Window", "Width", 1600);
    config_.SaveInt("Window", "Height", 900);
    config_.SaveBool("Window", "Maximized", true);

    EXPECT_EQ(config_.LoadInt("Window", "X", 0, -100000, 100000), -500);
    EXPECT_EQ(config_.LoadInt("Window", "Y", 0, -100000, 100000), 200);
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 1600);
    EXPECT_EQ(config_.LoadInt("Window", "Height", 0, 100, 100000), 900);
    EXPECT_TRUE(config_.LoadBool("Window", "Maximized"));
}

TEST_F(ConfigServiceTest, WindowPlacementMissingReturnsDefaults)
{
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 0);
    EXPECT_EQ(config_.LoadInt("Window", "Height", 0, 100, 100000), 0);
    EXPECT_FALSE(config_.LoadBool("Window", "Maximized"));
}

// ---- スクロール復元設定テスト ----

TEST_F(ConfigServiceTest, ScrollNodeRoundTrip)
{
    config_.SaveInt("Session", "ScrollNode", 42);
    config_.SaveInt("Session", "ScrollOffset", 15);

    EXPECT_EQ(config_.LoadInt("Session", "ScrollNode", -1, -1, 100000000), 42);
    EXPECT_EQ(config_.LoadInt("Session", "ScrollOffset", 0, -100000, 100000), 15);
}

TEST_F(ConfigServiceTest, ScrollNodeMissingReturnsDefault)
{
    EXPECT_EQ(config_.LoadInt("Session", "ScrollNode", -1, -1, 100000000), -1);
    EXPECT_EQ(config_.LoadInt("Session", "ScrollOffset", 0, -100000, 100000), 0);
}

// ---- INIファイル破損テスト ----

TEST_F(ConfigServiceTest, LoadCorruptedIniFile)
{
    {
        std::ofstream(temp_dir_ / L"settings.ini") << "garbage content\nno sections\n";
    }

    config_.Load();
    // パースエラーでもクラッシュしないことを確認
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 0);
}

TEST_F(ConfigServiceTest, GetIntCorruptedValue)
{
    config_.SaveInt("Test", "Value", 42);
    config_.Flush();
    {
        auto path = temp_dir_ / L"settings.ini";
        std::ofstream ofs(path);
        ofs << "[Test]\nValue=not_a_number\n";
    }
    auto fresh = MakeFreshLoadedConfig();
    EXPECT_EQ(fresh.LoadInt("Test", "Value", 7, 0, 100), 7);
}
