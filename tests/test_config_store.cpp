#include <gtest/gtest.h>
#include "config_store.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <Windows.h>

namespace fs = std::filesystem;

class ConfigStoreTest : public ::testing::Test {
protected:
    fs::path temp_dir_;

    void SetUp() override
    {
        temp_dir_ = fs::temp_directory_path() / (L"mendo_test_config_" + std::to_wstring(GetCurrentProcessId()));
        fs::remove_all(temp_dir_);
        fs::create_directories(temp_dir_);
        config::SetConfigDirOverride(temp_dir_);
        config::Clear();
    }

    void TearDown() override
    {
        config::Clear();
        config::SetConfigDirOverride({});
        fs::remove_all(temp_dir_);
    }
};

// ---- GetConfigDir / GetConfigPath ----

TEST_F(ConfigStoreTest, GetConfigDirReturnsOverride)
{
    EXPECT_EQ(config::GetConfigDir(), temp_dir_);
}

TEST_F(ConfigStoreTest, GetConfigDirReturnsDefaultWhenNoOverride)
{
    config::SetConfigDirOverride({});
    auto dir = config::GetConfigDir();
    EXPECT_FALSE(dir.empty());
    EXPECT_NE(dir.wstring().find(L"mendo"), std::wstring::npos);
}

TEST_F(ConfigStoreTest, GetConfigPathCombinesCorrectly)
{
    auto path = config::GetConfigPath(L"test.txt");
    EXPECT_EQ(path, temp_dir_ / L"test.txt");
}

// ---- SetBool / GetBool ----

TEST_F(ConfigStoreTest, SetAndGetBoolTrue)
{
    config::SetBool("Test", "Flag", true);
    EXPECT_TRUE(config::GetBool("Test", "Flag", false));
}

TEST_F(ConfigStoreTest, SetAndGetBoolFalse)
{
    config::SetBool("Test", "Flag", false);
    EXPECT_FALSE(config::GetBool("Test", "Flag", true));
}

TEST_F(ConfigStoreTest, GetBoolReturnsDefaultWhenMissing)
{
    EXPECT_TRUE(config::GetBool("Test", "Missing", true));
    EXPECT_FALSE(config::GetBool("Test", "Missing", false));
}

TEST_F(ConfigStoreTest, SetBoolOverwrite)
{
    config::SetBool("Test", "Flag", true);
    EXPECT_TRUE(config::GetBool("Test", "Flag"));
    config::SetBool("Test", "Flag", false);
    EXPECT_FALSE(config::GetBool("Test", "Flag"));
}

// ---- SetInt / GetInt ----

TEST_F(ConfigStoreTest, SetAndGetInt)
{
    config::SetInt("Test", "Value", 42);
    EXPECT_EQ(config::GetInt("Test", "Value", 0, 0, 100), 42);
}

TEST_F(ConfigStoreTest, GetIntReturnsDefaultWhenMissing)
{
    EXPECT_EQ(config::GetInt("Test", "Missing", 7, 0, 100), 7);
}

TEST_F(ConfigStoreTest, GetIntReturnsDefaultWhenBelowMin)
{
    config::SetInt("Test", "Value", -5);
    EXPECT_EQ(config::GetInt("Test", "Value", 7, 0, 100), 7);
}

TEST_F(ConfigStoreTest, GetIntReturnsDefaultWhenAboveMax)
{
    config::SetInt("Test", "Value", 200);
    EXPECT_EQ(config::GetInt("Test", "Value", 7, 0, 100), 7);
}

TEST_F(ConfigStoreTest, GetIntBoundaryValues)
{
    config::SetInt("Test", "Min", 0);
    EXPECT_EQ(config::GetInt("Test", "Min", 99, 0, 100), 0);

    config::SetInt("Test", "Max", 100);
    EXPECT_EQ(config::GetInt("Test", "Max", 99, 0, 100), 100);
}

TEST_F(ConfigStoreTest, SetIntNegativeValues)
{
    config::SetInt("Test", "Neg", -10);
    EXPECT_EQ(config::GetInt("Test", "Neg", 0, -20, 20), -10);
}

// ---- SetWString / GetWString ----

TEST_F(ConfigStoreTest, SetAndGetWString)
{
    std::wstring_view test_path = L"C:\\Users\\test\\document.md";
    config::SetWString("Session", "LastFile", test_path);
    EXPECT_EQ(config::GetWString("Session", "LastFile"), test_path);
}

TEST_F(ConfigStoreTest, SetAndGetWStringJapanese)
{
    std::wstring_view jp = L"C:\\ユーザー\\テスト\\文書.md";
    config::SetWString("Session", "Path", jp);
    EXPECT_EQ(config::GetWString("Session", "Path"), jp);
}

TEST_F(ConfigStoreTest, GetWStringReturnsEmptyWhenMissing)
{
    EXPECT_TRUE(config::GetWString("Test", "Missing").empty());
}

TEST_F(ConfigStoreTest, SetWStringEmptyStoresEmpty)
{
    config::SetWString("Test", "Key", L"value");
    EXPECT_EQ(config::GetWString("Test", "Key"), L"value");
    config::SetWString("Test", "Key", L"");
    EXPECT_TRUE(config::GetWString("Test", "Key").empty());
}

