// MockTextMeasurer と DWriteTextMeasurer の不変条件を突き合わせる回帰テスト。
// 絶対値は両者で異なる（mock はヒューリスティック、DWrite は実測）が、
// 「どちらも正の高さを返す」「順序性を維持する」「同じスケーリング式を使う」等の
// 共通ルールを検証することで、片方だけ仕様が変わった際の検出ポイントを残す。
#include <gtest/gtest.h>
#include "mock_text_measurer.h"
#include "dwrite_measurer.h"
#include "document_types.h"
#include "layout_cache.h"
#include "test_helpers.h"
#include "theme.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class MeasurerParityTest : public ComApartmentTest {
protected:
    ComPtr<IDWriteFactory> dwrite_factory_;
    DWriteTextMeasurer dwrite_;
    MockTextMeasurer mock_;
    Theme theme_;

    void SetUp() override
    {
        const HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr));

        theme_ = GetLightTheme();
        dwrite_.SetFactory(dwrite_factory_.Get());
        ASSERT_TRUE(dwrite_.Init(theme_));
        ASSERT_TRUE(mock_.Init(theme_));
    }

    struct Heights {
        float mock;
        float dwrite;
    };

    Heights MeasureBoth(Node& node, float max_width)
    {
        Node mock_node = CloneNode(node);
        Node dw_node = CloneNode(node);
        NodeLayoutEntry mock_entry{};
        NodeLayoutEntry dwrite_entry{};
        mock_.MeasureNode(mock_node, mock_entry, max_width);
        dwrite_.MeasureNode(dw_node, dwrite_entry, max_width);
        return { mock_entry.height, dwrite_entry.height };
    }

private:
    // text_layout が非コピー可能なので、計測前の入力のみを複製する。
    // SetText は text_utf8 をクリアするため、utf8 のみ保持している src のケースを
    // 壊さないように text_utf8 と text_ の両経路を使い分ける。
    static Node CloneNode(const Node& src)
    {
        Node n;
        n.type = src.type;
        n.heading_level = src.heading_level;
        n.code_language = src.code_language;
        if (!src.text_utf8.empty()) {
            n.text_utf8 = src.text_utf8;
            n.ConvertTextFromUtf8();
        }
        else if (!src.GetText().empty()) {
            n.SetText(src.GetText());
        }
        n.runs = src.runs;
        if (src.has_image()) {
            n.ensure_image();
            *n.image_data = *src.image_data;
        }
        return n;
    }
};

// HorizontalRule: 両計測とも定数の正の高さを返すこと。
TEST_F(MeasurerParityTest, HorizontalRuleIsPositive)
{
    Node node;
    node.type = NodeType::HorizontalRule;
    const auto h = MeasureBoth(node, 600.0f);
    EXPECT_GT(h.mock, 0.0f);
    EXPECT_GT(h.dwrite, 0.0f);
}

// 空の段落: マージン確保のため両計測とも正の高さを返すこと。
TEST_F(MeasurerParityTest, EmptyParagraphIsPositive)
{
    Node node;
    node.type = NodeType::Paragraph;
    const auto h = MeasureBoth(node, 600.0f);
    EXPECT_GT(h.mock, 0.0f);
    EXPECT_GT(h.dwrite, 0.0f);
}

// 画像（原寸 > max_width）: 両計測とも max_width/orig_w のスケールを適用して一致すること。
TEST_F(MeasurerParityTest, ImageScalesToMaxWidthIdentically)
{
    Node node;
    node.type = NodeType::Image;
    node.ensure_image();
    node.image_data->width = 1200.0f;
    node.image_data->height = 800.0f;

    const auto h = MeasureBoth(node, 600.0f);
    EXPECT_FLOAT_EQ(h.mock, 400.0f);
    EXPECT_FLOAT_EQ(h.dwrite, 400.0f);
}

// 画像（原寸 <= max_width）: 両計測とも原寸高を維持すること。
TEST_F(MeasurerParityTest, ImagePreservesOriginalHeightWhenFits)
{
    Node node;
    node.type = NodeType::Image;
    node.ensure_image();
    node.image_data->width = 300.0f;
    node.image_data->height = 200.0f;

    const auto h = MeasureBoth(node, 600.0f);
    EXPECT_FLOAT_EQ(h.mock, 200.0f);
    EXPECT_FLOAT_EQ(h.dwrite, 200.0f);
}

// ダイアグラム系コードブロック: 両計測ともプレースホルダー最低値以上を返すこと。
TEST_F(MeasurerParityTest, DiagramCodeBlockUsesPlaceholderHeight)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.code_language = SyntaxLanguage::Mermaid;
    const auto h = MeasureBoth(node, 600.0f);
    EXPECT_GE(h.mock, 60.0f);
    EXPECT_GE(h.dwrite, 60.0f);
}

// 見出しと段落の順序性: 同じテキストに対し、両計測とも見出し > 段落 となること。
TEST_F(MeasurerParityTest, HeadingTallerThanParagraphInBoth)
{
    Node para;
    para.type = NodeType::Paragraph;
    para.SetText(L"Sample text");

    Node heading;
    heading.type = NodeType::Heading;
    heading.heading_level = 1;
    heading.SetText(L"Sample text");

    const auto p = MeasureBoth(para, 600.0f);
    const auto h = MeasureBoth(heading, 600.0f);

    EXPECT_GT(p.mock, 0.0f);
    EXPECT_GT(p.dwrite, 0.0f);
    EXPECT_GT(h.mock, p.mock);
    EXPECT_GT(h.dwrite, p.dwrite);
}

// 折り返し: 短いテキストと長いテキスト（同じ幅）で、長い方が高くなるか同じ高さであること。
TEST_F(MeasurerParityTest, LongerTextIsNotShorter)
{
    Node short_node;
    short_node.type = NodeType::Paragraph;
    short_node.SetText(L"Short");

    Node long_node;
    long_node.type = NodeType::Paragraph;
    long_node.SetText(L"This is a much longer paragraph that should wrap across multiple lines "
                     L"at the given narrow max width, producing a taller layout result than the short one.");

    const auto s = MeasureBoth(short_node, 200.0f);
    const auto l = MeasureBoth(long_node, 200.0f);
    EXPECT_GE(l.mock, s.mock);
    EXPECT_GE(l.dwrite, s.dwrite);
}
