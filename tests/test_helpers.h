#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <d2d1.h>
#include <dwrite.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <windows.h>
#include <wrl/client.h>
#include <vector>
#include "document_types.h"
#include "i18n.h"
#include "layout_cache.h"
#include "task_scheduler.h"
#include "test_nodes.h"
#include "theme.h"
#include "ui_types.h"

// 区切って進める処理 (ContinueSearch など) を、最初の区切りで止める期限。
inline constexpr auto kExpiredDeadline = std::chrono::steady_clock::time_point::min();
// 区切らずに最後まで進める期限。
inline constexpr auto kNoDeadline = std::chrono::steady_clock::time_point::max();

// 表示言語を切り替え、抜けるときに表示中の言語と選んだ言語を元に戻す。ASSERT で抜けても後続のテストへ漏らさない。
// 既定の構築は切り替えずに戻すだけ (テストの中で Init や SetLanguage を呼ぶフィクスチャ用)。
class ScopedLanguage {
public:
    ScopedLanguage() = default;
    explicit ScopedLanguage(const i18n::Strings* language) noexcept
    {
        i18n::SetLanguage(language);
    }
    ~ScopedLanguage()
    {
        i18n::g_strings.store(saved_strings_);
        i18n::g_chosen.store(saved_chosen_);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;

private:
    const i18n::Strings* saved_strings_ = i18n::g_strings.load();
    const i18n::Strings* saved_chosen_ = i18n::g_chosen.load();
};

// prefix の後に、全体が min_bytes 以上になるまで unit を繰り返す。
inline std::string RepeatTo(std::string_view unit, size_t min_bytes, std::string prefix = {})
{
    prefix.reserve(min_bytes + unit.size());
    while (prefix.size() < min_bytes) {
        prefix += unit;
    }
    return prefix;
}

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
    auto& text = tbl->own_text;
    for (const auto& row : rows) {
        if (!tbl->cell_text_starts.empty()) {
            text.push_back('\n');
        }
        for (size_t c = 0; c < row.size(); ++c) {
            if (c > 0) {
                text.push_back('\t');
            }
            tbl->cell_text_starts.push_back(static_cast<uint32_t>(text.size()));
            tbl->cell_run_starts.push_back(0);
            text.append(row.begin()[c]);
        }
    }
    tbl->cell_text_starts.push_back(static_cast<uint32_t>(text.size()));
    tbl->concat_text = text;
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

// 並列計測の stress 用の Mixed fixture を決定論的に生成する。配分:
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
        n.type = NodeType::Diagram;
        n.code_language = SyntaxLanguage::Mermaid;
        SetTestText(n, "graph TD; A-->B", 0);
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
            if (nodes[i].type == NodeType::Diagram) {
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

    // バイナリで配下にファイルを作成し、絶対パスを返す。失敗はテスト失敗として報告する。
    std::filesystem::path Write(std::wstring_view name, std::string_view content = {}) const
    {
        auto path = path_ / name;
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
// sleep_for は短く指定してもタイマ分解能 (約 15.6ms) まで寝るので、ふつうの待ちが収まる間は yield で回す。
template <class Pred>
inline bool PollUntil(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + timeout;
    const auto spin_until = start + std::chrono::milliseconds(50);
    while (true) {
        if (pred()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        if (now < spin_until) {
            std::this_thread::yield();
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// worker を workers 本塞ぎ、以降に積んだタスクを待たせる。Open で走り出す。破棄時にも開くので、ASSERT で抜けても
// 後続テストを道連れにしない。タスクが待ちから抜けるのは Open より後になりうるので、状態はタスクと共有する。
class WorkerGate {
public:
    explicit WorkerGate(TaskScheduler& scheduler, int workers = 1, std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        int posted = 0;
        while (posted < workers && scheduler.Post([state = state_, workers] {
            std::unique_lock lock(state->mutex);
            if (++state->entered == workers) {
                state->cv.notify_all();
            }
            state->cv.wait(lock, [&] { return state->open; });
        })) {
            ++posted;
        }
        std::unique_lock lock(state_->mutex);
        ready_ = posted == workers && state_->cv.wait_for(lock, timeout, [&] { return state_->entered == workers; });
        EXPECT_TRUE(ready_) << "worker を塞げなかった (積めない・時間切れ)";
    }
    ~WorkerGate()
    {
        Open();
    }
    WorkerGate(const WorkerGate&) = delete;
    WorkerGate& operator=(const WorkerGate&) = delete;

    // 塞ぐ worker がすべて待ちに入ったか。
    bool ready() const noexcept
    {
        return ready_;
    }
    void Open()
    {
        const std::lock_guard lock(state_->mutex);
        state_->open = true;
        state_->cv.notify_all();
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        int entered = 0;
        bool open = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    bool ready_ = false;
};

// 呼ぶ前に積んだタスク (PostFront も含む) がすべて終わるまで待つ。時間切れや Shutdown 後は false。
// 全 worker を塞げた時点で、キューの先頭から取るのでそれより前のタスクはどの worker でも終わっている。
// 1 本ずつ塞いで返すと、空いた worker が残りを拾い、別の worker が走らせているタスクを見逃す。
// 実行中のタスクが後から Post した続きは待たない。
[[nodiscard]] inline bool DrainScheduler(TaskScheduler& scheduler)
{
    const WorkerGate gate(scheduler, static_cast<int>(scheduler.WorkerCount()));
    return scheduler.WorkerCount() > 0 && gate.ready();
}

// 読み込んだ画像を装う最小 ID2D1Bitmap スタブ。描画はしない。
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

// 描き終えた図 (DiagramResult::commands) を装う最小 ID2D1CommandList スタブ。描画はしない。
class StubD2D1CommandList : public ID2D1CommandList {
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
    HRESULT STDMETHODCALLTYPE Stream(ID2D1CommandSink*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Close() override
    {
        return S_OK;
    }
};

inline StubD2D1CommandList& SharedStubCommandList() noexcept
{
    static StubD2D1CommandList instance;
    return instance;
}

// DWRITE_FACTORY_TYPE_SHARED はプロセスで 1 つなので、テストごとに作らず引き回す。作れなければ null。
inline IDWriteFactory* SharedDWriteFactory()
{
    static const Microsoft::WRL::ComPtr<IDWriteFactory> factory = [] {
        Microsoft::WRL::ComPtr<IDWriteFactory> created;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(created.GetAddressOf()));
        return created;
    }();
    return factory.Get();
}

// worker から PostMessage される HWND の提供と、届いた通知の計数に使う。
// HWND_MESSAGE 親で可視化されず、メッセージループ不要 (作成スレッドのキューに積まれる)。
class MessageOnlyWindow {
public:
    MessageOnlyWindow()
    {
        const wchar_t* class_name = L"MendoTestMsgWnd";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = class_name;
        ::RegisterClassExW(&wc);
        hwnd_ = ::CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }
    ~MessageOnlyWindow()
    {
        if (hwnd_) {
            ::DestroyWindow(hwnd_);
        }
    }
    MessageOnlyWindow(const MessageOnlyWindow&) = delete;
    MessageOnlyWindow& operator=(const MessageOnlyWindow&) = delete;

    HWND Get() const noexcept
    {
        return hwnd_;
    }

    // キューに溜まった msg_id を取り出して数える。
    int DrainMessages(UINT msg_id) const noexcept
    {
        int n = 0;
        MSG m;
        while (::PeekMessageW(&m, hwnd_, msg_id, msg_id, PM_REMOVE)) {
            ++n;
        }
        return n;
    }

private:
    HWND hwnd_ = nullptr;
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
