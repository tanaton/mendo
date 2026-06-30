#include <gtest/gtest.h>
#include "clipboard_manager.h"
#include "document.h"
#include "document_test_helpers.h"
#include "mermaid_renderer_interface.h"
#include <atomic>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace {

// SVG リクエスト引数とコールバックを記録するモック。
// MermaidFileCache 系は HWND/TaskScheduler 依存のため触らない経路だけ踏む。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    void RequestRender(Node&, NodeLayoutEntry&, DiagramEntry&, float, bool, Callback) override
    {}
    void CancelPending() override
    {}
    void ClearCache() override
    {}

    void RequestSvg(std::wstring_view code, float max_width, bool dark_mode, SvgCallback cb) override
    {
        ++request_count;
        last_code.assign(code);
        last_width = max_width;
        last_dark = dark_mode;
        last_callback = std::move(cb);
    }

    void FireCallback(std::pmr::wstring svg, bool cancelled)
    {
        if (last_callback) {
            // SvgCallback は move_only_function なので 1 回実行後は破棄。
            auto cb = std::move(last_callback);
            last_callback = {};
            cb(std::move(svg), cancelled);
        }
    }

    int request_count = 0;
    std::pmr::wstring last_code;
    float last_width = 0.0f;
    bool last_dark = false;
    SvgCallback last_callback;
};

struct ToastRecorder {
    std::vector<std::wstring> messages;

    ClipboardManager::ToastCallback Callback()
    {
        return [this](std::wstring_view m) { messages.emplace_back(m); };
    }
};

ClipboardManager MakeManager(MockMermaidRenderer* renderer, ToastRecorder& toast)
{
    ClipboardManager m;
    // HWND/WIC/file_cache nullptr は production では成立しないが、ガード経路の単体テストに限り許容。
    // png 無し (nullptr) のため BuildDib は空を返し、クリップボード書き込みは行われない。
    m.Init(nullptr, nullptr, renderer, nullptr, toast.Callback());
    return m;
}

constexpr float kDefaultMdWidth = 800.0f;

// テストは png 無しでガード経路のみ踏むため、共通の空 PNG ハンドルを使う。
const std::shared_ptr<const std::pmr::vector<uint8_t>> kNoPng;

} // namespace

// ---- CopyCodeBlock 入力検証 ----

TEST(ClipboardManager, CopyCodeBlockNoOpForOutOfRangeIndex)
{
    auto doc = Document::FromMarkdown("para", L"t.md");
    ToastRecorder toast;
    auto m = MakeManager(nullptr, toast);
    m.CopyCodeBlock(doc, -1, false);
    m.CopyCodeBlock(doc, 9999, false);
    EXPECT_TRUE(toast.messages.empty());
}

TEST(ClipboardManager, CopyCodeBlockNoOpForNonCodeBlock)
{
    auto doc = Document::FromMarkdown("paragraph text", L"t.md");
    ToastRecorder toast;
    auto m = MakeManager(nullptr, toast);
    m.CopyCodeBlock(doc, 0, false);
    EXPECT_TRUE(toast.messages.empty());
}

// ---- SaveDiagramAsPng 入力検証 ----

TEST(ClipboardManager, SaveDiagramAsPngNoOpForOutOfRangeIndex)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    ToastRecorder toast;
    auto m = MakeManager(nullptr, toast);
    m.SaveDiagramAsPng(doc, -1, kDefaultMdWidth, false);
    m.SaveDiagramAsPng(doc, 9999, kDefaultMdWidth, false);
    EXPECT_TRUE(toast.messages.empty());
}

