#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <d2d1.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <windows.h>
#include <vector>
#include "document_types.h"
#include "layout_cache.h"
#include "task_scheduler.h"
#include "test_nodes.h"
#include "theme.h"

// 重なりを許さず needle の出現回数を数える。
inline size_t CountOccurrences(std::string_view s, std::string_view needle)
{
    size_t n = 0;
    for (size_t pos = s.find(needle); pos != std::string_view::npos; pos = s.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// line_count を内容から数える。
inline void SetNodeTextCounted(Node& n, std::string_view sv)
{
    const int32_t lc = static_cast<int32_t>(std::ranges::count(sv, '\n'));
    SetTestText(n, sv, lc);
}

// テキスト持ちの Paragraph ノードを作るテスト用ヘルパー。
inline Node MakeTextNode(const char* text)
{
    Node n;
    n.type = NodeType::Paragraph;
    SetNodeTextCounted(n, std::string_view{ text });
    return n;
}

inline std::vector<Node> TextNodes(std::initializer_list<const char*> texts)
{
    std::vector<Node> nodes;
    for (const char* text : texts) {
        nodes.push_back(MakeTextNode(text));
    }
    return nodes;
}

// パーサが区切ったテキストノードの並び。newline_before なら継ぎ目に改行を挟む。
inline std::vector<Node> ChunkedTextNodes(std::initializer_list<const char*> texts, bool newline_before = false)
{
    auto nodes = TextNodes(texts);
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        nodes[i].set_continues_next();
        nodes[i + 1].set_continues_prev(newline_before);
    }
    return nodes;
}

// rows[r][c] をセルに持つテーブルノード。区切りはパーサと同じく行内 '\t'、行間 '\n'。
inline Node MakeTableNode(std::initializer_list<std::initializer_list<std::string_view>> rows)
{
    Node n;
    n.type = NodeType::Table;
    auto* tbl = AttachTestTable(n);
    tbl->row_count = static_cast<uint32_t>(rows.size());
    tbl->col_count = static_cast<uint16_t>(rows.begin()->size());
    for (const auto& row : rows) {
        if (!tbl->cell_text_starts.empty()) {
            tbl->concat_text.push_back('\n');
        }
        for (size_t c = 0; c < row.size(); ++c) {
            if (c > 0) {
                tbl->concat_text.push_back('\t');
            }
            tbl->cell_text_starts.push_back(static_cast<uint32_t>(tbl->concat_text.size()));
            tbl->cell_run_starts.push_back(0);
            tbl->concat_text.append(row.begin()[c]);
        }
    }
    tbl->cell_text_starts.push_back(static_cast<uint32_t>(tbl->concat_text.size()));
    tbl->cell_run_starts.push_back(0);
    tbl->aligns.assign(tbl->col_count, TableAlign::Default);
    n.SetText(tbl->concat_text);
    return n;
}

// 等間隔ノードの LayoutCache を構築するテスト用ヘルパー。
// spacing_above = spacing_below = 0 を仮定する。
inline LayoutCache MakeUniformCache(int count, float node_height = 100.0f)
{
    LayoutCache cache(count);
    float y = 0.0f;
    for (int i = 0; i < count; ++i) {
        cache.SetTop(i, y);
        cache[i].height = node_height;
        y += node_height;
    }
    return cache;
}

inline size_t CountDirty(const LayoutCache& cache) noexcept
{
    size_t n = 0;
    for (size_t i = 0; i < cache.size(); ++i) {
        n += cache[i].layout_dirty ? 1 : 0;
    }
    return n;
}

// 22000 ノード級の Mixed fixture を決定論的に生成する。配分:
// Paragraph 60% / Heading 10% / HorizontalRule 5% / Table(1x2) 10% / Image 10% / 空 Paragraph 5%
// include_code_block=true の場合は bucket==0,1 を CodeBlock(Cpp) に置換し Paragraph を 50% に減らす。
inline Node MakeStressNode(size_t i, bool include_code_block)
{
    Node n;
    // 派生 mock のトークン seed として使うため、必ず source_offset を埋める。
    n.SetSourceOffset(i);
    const size_t bucket = i % 20;

    if (include_code_block && bucket < 2) {
        n.type = NodeType::CodeBlock;
        n.code_language = SyntaxLanguage::Cpp;
        std::string text = "int v_" + std::to_string(i) + " = 0;";
        SetTestText(n, text, 0);
        return n;
    }
    if (bucket < 12) {
        n.type = NodeType::Paragraph;
        const size_t len = 5 + (i % 80);
        std::string text;
        text.reserve(len);
        for (size_t k = 0; k < len; ++k) {
            text.push_back(static_cast<char>('a' + ((i + k) % 26)));
        }
        SetTestText(n, text, 0);
        return n;
    }
    if (bucket < 14) {
        n.type = NodeType::Heading;
        n.heading_level = static_cast<int8_t>(1 + (i % 6));
        std::string text = "Heading " + std::to_string(i);
        SetTestText(n, text, 0);
        return n;
    }
    if (bucket == 14) {
        n.type = NodeType::HorizontalRule;
        return n;
    }
    if (bucket < 17) {
        Node table = MakeTableNode({ { "a", "b" } });
        table.SetSourceOffset(i);
        return table;
    }
    if (bucket < 19) {
        static constexpr std::string_view kImageSrc = "img.png";
        n.type = NodeType::Image;
        n.set_image_src(&kImageSrc);
        return n;
    }
    n.type = NodeType::Paragraph;
    n.SetText("");
    return n;
}

struct StressFixture {
    std::vector<Node> nodes;
    LayoutCache cache;

    void Build(size_t n, bool include_code_block)
    {
        nodes.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            nodes.push_back(MakeStressNode(i, include_code_block));
        }
        cache = MakeUniformCache(static_cast<int>(n), 80.0f);
        for (size_t i = 0; i < n; ++i) {
            if (nodes[i].type == NodeType::Image) {
                auto& d = cache.EnsureDiagram(i);
                d.width = 100.0f + static_cast<float>(i % 50);
                d.height = 50.0f + static_cast<float>(i % 30);
            }
        }
    }
};

