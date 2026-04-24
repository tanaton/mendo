#pragma once
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <windows.h>
#include "document_types.h"
#include "layout_cache.h"

// テキスト持ちの Paragraph ノードを作るテスト用ヘルパー。
inline Node MakeTextNode(const wchar_t* text)
{
    Node n;
    n.type = NodeType::Paragraph;
    n.SetText(text);
    return n;
}

// 1行2列のテーブルノードを作るテスト用ヘルパー。
inline Node MakeTableNode(const wchar_t* cell0, const wchar_t* cell1)
{
    Node n;
    n.type = NodeType::Table;
    n.ensure_table();
    TableRow row;
    TableCell c0;
    c0.text.assign(cell0);
    row.cells.push_back(std::move(c0));
    TableCell c1;
    c1.text.assign(cell1);
    row.cells.push_back(std::move(c1));
    n.table_data->rows.push_back(std::move(row));
    return n;
}

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
        if (!f.is_open()) {
            ADD_FAILURE() << "Failed to open temp file: " << path.string();
            return path;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f.good()) {
            ADD_FAILURE() << "Failed to write temp file: " << path.string();
        }
        return path;
    }
};

// COM apartment 初期化を管理する基底フィクスチャ。
// 既に他スイートで別モードで初期化済みのケース (RPC_E_CHANGED_MODE) では
// CoUninitialize を呼ばずに既存 apartment のカウントを保つ — そうしないと
// 初期化していないスイートが他スイートの COM 状態を破壊する。
class ComApartmentTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        co_init_hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(co_init_hr_) || co_init_hr_ == RPC_E_CHANGED_MODE)
            << "CoInitializeEx failed: 0x" << std::hex << co_init_hr_;
    }

    static void TearDownTestSuite()
    {
        if (SUCCEEDED(co_init_hr_)) {
            CoUninitialize();
        }
    }

private:
    static inline HRESULT co_init_hr_ = E_UNEXPECTED;
};
