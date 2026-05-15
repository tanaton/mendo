#pragma once
#include <gtest/gtest.h>
#include <chrono>
#include <d2d1.h>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <windows.h>
#include "document_types.h"
#include "layout_cache.h"
#include "side_effect.h"

// 同色判定。完全一致のみ（テストで使う色はテーマ定数なので浮動小数点誤差は問題にならない）。
constexpr bool ColorEq(D2D1_COLOR_F a, D2D1_COLOR_F b) noexcept
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// テキスト持ちの Paragraph ノードを作るテスト用ヘルパー。
inline Node MakeTextNode(const char* text)
{
    Node n;
    n.type = NodeType::Paragraph;
    n.SetText(text);
    return n;
}

// 1 行 2 列のテーブルノードを作るテスト用ヘルパー。
inline Node MakeTableNode(const char* cell0, const char* cell1)
{
    Node n;
    n.type = NodeType::Table;
    n.ensure_table();
    auto* tbl = n.table_data();
    tbl->row_count = 1;
    tbl->col_count = 2;
    tbl->concat_text.append(cell0);
    tbl->concat_text.push_back(mendo::doc_tab);
    tbl->concat_text.append(cell1);
    tbl->cell_text_starts.push_back(0);
    tbl->cell_text_starts.push_back(static_cast<uint32_t>(std::char_traits<char>::length(cell0) + 1));
    tbl->cell_text_starts.push_back(static_cast<uint32_t>(tbl->concat_text.size()));
    tbl->cell_run_starts.push_back(0);
    tbl->cell_run_starts.push_back(0);
    tbl->cell_run_starts.push_back(0);
    tbl->aligns.push_back(TableAlign::Default);
    tbl->aligns.push_back(TableAlign::Default);
    tbl->is_header_row.push_back(false);
    return n;
}

// 等間隔ノードの LayoutCache を構築するテスト用ヘルパー。
// 複数テストファイルで共通して使用される。
// spacing_above = spacing_below = 0 を仮定し、block_height = node_height で Fenwick も同期する。
inline LayoutCache MakeUniformCache(int count, float node_height = 100.0f)
{
    LayoutCache cache;
    cache.Resize(count);
    std::pmr::vector<float> block_heights;
    block_heights.reserve(count);
    float y = 0.0f;
    for (int i = 0; i < count; ++i) {
        cache[i].text_top = y;
        cache[i].height = node_height;
        block_heights.push_back(node_height);
        y += node_height;
    }
    if (count > 0) {
        cache.BuildBlockHeights(std::span<const float>(block_heights.data(), block_heights.size()));
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

// SideEffectList の中で型 T が最初に現れる位置を返す。
// 型 T はいずれかのドメイン variant のメンバである必要がある。
template <typename T>
inline std::optional<size_t> IndexOfEffect(const SideEffectList& effects) noexcept
{
    for (size_t i = 0; i < effects.size(); ++i) {
        if (GetEffect<T>(effects[i]) != nullptr) {
            return i;
        }
    }
    return std::nullopt;
}

enum class EffectOrdering {
    Before, // A が先、B が後
    After,  // B が先、A が後
    OnlyA,  // A だけ存在
    OnlyB,  // B だけ存在
    Neither // どちらも無し
};

// effects 内で A が B より先に現れるか判定する。
// 順序の意味づけが分岐ごとに違う場合があるので、Before / After 以外も
// 失敗時に何が起きたかを呼び出し側が EXPECT_EQ で識別できるよう列挙する。
template <typename A, typename B>
inline EffectOrdering EffectOrder(const SideEffectList& effects) noexcept
{
    const auto a = IndexOfEffect<A>(effects);
    const auto b = IndexOfEffect<B>(effects);
    if (!a && !b) {
        return EffectOrdering::Neither;
    }
    if (a && !b) {
        return EffectOrdering::OnlyA;
    }
    if (!a && b) {
        return EffectOrdering::OnlyB;
    }
    return *a < *b ? EffectOrdering::Before : EffectOrdering::After;
}

// effects 内で A が存在し、かつ B が存在し、A が B より先に現れる場合のみ true。
template <typename A, typename B>
inline bool HasEffectInOrder(const SideEffectList& effects) noexcept
{
    return EffectOrder<A, B>(effects) == EffectOrdering::Before;
}

// プロセス内一意の一時ファイルを RAII で作成・削除する。
// jthread / 非同期ロード系テストで「実ファイルを 1 つ」だけ要求するケースに使う。
// 大量ファイルや subdir 構造が要るテストは TempDirTestBase を使うこと。
class TempFile {
public:
    TempFile(std::wstring_view name_hint, std::string_view content)
    {
        const auto tmp_dir = std::filesystem::temp_directory_path();
        std::wstring fname = L"mendo_";
        fname.append(name_hint);
        fname += L"_";
        fname += std::to_wstring(::GetCurrentProcessId());
        fname += L"_";
        fname += std::to_wstring(reinterpret_cast<uintptr_t>(this));
        path_ = tmp_dir / fname;
        std::ofstream(path_, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::pmr::wstring PmrPath() const
    {
        return std::pmr::wstring(path_.wstring());
    }

private:
    std::filesystem::path path_;
};

// 条件 pred が true になるまで短時間ポーリングする。timeout 内に成立すれば true。
// バックグラウンドワーカー完了の決定論的観測手段が無いケースの最終手段。
template <class Pred>
inline bool PollUntil(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

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