TEST_F(ConfigStoreTest, SetWStringOverwrite)
{
    config::SetWString("Test", "Key", L"first");
    EXPECT_EQ(config::GetWString("Test", "Key"), L"first");
    config::SetWString("Test", "Key", L"second");
    EXPECT_EQ(config::GetWString("Test", "Key"), L"second");
}

// ---- Load / Save ラウンドトリップ ----

TEST_F(ConfigStoreTest, SaveAndLoadRoundTrip)
{
    config::SetBool("View", "DarkMode", true);
    config::SetInt("Window", "X", -500);
    config::SetWString("Session", "LastFile", L"C:\\テスト\\file.md");
    config::Save();

    config::Clear();
    EXPECT_FALSE(config::GetBool("View", "DarkMode"));

    config::Load();
    EXPECT_TRUE(config::GetBool("View", "DarkMode"));
    EXPECT_EQ(config::GetInt("Window", "X", 0, -100000, 100000), -500);
    EXPECT_EQ(config::GetWString("Session", "LastFile"), L"C:\\テスト\\file.md");
}

TEST_F(ConfigStoreTest, LoadEmptyDirectoryProducesDefaults)
{
    config::Load();
    EXPECT_EQ(config::GetInt("Window", "Width", 0, 100, 100000), 0);
    EXPECT_FALSE(config::GetBool("View", "DarkMode"));
}

// ---- 複数セクションの独立性 ----

TEST_F(ConfigStoreTest, MultipleSectionsIndependent)
{
    config::SetBool("View", "DarkMode", true);
    config::SetInt("Window", "X", 42);
    config::SetWString("Session", "LastFile", L"hello");

    EXPECT_TRUE(config::GetBool("View", "DarkMode"));
    EXPECT_EQ(config::GetInt("Window", "X", 0, 0, 100), 42);
    EXPECT_EQ(config::GetWString("Session", "LastFile"), L"hello");
}

// ---- ウィンドウ配置設定テスト ----

TEST_F(ConfigStoreTest, WindowPlacementRoundTrip)
{
    config::SetInt("Window", "X", -500);
    config::SetInt("Window", "Y", 200);
    config::SetInt("Window", "Width", 1600);
    config::SetInt("Window", "Height", 900);
    config::SetBool("Window", "Maximized", true);

    EXPECT_EQ(config::GetInt("Window", "X", 0, -100000, 100000), -500);
    EXPECT_EQ(config::GetInt("Window", "Y", 0, -100000, 100000), 200);
    EXPECT_EQ(config::GetInt("Window", "Width", 0, 100, 100000), 1600);
    EXPECT_EQ(config::GetInt("Window", "Height", 0, 100, 100000), 900);
    EXPECT_TRUE(config::GetBool("Window", "Maximized"));
}

TEST_F(ConfigStoreTest, WindowPlacementMissingReturnsDefaults)
{
    EXPECT_EQ(config::GetInt("Window", "Width", 0, 100, 100000), 0);
    EXPECT_EQ(config::GetInt("Window", "Height", 0, 100, 100000), 0);
    EXPECT_FALSE(config::GetBool("Window", "Maximized"));
}

// ---- スクロール復元設定テスト ----

TEST_F(ConfigStoreTest, ScrollNodeRoundTrip)
{
    config::SetInt("Session", "ScrollNode", 42);
    config::SetInt("Session", "ScrollOffset", 15);

    EXPECT_EQ(config::GetInt("Session", "ScrollNode", -1, -1, 100000000), 42);
    EXPECT_EQ(config::GetInt("Session", "ScrollOffset", 0, -100000, 100000), 15);
}

TEST_F(ConfigStoreTest, ScrollNodeMissingReturnsDefault)
{
    EXPECT_EQ(config::GetInt("Session", "ScrollNode", -1, -1, 100000000), -1);
    EXPECT_EQ(config::GetInt("Session", "ScrollOffset", 0, -100000, 100000), 0);
}

// ---- INIファイル破損テスト ----

TEST_F(ConfigStoreTest, LoadCorruptedIniFile)
{
    {
        std::ofstream(temp_dir_ / L"settings.ini") << "garbage content\nno sections\n";
    }

    config::Load();
    // パースエラーでもクラッシュしないことを確認
    EXPECT_EQ(config::GetInt("Window", "Width", 0, 100, 100000), 0);
}

TEST_F(ConfigStoreTest, GetIntCorruptedValue)
{
    config::SetInt("Test", "Value", 42);
    // メモリ上のデータを直接書き換えてテスト（Save/Loadでシミュレーション）
    config::Save();
    // INIファイルを手動で破壊
    {
        auto path = temp_dir_ / L"settings.ini";
        std::ofstream ofs(path);
        ofs << "[Test]\nValue=not_a_number\n";
    }
    config::Clear();
    config::Load();
    EXPECT_EQ(config::GetInt("Test", "Value", 7, 0, 100), 7);
}
