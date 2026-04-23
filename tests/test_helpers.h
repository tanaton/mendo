#pragma once
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <windows.h>
#include "layout_cache.h"

// 等間隔ノードの LayoutCache を構築するテスト用ヘルパー。
// 複数テストファイルで共通して使用される。
inline LayoutCache MakeUniformCache(int count, float node_height = 100.0f)
{
    LayoutCache cache;
    cache.Resize(count);
    float y = 0.0f;
    for (int i = 0; i < count; ++i) {
        cache[i].y_position = y;
        cache[i].height = node_height;
        y += node_height;
    }
    return cache;
}

// テストケースごとに temp ディレクトリを自動で用意・破棄するフィクスチャ基底。
// テスト名とプロセスIDで一意な名前を生成するため、並列実行や異常終了時も衝突しない。
class TempDirTestBase : public ::testing::Test {
protected:
    std::filesystem::path temp_dir_;

    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = "mendo_test_";
        if (info) {
            name += info->test_suite_name();
            name += "_";
            name += info->name();
            name += "_";
        }
        name += std::to_string(::GetCurrentProcessId());
        temp_dir_ = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    // バイナリで temp_dir_ 配下にファイルを作成し、絶対パスを返す。
    std::filesystem::path WriteTempFile(std::wstring_view name, std::string_view content) const
    {
        auto path = temp_dir_ / name;
        std::ofstream f(path, std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        return path;
    }
};