// レイアウト系テスト共通の Theme。
inline Theme MakeLayoutTestTheme()
{
    Theme theme;
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

// content をバイナリで書き出す。失敗はテスト失敗として報告する。
inline void WriteTestFile(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        ADD_FAILURE() << "Failed to open temp file: " << path.string();
        return;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f.good()) {
        ADD_FAILURE() << "Failed to write temp file: " << path.string();
    }
}

// temp ディレクトリを RAII で用意・破棄する。テスト名とプロセスIDで一意な名前を生成するため、
// 並列実行や異常終了時も衝突しない。gtest はフィクスチャ生成前に current_test_info を
// 設定するので、フィクスチャのメンバとして保持できる。
class ScopedTempDir {
public:
    ScopedTempDir()
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
        // 値パラメータ化テスト名の '/' でネストしたディレクトリにならないようにする
        std::ranges::replace(name, '/', '_');
        path_ = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        // PID 再利用時に前回の異常終了の残骸を引き継がない
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            ADD_FAILURE() << "Failed to create temp dir: " << path_.string() << ": " << ec.message();
        }
    }
    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    // バイナリで配下にファイルを作成し、絶対パスを返す。
    std::filesystem::path Write(std::wstring_view name, std::string_view content = {}) const
    {
        auto path = path_ / name;
        WriteTestFile(path, content);
        return path;
    }

private:
    std::filesystem::path path_;
};

// テストケースごとに temp ディレクトリを自動で用意・破棄するフィクスチャ基底。
// ディレクトリは派生クラスのメンバ破棄後に消えるため、ファイルを開いたままのメンバがいても安全。
class TempDirTestBase : public ::testing::Test {
private:
    ScopedTempDir scoped_temp_dir_;

protected:
    const std::filesystem::path temp_dir_ = scoped_temp_dir_.path();

    std::filesystem::path WriteTempFile(std::wstring_view name, std::string_view content = {}) const
    {
        return scoped_temp_dir_.Write(name, content);
    }
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

// worker を workers 本塞ぎ、以降に積んだタスクを待たせる。Open で走り出す。破棄時にも開くので、ASSERT で抜けても
// 後続テストを道連れにしない。タスクが待ちから抜けるのは Open より後になりうるので、状態はタスクと共有する。
class WorkerGate {
public:
    explicit WorkerGate(TaskScheduler& scheduler, int workers = 1)
    {
        for (int i = 0; i < workers; ++i) {
            const bool posted = scheduler.Post([state = state_] {
                state->entered.fetch_add(1);
                state->entered.notify_all();
                state->open.wait(false);
            });
            EXPECT_TRUE(posted);
            if (!posted) {
                return;
            }
        }
        for (int n = state_->entered.load(); n < workers; n = state_->entered.load()) {
            state_->entered.wait(n);
        }
    }
    ~WorkerGate()
    {
        Open();
    }
    WorkerGate(const WorkerGate&) = delete;
    WorkerGate& operator=(const WorkerGate&) = delete;

    void Open() noexcept
    {
        state_->open.store(true);
        state_->open.notify_all();
    }

private:
    struct State {
        std::atomic<int> entered{ 0 };
        std::atomic<bool> open{ false };
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// 同期キャッシュヒットをシミュレートするための最小 ID2D1Bitmap スタブ。
// AddRef/Release は no-op (static instance で寿命管理は不要)。
class StubD2D1Bitmap : public ID2D1Bitmap {
public:
    STDMETHODIMP QueryInterface(REFIID, void**) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return 1;
    }
    STDMETHODIMP_(ULONG)
    Release() override
    {
        return 1;
    }
    void STDMETHODCALLTYPE GetFactory(ID2D1Factory** factory) const override
    {
        if (factory) {
            *factory = nullptr;
        }
    }
    D2D1_SIZE_F STDMETHODCALLTYPE GetSize() const override
    {
        return D2D1_SIZE_F{ 1.0f, 1.0f };
    }
    D2D1_SIZE_U STDMETHODCALLTYPE GetPixelSize() const override
    {
        return D2D1_SIZE_U{ 1u, 1u };
    }
    D2D1_PIXEL_FORMAT STDMETHODCALLTYPE GetPixelFormat() const override
    {
        return D2D1_PIXEL_FORMAT{};
    }
    void STDMETHODCALLTYPE GetDpi(FLOAT* dpiX, FLOAT* dpiY) const override
    {
        if (dpiX) {
            *dpiX = 96.0f;
        }
        if (dpiY) {
            *dpiY = 96.0f;
        }
    }
    HRESULT STDMETHODCALLTYPE CopyFromBitmap(const D2D1_POINT_2U*, ID2D1Bitmap*, const D2D1_RECT_U*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CopyFromRenderTarget(const D2D1_POINT_2U*, ID2D1RenderTarget*, const D2D1_RECT_U*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CopyFromMemory(const D2D1_RECT_U*, const void*, UINT32) override
    {
        return E_NOTIMPL;
    }
};

// AddRef/Release が何もしないので static のまま共用する。
inline StubD2D1Bitmap& SharedStubBitmap() noexcept
{
    static StubD2D1Bitmap instance;
    return instance;
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
