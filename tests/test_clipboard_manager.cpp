#include <gtest/gtest.h>
#include "clipboard_manager.h"
#include "document.h"
#include "i18n.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

// 画像の要求の引数を記録するモック。画像は返さない。
class MockMermaidRenderer : public IMermaidRenderer {
public:
    bool RequestRender(const Node&, DiagramEntry&, float, bool) override
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& drawing, float max_width) override
    {
        ++bitmap_count;
        last_drawing = &drawing;
        last_width = max_width;
        return nullptr;
    }

    int bitmap_count = 0;
    const mermaid_util::DiagramDrawing* last_drawing = nullptr;
    float last_width = 0.0f;
};

constexpr float kDefaultMdWidth = 800.0f;

} // namespace

class ClipboardManagerTest : public ::testing::Test {
protected:
    // HWND/WIC nullptr は production では成立しないが、ガード経路の単体テストに限り許容。
    // 画像無し (nullptr) のため BuildDib は空を返し、クリップボード書き込みは行われない。
    void SetUp() override
    {
        manager_.Init(nullptr, &renderer_, nullptr, [this](std::wstring_view m) { toasts_.emplace_back(m); });
    }

    MockMermaidRenderer renderer_;
    std::vector<std::wstring> toasts_;
    ClipboardManager manager_;
    std::shared_ptr<const mermaid_util::DiagramDrawing> drawing_ =
        std::make_shared<const mermaid_util::DiagramDrawing>(mermaid_util::DiagramDrawing{ .width = 1200.0f, .height = 600.0f });
};

// 範囲外・種別違いのノードのコードのコピーと、描いた絵の無い図は何もしない (トーストも出さず、画像も描かない)。
TEST_F(ClipboardManagerTest, InvalidTargetsAreNoOps)
{
    const Document doc = Document::FromMarkdown("paragraph text", L"t.md");
    manager_.CopyCodeBlock(doc, -1, false);
    manager_.CopyCodeBlock(doc, 9999, false);
    manager_.CopyCodeBlock(doc, 0, false);

    manager_.SaveDiagramAsPng({}, kDefaultMdWidth);
    manager_.CopyDiagramToClipboard({}, kDefaultMdWidth);

    EXPECT_EQ(renderer_.bitmap_count, 0);
    EXPECT_TRUE(toasts_.empty());
}

// コピーは押したその場で終わる。画像は受け取った描画命令から本文の幅で描かせ、結果のトーストを 1 回だけ出す
// (画像が無ければクリップボードに触れず、失敗と知らせる)。
TEST_F(ClipboardManagerTest, CopyDiagramDrawsBitmapAtBodyWidthAndToastsOnce)
{
    manager_.CopyDiagramToClipboard(drawing_, kDefaultMdWidth);
    EXPECT_EQ(renderer_.bitmap_count, 1);
    EXPECT_EQ(renderer_.last_drawing, drawing_.get());
    EXPECT_FLOAT_EQ(renderer_.last_width, kDefaultMdWidth);
    ASSERT_EQ(toasts_.size(), 1u);
    EXPECT_EQ(toasts_[0], i18n::S().toast_diagram_copy_failed);

    // 同じ図を押し直しても、別の図を押しても、毎回その図を描いて知らせる。
    manager_.CopyDiagramToClipboard(drawing_, 1024.0f);
    EXPECT_EQ(renderer_.bitmap_count, 2);
    EXPECT_FLOAT_EQ(renderer_.last_width, 1024.0f);
    EXPECT_EQ(toasts_.size(), 2u);
}
