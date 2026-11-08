#include <gtest/gtest.h>
#include "clipboard_manager.h"
#include "document.h"
#include "document_test_helpers.h"
#include "mermaid_renderer_interface.h"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
        callbacks.push_back(std::move(cb));
    }

    // index 番目 (既定は最後) の要求の結果を返す。結果は 1 回だけ返す。
    void FireCallback(std::string_view svg, std::optional<size_t> index = {})
    {
        if (callbacks.empty()) {
            return;
        }
        if (auto cb = std::exchange(callbacks[index.value_or(callbacks.size() - 1)], {})) {
            cb(svg);
        }
    }

    int request_count = 0;
    std::string last_code;
    float last_width = 0.0f;
    bool last_dark = false;
    std::vector<SvgCallback> callbacks;
};

constexpr float kDefaultMdWidth = 800.0f;
constexpr std::string_view kMermaidMd = "```mermaid\ngraph TD; A-->B\n```\n";

} // namespace

class ClipboardManagerTest : public ::testing::Test {
protected:
    // HWND/WIC nullptr は production では成立しないが、ガード経路の単体テストに限り許容。
    // 画像無し (nullptr) のため BuildDib は空を返し、クリップボード書き込みは行われない。
    void SetUp() override
    {
        manager_.Init(nullptr, &renderer_, nullptr, [this](std::wstring_view m) { toasts_.emplace_back(m); });
    }

    // 文書を差し替え、最初の type ノードの index を返す。
    int Load(std::string_view md, NodeType type = NodeType::Diagram)
    {
        doc_ = Document::FromMarkdown(std::string(md), L"t.md");
        return FindFirstNodeIndexByType(doc_.GetNodes(), type);
    }

    void CopyDiagram(int node_index, float md_width = kDefaultMdWidth, bool dark = false)
    {
        manager_.CopyDiagramToClipboard(doc_, node_index, nullptr, md_width, dark);
    }

    Document doc_;
    MockMermaidRenderer renderer_;
    std::vector<std::wstring> toasts_;
    ClipboardManager manager_;
};

// ---- 入力検証 ----

// 範囲外・種別違いのノードと、描いた絵の無い図は何もしない (トーストも出さず、図も頼まない)。
TEST_F(ClipboardManagerTest, InvalidTargetsAreNoOps)
{
    Load("paragraph text");
    manager_.CopyCodeBlock(doc_, -1, false);
    manager_.CopyCodeBlock(doc_, 9999, false);
    manager_.CopyCodeBlock(doc_, 0, false);

    const int code = Load("```cpp\nint main(){}\n```\n", NodeType::CodeBlock);
    ASSERT_GE(code, 0);
    manager_.SaveDiagramAsPng(doc_, code, {});

    const int diagram = Load(kMermaidMd);
    ASSERT_GE(diagram, 0);
    manager_.SaveDiagramAsPng(doc_, -1, {});
    manager_.SaveDiagramAsPng(doc_, 9999, {});
    manager_.SaveDiagramAsPng(doc_, diagram, {});
    CopyDiagram(-1);
    CopyDiagram(9999);

    EXPECT_EQ(renderer_.request_count, 0);
    EXPECT_TRUE(toasts_.empty());
}

// ---- CopyDiagramToClipboard 主要パス ----

// 図のコード・幅・ダークモードを渡して SVG を頼み、待つ間のトーストを出す。
TEST_F(ClipboardManagerTest, CopyDiagramToClipboardPassesCodeWidthAndDarkModeToRenderer)
{
    const int cb_idx = Load("```mermaid\ngraph LR; X-->Y\n```\n");
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 1);
    EXPECT_FALSE(toasts_.empty());
    EXPECT_FLOAT_EQ(renderer_.last_width, kDefaultMdWidth);
    EXPECT_FALSE(renderer_.last_dark);
    EXPECT_TRUE(renderer_.last_code.contains("graph LR"));
    renderer_.FireCallback({});

    CopyDiagram(cb_idx, 1024.0f, true);
    EXPECT_EQ(renderer_.request_count, 2);
    EXPECT_FLOAT_EQ(renderer_.last_width, 1024.0f);
    EXPECT_TRUE(renderer_.last_dark);
    renderer_.FireCallback({});
}

// 同じ図を待っている間に押し直しても頼み直さない。頼み直すと、待っている要求の結果が古い扱いになりトーストが出ない。
// 結果 (描けなかったときも) が届いたら次の要求を受け付ける。
TEST_F(ClipboardManagerTest, CopyDiagramToClipboardInFlightGuardBlocksReentry)
{
    const int cb_idx = Load(kMermaidMd);
    ASSERT_GE(cb_idx, 0);

    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 1);
    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 1);

    // 描けなかった結果で in-flight を解除する (svg_cache に何も入れない)。
    const auto before = toasts_.size();
    renderer_.FireCallback({});
    EXPECT_EQ(toasts_.size(), before + 1);
    CopyDiagram(cb_idx);
    EXPECT_EQ(renderer_.request_count, 2);
}

// 待っている間に別の図をコピーしたら、後から押した方に置き換える。先に頼んだ結果は書かず、トーストも出さない。
TEST_F(ClipboardManagerTest, CopyOfAnotherDiagramReplacesPending)
{
    Load("```mermaid\ngraph TD; A-->B\n```\n\n```mermaid\ngraph TD; C-->D\n```\n");
    const auto& diagrams = doc_.GetDiagramNodeIndices();
    ASSERT_EQ(diagrams.size(), 2u);

    CopyDiagram(static_cast<int>(diagrams[0]));
    CopyDiagram(static_cast<int>(diagrams[1]));
    ASSERT_EQ(renderer_.request_count, 2);
    EXPECT_EQ(renderer_.last_code, "graph TD; C-->D");

    const auto before = toasts_.size();
    renderer_.FireCallback({}, 0);
    EXPECT_EQ(toasts_.size(), before) << "置き換えた要求の結果";
    renderer_.FireCallback({}, 1);
    EXPECT_EQ(toasts_.size(), before + 1);
}
