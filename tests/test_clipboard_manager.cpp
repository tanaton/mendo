#include <gtest/gtest.h>
#include "clipboard_manager.h"
#include "document.h"
#include "document_test_helpers.h"
#include "mermaid_renderer_interface.h"
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace {

// SVG リクエスト引数とコールバックを記録するモック。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    bool RequestRender(const Node&, DiagramEntry&, float, bool) override
    {
        return false;
    }

    void RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback cb) override
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
    std::pmr::string last_code;
    float last_width = 0.0f;
    bool last_dark = false;
    SvgCallback last_callback;
};

constexpr float kDefaultMdWidth = 800.0f;
constexpr std::string_view kMermaidMd = "```mermaid\ngraph TD; A-->B\n```\n";

// テストは png 無しでガード経路のみ踏むため、共通の空 PNG ハンドルを使う。
const PngBytes kNoPng;

} // namespace

class ClipboardManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        InitManager(&renderer_);
    }

    // HWND/WIC nullptr は production では成立しないが、ガード経路の単体テストに限り許容。
    // png 無し (nullptr) のため BuildDib は空を返し、クリップボード書き込みは行われない。
    void InitManager(IMermaidRenderer* renderer)
    {
        manager_.Init(nullptr, renderer, nullptr, [this](std::wstring_view m) { toasts_.emplace_back(m); });
    }

    // 文書を差し替え、最初の type ノードの index を返す。
    int Load(std::string_view md, NodeType type = NodeType::Diagram)
    {
        doc_ = Document::FromMarkdown(std::pmr::string(md), L"t.md");
        return FindFirstNodeIndexByType(doc_.GetNodes(), type);
    }

    void CopyDiagram(int node_index, float md_width = kDefaultMdWidth, bool dark = false)
    {
        manager_.CopyDiagramToClipboard(doc_, node_index, kNoPng, md_width, dark);
    }

    Document doc_;
    MockMermaidRenderer renderer_;
    std::vector<std::wstring> toasts_;
    ClipboardManager manager_;
};

// ---- CopyCodeBlock 入力検証 ----

TEST_F(ClipboardManagerTest, CopyCodeBlockNoOpForOutOfRangeIndex)
{
    Load("para");
    manager_.CopyCodeBlock(doc_, -1, false);
    manager_.CopyCodeBlock(doc_, 9999, false);
    EXPECT_TRUE(toasts_.empty());
}

TEST_F(ClipboardManagerTest, CopyCodeBlockNoOpForNonCodeBlock)
{
    Load("paragraph text");
    manager_.CopyCodeBlock(doc_, 0, false);
    EXPECT_TRUE(toasts_.empty());
}

// ---- SaveDiagramAsPng 入力検証 ----

TEST_F(ClipboardManagerTest, SaveDiagramAsPngNoOpForOutOfRangeIndex)
{
    Load(kMermaidMd);
    manager_.SaveDiagramAsPng(doc_, -1, kNoPng);
    manager_.SaveDiagramAsPng(doc_, 9999, kNoPng);
    EXPECT_TRUE(toasts_.empty());
}

TEST_F(ClipboardManagerTest, SaveDiagramAsPngNoOpForNonDiagramLanguage)
{
    const int cb_idx = Load("```cpp\nint main(){}\n```\n", NodeType::CodeBlock);
    ASSERT_GE(cb_idx, 0);
    manager_.SaveDiagramAsPng(doc_, cb_idx, kNoPng);
    EXPECT_TRUE(toasts_.empty());
}

TEST_F(ClipboardManagerTest, SaveDiagramAsPngNoOpWithoutPng)
{
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);
    manager_.SaveDiagramAsPng(doc_, cb_idx, kNoPng);
    EXPECT_TRUE(toasts_.empty());
}

// ---- CopyDiagramToClipboard 入力検証 ----

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardNoOpForOutOfRangeIndex)
{
    Load(kMermaidMd);
    CopyDiagram(-1);
    CopyDiagram(9999);
    EXPECT_EQ(renderer_.request_count, 0);
    EXPECT_TRUE(toasts_.empty());
}

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardLatexMathCopiesImageWithoutRenderer)
{
    // LatexMath は IsSvgExportable=false。画像 (CF_DIB) のみを同期コピーし、SVG レンダラは呼ばない。
    // LatexMath は $$..$$ 段落の昇格でのみ生成される (parser.cpp)。
    // png 無しのため画像構築は失敗し、同期パスは結果トースト (失敗) を 1 回出す。
    const int cb_idx = Load("$$x^2$$\n");
    ASSERT_GE(cb_idx, 0);
    ASSERT_EQ(doc_.GetNodes()[cb_idx].code_language(), SyntaxLanguage::LatexMath);
    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 0); // SVG レンダラには行かない
    EXPECT_EQ(toasts_.size(), 1u);         // 同期パスが結果を報告する (旧実装の no-op ではない)
}

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardNoOpWhenRendererNullptr)
{
    InitManager(nullptr);
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);
    CopyDiagram(cb_idx);
    EXPECT_TRUE(toasts_.empty());
}

// ---- CopyDiagramToClipboard 主要パス ----

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardRequestsRendererForMermaid)
{
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 1);
    ASSERT_FALSE(toasts_.empty());
    EXPECT_FLOAT_EQ(renderer_.last_width, kDefaultMdWidth);
    EXPECT_FALSE(renderer_.last_dark);
}

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardInFlightGuardBlocksReentry)
{
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 1);
    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 1);

    // cancelled 経路で in-flight だけ解除 (svg_cache に何も入れない)。
    renderer_.FireCallback({}, true);
    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 2);
}

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardInFlightReentryDoesNotStaleInProgress)
{
    // in-flight 中の再入(2回目)で世代を進めてしまうと、進行中リクエストのコールバックが
    // stale 扱いになり結果(トースト)が出ずに終わる退行を防ぐ。
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx); // request 1 (gen 捕捉)
    CopyDiagram(cb_idx); // in-flight で弾く (世代は進めない)
    EXPECT_EQ(renderer_.request_count, 1);

    const auto before = toasts_.size();
    renderer_.FireCallback({}, false); // request 1 完了。stale でないので結果トーストを出す
    EXPECT_EQ(toasts_.size(), before + 1);
}

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardCancelledCallbackEmitsFailToast)
{
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx);
    const auto toast_count_before = toasts_.size();
    renderer_.FireCallback({}, true);
    EXPECT_EQ(toasts_.size(), toast_count_before + 1);
    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 2);
}

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardEmptyResultEmitsFailToast)
{
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx);
    const auto before = toasts_.size();
    renderer_.FireCallback({}, false);
    EXPECT_EQ(toasts_.size(), before + 1);
}

TEST_F(ClipboardManagerTest, CopyDiagramToClipboardPassesCodeAndDarkModeToRenderer)
{
    const int cb_idx = Load("```mermaid\ngraph LR; X-->Y\n```\n");
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx, 1024.0f, true);
    EXPECT_EQ(renderer_.request_count, 1);
    EXPECT_FLOAT_EQ(renderer_.last_width, 1024.0f);
    EXPECT_TRUE(renderer_.last_dark);
    EXPECT_TRUE(renderer_.last_code.contains("graph LR"));

    renderer_.FireCallback({}, true);
}
