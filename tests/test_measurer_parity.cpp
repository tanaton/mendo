// MockTextMeasurer と DWriteTextMeasurer の不変条件を突き合わせる回帰テスト。
// 絶対値は両者で異なる（mock はヒューリスティック、DWrite は実測）が、
// 「どちらも正の高さを返す」「順序性を維持する」「同じスケーリング式を使う」等の
// 共通ルールを検証することで、片方だけ仕様が変わった際の検出ポイントを残す。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "mock_text_measurer.h"

class MeasurerParityTest : public DWriteTestBase {
protected:
    MockTextMeasurer mock_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        mock_.SetTheme(theme_);
    }

    struct Heights {
        float mock;
        float dwrite;
    };

    // 本番と同じく MeasureEntry 経由で計測する。
    Heights MeasureBoth(Node& node, float max_width)
    {
        Node mock_node = CloneNode(node);
        Node dw_node = CloneNode(node);
        NodeLayoutEntry mock_entry{};
        NodeLayoutEntry dwrite_entry{};
        MeasureEntry(mock_, mock_node, mock_entry, max_width, YRange::All(), 0.0f, nullptr, theme_);
        MeasureEntry(measurer_, dw_node, dwrite_entry, max_width, YRange::All(), 0.0f, nullptr, theme_);
        return { mock_entry.height, dwrite_entry.height };
    }

private:
    // text_layout が非コピー可能なので、計測前の入力のみを複製する。
    // ParseMarkdown 完了後は Node のテキストは Wide で完結する契約のため、Wide テキスト経路のみ扱う。
    static Node CloneNode(const Node& src)
    {
        Node n;
        n.type = src.type;
        n.set_heading_level(src.heading_level());
        n.set_code_language(src.code_language());
        n.SetText(src.GetText(), src.line_count);
        n.SetRuns(src.runs());
        n.set_image_data(src.image_data());
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

// 見出しと段落の順序性: 同じテキストに対し、両計測とも見出し > 段落 となること。
TEST_F(MeasurerParityTest, HeadingTallerThanParagraphInBoth)
{
    Node para = MakeTextNode("Sample text");

    Node heading = MakeTextNode("Sample text");
    heading.type = NodeType::Heading;
    heading.set_heading_level(1);

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
    Node short_node = MakeTextNode("Short");
    Node long_node = MakeTextNode(
        "This is a much longer paragraph that should wrap across multiple lines "
        "at the given narrow max width, producing a taller layout result than the short one.");

    const auto s = MeasureBoth(short_node, 200.0f);
    const auto l = MeasureBoth(long_node, 200.0f);
    EXPECT_GE(l.mock, s.mock);
    EXPECT_GE(l.dwrite, s.dwrite);
}
