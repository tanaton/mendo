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

    void SetUp() override {
        // 各テスト用に一意の一時ディレクトリを作成
        temp_dir_ = fs::temp_directory_path() / (L"mendo_test_config_" + std::to_wstring(GetCurrentProcessId()));
        fs::remove_all(temp_dir_);
        fs::create_directories(temp_dir_);
        config::SetConfigDirOverride(temp_dir_);
    }

    void TearDown() override {
        config::SetConfigDirOverride({});
        fs::remove_all(temp_dir_);
    }
};

// ---- GetConfigDir / GetConfigPath ----

TEST_F(ConfigStoreTest, GetConfigDirReturnsOverride) {
    EXPECT_EQ(config::GetConfigDir(), temp_dir_);
}

TEST_F(ConfigStoreTest, GetConfigDirReturnsDefaultWhenNoOverride) {
    config::SetConfigDirOverride({});
    auto dir = config::GetConfigDir();
    // "mendo"を含む有効なパスを返すべき
    EXPECT_FALSE(dir.empty());
    EXPECT_NE(dir.wstring().find(L"mendo"), std::wstring::npos);
}

TEST_F(ConfigStoreTest, GetConfigPathCombinesCorrectly) {
    auto path = config::GetConfigPath(L"test.txt");
    EXPECT_EQ(path, temp_dir_ / L"test.txt");
}

// ---- SaveBool / LoadBool ----

TEST_F(ConfigStoreTest, SaveAndLoadBoolTrue) {
    config::SaveBool(L"bool_test.txt", true);
    EXPECT_TRUE(config::LoadBool(L"bool_test.txt", false));
}

TEST_F(ConfigStoreTest, SaveAndLoadBoolFalse) {
    config::SaveBool(L"bool_test.txt", false);
    EXPECT_FALSE(config::LoadBool(L"bool_test.txt", true));
}

TEST_F(ConfigStoreTest, LoadBoolReturnsDefaultWhenFileMissing) {
    EXPECT_TRUE(config::LoadBool(L"nonexistent.txt", true));
    EXPECT_FALSE(config::LoadBool(L"nonexistent.txt", false));
}

TEST_F(ConfigStoreTest, LoadBoolOverwrite) {
    config::SaveBool(L"bool_ow.txt", true);
    EXPECT_TRUE(config::LoadBool(L"bool_ow.txt"));
    config::SaveBool(L"bool_ow.txt", false);
    EXPECT_FALSE(config::LoadBool(L"bool_ow.txt"));
}

// ---- SaveInt / LoadInt ----

TEST_F(ConfigStoreTest, SaveAndLoadInt) {
    config::SaveInt(L"int_test.txt", 42);
    EXPECT_EQ(config::LoadInt(L"int_test.txt", 0, 0, 100), 42);
}

TEST_F(ConfigStoreTest, LoadIntReturnsDefaultWhenFileMissing) {
    EXPECT_EQ(config::LoadInt(L"nonexistent.txt", 7, 0, 100), 7);
}

TEST_F(ConfigStoreTest, LoadIntReturnsDefaultWhenBelowMin) {
    config::SaveInt(L"int_low.txt", -5);
    EXPECT_EQ(config::LoadInt(L"int_low.txt", 7, 0, 100), 7);
}

TEST_F(ConfigStoreTest, LoadIntReturnsDefaultWhenAboveMax) {
    config::SaveInt(L"int_high.txt", 200);
    EXPECT_EQ(config::LoadInt(L"int_high.txt", 7, 0, 100), 7);
}

TEST_F(ConfigStoreTest, LoadIntBoundaryValues) {
    config::SaveInt(L"int_min.txt", 0);
    EXPECT_EQ(config::LoadInt(L"int_min.txt", 99, 0, 100), 0);

    config::SaveInt(L"int_max.txt", 100);
    EXPECT_EQ(config::LoadInt(L"int_max.txt", 99, 0, 100), 100);
}

TEST_F(ConfigStoreTest, LoadIntCorruptedFile) {
    // 整数でないコンテンツを書き込み
    auto path = config::GetConfigPath(L"int_corrupt.txt");
    std::ofstream ofs(path);
    ofs << "not_a_number";
    ofs.close();
    EXPECT_EQ(config::LoadInt(L"int_corrupt.txt", 7, 0, 100), 7);
}

TEST_F(ConfigStoreTest, SaveIntNegativeValues) {
    config::SaveInt(L"int_neg.txt", -10);
    EXPECT_EQ(config::LoadInt(L"int_neg.txt", 0, -20, 20), -10);
}

// ---- SaveWString / LoadWString ----

TEST_F(ConfigStoreTest, SaveAndLoadWString) {
    std::pmr::wstring test_path = L"C:\\Users\\test\\document.md";
    config::SaveWString(L"wstr_test.txt", test_path);
    EXPECT_EQ(config::LoadWString(L"wstr_test.txt"), test_path);
}

TEST_F(ConfigStoreTest, SaveAndLoadWStringJapanese) {
    std::pmr::wstring jp = L"C:\\ユーザー\\テスト\\文書.md";
    config::SaveWString(L"wstr_jp.txt", jp);
    EXPECT_EQ(config::LoadWString(L"wstr_jp.txt"), jp);
}

TEST_F(ConfigStoreTest, LoadWStringReturnsEmptyWhenFileMissing) {
    EXPECT_TRUE(config::LoadWString(L"nonexistent.txt").empty());
}

TEST_F(ConfigStoreTest, SaveWStringEmptyDoesNothing) {
    config::SaveWString(L"wstr_empty.txt", L"");
    // ファイルは存在しないべき（空文字列は保存されない）
    EXPECT_TRUE(config::LoadWString(L"wstr_empty.txt").empty());
}

