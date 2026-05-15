#include <gtest/gtest.h>
#include "clipboard_manager.h"
#include "document.h"
#include "document_test_helpers.h"
#include "mermaid_renderer_interface.h"
#include <atomic>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace {

// SVG リクエスト引数とコールバックを記録するモック。
// MermaidFileCache 系は HWND/TaskScheduler 依存のため触らない経路だけ踏む。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    void RequestRender(Node&, NodeLayoutEntry&, DiagramEntry&, float, bool, Callback) override {}
    void CancelPending() override {}
    void ClearCache() override {}

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
    // HWND nullptr は production では成立しないが、ガード経路の単体テストに限り許容。
    m.Init(nullptr, nullptr, renderer, toast.Callback());
    return m;
}

constexpr float kDefaultMdWidth = 800.0f;

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

// ---- CopyDiagramAsSvg 入力検証 ----

TEST(ClipboardManager, CopyDiagramAsSvgNoOpForOutOfRangeIndex)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    m.CopyDiagramAsSvg(doc, -1, kDefaultMdWidth, false);
    m.CopyDiagramAsSvg(doc, 9999, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 0);
    EXPECT_TRUE(toast.messages.empty());
}

TEST(ClipboardManager, CopyDiagramAsSvgNoOpForLatexMath)
{
    // LatexMath は IsSvgExportable=false (Mermaid のみ対象)。
    auto doc = Document::FromMarkdown("```math\nx^2\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);
    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 0);
}

TEST(ClipboardManager, CopyDiagramAsSvgNoOpWhenRendererNullptr)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    ToastRecorder toast;
    auto m = MakeManager(nullptr, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);
    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_TRUE(toast.messages.empty());
}

// ---- CopyDiagramAsSvg 主要パス ----

TEST(ClipboardManager, CopyDiagramAsSvgRequestsRendererForMermaid)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 1);
    ASSERT_FALSE(toast.messages.empty()); // "Copying" toast
    EXPECT_FLOAT_EQ(renderer.last_width, kDefaultMdWidth);
    EXPECT_FALSE(renderer.last_dark);
}

TEST(ClipboardManager, CopyDiagramAsSvgInFlightGuardBlocksReentry)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 1);
    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 1); // in-flight ガードで 2 回目は no-op

    // cancelled 経路で in-flight だけ解除 (svg_cache に何も入れない)。
    renderer.FireCallback({}, true);
    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 2);
}

TEST(ClipboardManager, CopyDiagramAsSvgCancelledCallbackEmitsNoToast)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    const auto toast_count_before = toast.messages.size();
    renderer.FireCallback({}, true);
    EXPECT_EQ(toast.messages.size(), toast_count_before);
    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    EXPECT_EQ(renderer.request_count, 2);
}

TEST(ClipboardManager, CopyDiagramAsSvgEmptyResultEmitsFailToast)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph TD; A-->B\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramAsSvg(doc, cb_idx, kDefaultMdWidth, false);
    const auto before = toast.messages.size();
    renderer.FireCallback({}, false);
    EXPECT_EQ(toast.messages.size(), before + 1);
}

TEST(ClipboardManager, CopyDiagramAsSvgPassesCodeAndDarkModeToRenderer)
{
    auto doc = Document::FromMarkdown("```mermaid\ngraph LR; X-->Y\n```\n", L"t.md");
    MockMermaidRenderer renderer;
    ToastRecorder toast;
    auto m = MakeManager(&renderer, toast);
    const int cb_idx = FindFirstNodeIndexByType(doc.GetNodes(), NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);

    m.CopyDiagramAsSvg(doc, cb_idx, 1024.0f, true);
    EXPECT_EQ(renderer.request_count, 1);
    EXPECT_FLOAT_EQ(renderer.last_width, 1024.0f);
    EXPECT_TRUE(renderer.last_dark);
    EXPECT_NE(renderer.last_code.find(L"graph LR"), std::wstring_view::npos);

    renderer.FireCallback({}, true);
}