TEST(ClipboardManager, SaveDiagramAsPngNoOpForNonDiagramLanguage)
{
    auto doc = Document::FromMarkdown("```cpp\nint main(){}\n```\n", L"t.md");
    ToastRecorder toast;
    auto m = MakeManager(nullptr, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);
    m.SaveDiagramAsPng(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_TRUE(toast.messages.empty());
}

TEST(ClipboardManager, SaveDiagramAsPngNoOpWhenFileCacheNullptr)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    ToastRecorder toast;
    auto m = MakeManager(nullptr, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);
    m.SaveDiagramAsPng(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_TRUE(toast.messages.empty());
}

// ---- CopyDiagramToClipboard 入力検証 ----

TEST(ClipboardManager, CopyDiagramToClipboardNoOpForOutOfRangeIndex)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    m.CopyDiagramToClipboard(doc, -1, kNoPng, kDefaultMdWidth, false);
    m.CopyDiagramToClipboard(doc, 9999, kNoPng, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 0);
    EXPECT_TRUE(toast.messages.empty());
}

TEST(ClipboardManager, CopyDiagramToClipboardLatexMathCopiesImageWithoutRenderer)
{
    // LatexMath は IsSvgExportable=false。画像 (CF_DIB) のみを同期コピーし、SVG レンダラは呼ばない。
    // LatexMath は $$..$$ 段落の昇格でのみ生成される (parser.cpp)。
    // png 無しのため画像構築は失敗し、同期パスは結果トースト (失敗) を 1 回出す。
    auto doc = Document::FromMarkdown("$$x^2$$\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);
    ASSERT_EQ(doc.GetNodes()[cb_idx].code_language(), SyntaxLanguage::LatexMath);
    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 0); // SVG レンダラには行かない
    EXPECT_EQ(toast.messages.size(), 1u); // 同期パスが結果を報告する (旧実装の no-op ではない)
}

TEST(ClipboardManager, CopyDiagramToClipboardNoOpWhenRendererNullptr)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    ToastRecorder toast;
    auto m = MakeManager(nullptr, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);
    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    EXPECT_TRUE(toast.messages.empty());
}

// ---- CopyDiagramToClipboard 主要パス ----

TEST(ClipboardManager, CopyDiagramToClipboardRequestsRendererForMermaid)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 1);
    ASSERT_FALSE(toast.messages.empty());
    EXPECT_FLOAT_EQ(renderer.last_width, kDefaultMdWidth);
    EXPECT_FALSE(renderer.last_dark);
}

TEST(ClipboardManager, CopyDiagramToClipboardInFlightGuardBlocksReentry)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 1);
    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 1);

    // cancelled 経路で in-flight だけ解除 (svg_cache に何も入れない)。
    renderer.FireCallback({}, true);
    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 2);
}

TEST(ClipboardManager, CopyDiagramToClipboardInFlightReentryDoesNotStaleInProgress)
{
    // in-flight 中の再入(2回目)で世代を進めてしまうと、進行中リクエストのコールバックが
    // stale 扱いになり結果(トースト)が出ずに終わる退行を防ぐ。
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false); // request 1 (gen 捕捉)
    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false); // in-flight で弾く (世代は進めない)
    EXPECT_EQ(renderer.request_count, 1);

    const auto before = toast.messages.size();
    renderer.FireCallback({}, false); // request 1 完了。stale でないので結果トーストを出す
    EXPECT_EQ(toast.messages.size(), before + 1);
}

TEST(ClipboardManager, CopyDiagramToClipboardCancelledCallbackEmitsNoToast)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    const auto toast_count_before = toast.messages.size();
    renderer.FireCallback({}, true);
    EXPECT_EQ(toast.messages.size(), toast_count_before);
    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 2);
}

TEST(ClipboardManager, CopyDiagramToClipboardEmptyResultEmitsFailToast)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, kDefaultMdWidth, false);
    const auto before = toast.messages.size();
    renderer.FireCallback({}, false);
    EXPECT_EQ(toast.messages.size(), before + 1);
}

TEST(ClipboardManager, CopyDiagramToClipboardPassesCodeAndDarkModeToRenderer)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph LR; X-->Y\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramToClipboard(doc, cb_idx, kNoPng, 1024.0f, true);
    EXPECT_EQ(renderer.request_count, 1);
    EXPECT_FLOAT_EQ(renderer.last_width, 1024.0f);
    EXPECT_TRUE(renderer.last_dark);
    EXPECT_NE(renderer.last_code.find(L"graph LR"), std::wstring_view::npos);

    renderer.FireCallback({}, true);
}
