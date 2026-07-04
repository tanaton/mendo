#pragma once
#include <gtest/gtest.h>
#include <algorithm>
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
#include <vector>
#include "document_types.h"
#include "layout_cache.h"
#include "side_effect.h"
#include "theme.h"

// Node::view_ は実バッファへの非 null ポインタを必要とする。テストでは中身が
// 任意で十分大きい固定 base を返し、SetSourceOffset(base, offset) と SourceOffsetFrom(base)
// のラウンドトリップ用に使う。サイズ 32KB は stress fixture の 22000 ノード + 余裕。
inline const char* SourceOffsetTestBase() noexcept
{
    static const std::vector<char> buf(32 * 1024);
    return buf.data();
}

// 同色判定。完全一致のみ（テストで使う色はテーマ定数なので浮動小数点誤差は問題にならない）。
constexpr bool ColorEq(D2D1_COLOR_F a, D2D1_COLOR_F b) noexcept
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// 本番 API SetTextWithLineCount は line_count を呼び出し側に渡させる契約だが、
// 任意の文字列を受け取るテストヘルパーでは内容から自動算出したい。
inline void SetNodeTextCounted(Node& n, std::string_view sv)
{
    const int32_t lc = static_cast<int32_t>(std::ranges::count(sv, mendo::doc_lf));
    n.SetTextWithLineCount(sv, lc);
}

// テキスト持ちの Paragraph ノードを作るテスト用ヘルパー。
inline Node MakeTextNode(const char* text)
{
    Node n;
    n.type = NodeType::Paragraph;
    SetNodeTextCounted(n, std::string_view{ text });
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

// レイアウト系テスト共通の Theme。Theme は zoom 以外初期化子なしの集約のため、
// value-init + 明示設定で未初期化読み（flaky の温床）を防ぐ。
inline Theme MakeLayoutTestTheme()
{
    Theme theme{};
    theme.margin_top = 10.0f;
    theme.heading_spacing_above = 8.0f;
    theme.heading_spacing_below = 4.0f;
    theme.heading_spacing_below_h1h2 = 6.0f;
    theme.code_block_spacing_above = 12.0f;
    theme.paragraph_spacing = 5.0f;
    theme.font_size_body = 14.0f;
    theme.font_size_code = 12.0f;
    for (int i = 0; i < 6; ++i) {
        theme.font_size_h[i] = 18.0f - static_cast<float>(i);
    }
    theme.list_item_spacing = 3.0f;
    theme.code_block_padding = 4.0f;
    theme.indent_width = 16.0f;
    theme.hr_thickness = 1.0f;
    return theme;
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

// SideEffectList の中で型 T が最初に現れる要素を返す。無ければ nullptr。
// フィールド値の検証 (HasEffect では型の有無しか見られない) に使う。
template <typename T>
inline const T* FindEffect(const SideEffectList& effects) noexcept
{
    for (const auto& se : effects) {
        if (const T* p = GetEffect<T>(se)) {
            return p;
        }
    }
    return nullptr;
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
        std::ofstream f(path_, std::ios::binary);
        if (!f.is_open()) {
            ADD_FAILURE() << "Failed to open temp file: " << path_.string();
            return;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f.good()) {
            ADD_FAILURE() << "Failed to write temp file: " << path_.string();
        }
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