// バグ #13: 空でのSaveWStringは以前に保存された値をクリアすべき
TEST_F(ConfigStoreTest, SaveWStringEmptyClearsPrevious) {
    config::SaveWString(L"wstr_clear.txt", L"some value");
    EXPECT_EQ(config::LoadWString(L"wstr_clear.txt"), L"some value");

    config::SaveWString(L"wstr_clear.txt", L"");
    EXPECT_TRUE(config::LoadWString(L"wstr_clear.txt").empty());
}

TEST_F(ConfigStoreTest, SaveWStringOverwrite) {
    config::SaveWString(L"wstr_ow.txt", L"first");
    EXPECT_EQ(config::LoadWString(L"wstr_ow.txt"), L"first");
    config::SaveWString(L"wstr_ow.txt", L"second");
    EXPECT_EQ(config::LoadWString(L"wstr_ow.txt"), L"second");
}

TEST_F(ConfigStoreTest, LoadWStringCorruptedOddBytes) {
    // 奇数バイトを書き込み（UTF-16LEとして無効）
    auto path = config::GetConfigPath(L"wstr_corrupt.txt");
    std::ofstream ofs(path, std::ios::binary);
    ofs.write("abc", 3);  // 3 bytes, not divisible by sizeof(wchar_t)
    ofs.close();
    EXPECT_TRUE(config::LoadWString(L"wstr_corrupt.txt").empty());
}

// ---- 設定ディレクトリの作成 ----

TEST_F(ConfigStoreTest, SaveCreatesDirectories) {
    // まだ存在しないネストされたパスにオーバーライドを設定
    fs::path nested = temp_dir_ / L"sub" / L"dir";
    config::SetConfigDirOverride(nested);
    config::SaveBool(L"nested_test.txt", true);
    EXPECT_TRUE(config::LoadBool(L"nested_test.txt", false));
}

// ---- 複数の設定ファイルの共存 ----

TEST_F(ConfigStoreTest, MultipleConfigFilesIndependent) {
    config::SaveBool(L"a.txt", true);
    config::SaveInt(L"b.txt", 42);
    config::SaveWString(L"c.txt", L"hello");

    EXPECT_TRUE(config::LoadBool(L"a.txt"));
    EXPECT_EQ(config::LoadInt(L"b.txt", 0, 0, 100), 42);
    EXPECT_EQ(config::LoadWString(L"c.txt"), L"hello");
}

// ---- ウィンドウ配置設定テスト ----

TEST_F(ConfigStoreTest, WindowPlacementRoundTrip) {
    // マルチモニター環境での負の座標を含むウィンドウ配置の保存/復元
    config::SaveInt(L"window_x.txt", -500);
    config::SaveInt(L"window_y.txt", 200);
    config::SaveInt(L"window_w.txt", 1600);
    config::SaveInt(L"window_h.txt", 900);
    config::SaveBool(L"window_maximized.txt", true);

    EXPECT_EQ(config::LoadInt(L"window_x.txt", 0, -100000, 100000), -500);
    EXPECT_EQ(config::LoadInt(L"window_y.txt", 0, -100000, 100000), 200);
    EXPECT_EQ(config::LoadInt(L"window_w.txt", 0, 100, 100000), 1600);
    EXPECT_EQ(config::LoadInt(L"window_h.txt", 0, 100, 100000), 900);
    EXPECT_TRUE(config::LoadBool(L"window_maximized.txt"));
}

TEST_F(ConfigStoreTest, WindowPlacementMissingReturnsDefaults) {
    // 初回起動時: 保存済み設定がなければデフォルト値を返す
    EXPECT_EQ(config::LoadInt(L"window_w.txt", 0, 100, 100000), 0);
    EXPECT_EQ(config::LoadInt(L"window_h.txt", 0, 100, 100000), 0);
    EXPECT_FALSE(config::LoadBool(L"window_maximized.txt"));
}

TEST_F(ConfigStoreTest, WindowPlacementTooSmallReturnsDefault) {
    // 幅/高さが最小値未満の場合はデフォルト値を返す
    config::SaveInt(L"window_w.txt", 50);  // 最小100未満
    config::SaveInt(L"window_h.txt", 30);  // 最小100未満
    EXPECT_EQ(config::LoadInt(L"window_w.txt", 0, 100, 100000), 0);
    EXPECT_EQ(config::LoadInt(L"window_h.txt", 0, 100, 100000), 0);
}

// ---- スクロール復元設定テスト ----

TEST_F(ConfigStoreTest, ScrollNodeRoundTrip) {
    config::SaveInt(L"scroll_node.txt", 42);
    config::SaveInt(L"scroll_offset.txt", 15);

    EXPECT_EQ(config::LoadInt(L"scroll_node.txt", -1, -1, 100000000), 42);
    EXPECT_EQ(config::LoadInt(L"scroll_offset.txt", 0, -100000, 100000), 15);
}

TEST_F(ConfigStoreTest, ScrollNodeMissingReturnsDefault) {
    // 保存済みスクロール位置なし: ノード=-1でスキップされる
    EXPECT_EQ(config::LoadInt(L"scroll_node.txt", -1, -1, 100000000), -1);
    EXPECT_EQ(config::LoadInt(L"scroll_offset.txt", 0, -100000, 100000), 0);
}

TEST_F(ConfigStoreTest, ScrollNodeNegativeOffset) {
    // ノード間のスペーシング領域にスクロール位置がある場合
    config::SaveInt(L"scroll_node.txt", 5);
    config::SaveInt(L"scroll_offset.txt", -10);

    EXPECT_EQ(config::LoadInt(L"scroll_node.txt", -1, -1, 100000000), 5);
    EXPECT_EQ(config::LoadInt(L"scroll_offset.txt", 0, -100000, 100000), -10);
}
