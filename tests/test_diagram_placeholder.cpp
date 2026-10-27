// ダイアグラムプレースホルダのエラー表示 (issue #271)。
// 文字列のテキストレイアウトを作るのに実 DirectWrite が必要なため DWriteTestBase を使う。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "command_generator.h"
#include "document_test_helpers.h"
#include "draw_cmd_test_util.h"
#include "draw_command.h"
#include "i18n.h"
#include "syntax.h"
#include <variant>

using Microsoft::WRL::ComPtr;

namespace {

class DiagramPlaceholderTest : public DWriteTestBase {
protected:
    CommandGenerator gen_;
    TextLayoutCache text_layouts_;
    MdPaneFormats formats_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        formats_.placeholder_text = CreateThemeFormat(theme_.font_size_body);
        ASSERT_NE(formats_.placeholder_text, nullptr);
        text_layouts_.SetFactory(dwrite_factory_.Get());
        gen_.Init(theme_, &formats_, &text_layouts_);
    }
};

} // namespace

TEST_F(DiagramPlaceholderTest, NoErrorShowsLoadingText)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(pl.nodes.empty());

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);

    const auto text_cmd = FindFirst<DrawTextLayoutCmd>(cmds);
    ASSERT_TRUE(text_cmd.has_value());
    EXPECT_EQ(text_layouts_.TextOf(text_cmd->layout), i18n::S().loading);
}

TEST_F(DiagramPlaceholderTest, ErrorShowsErrorMessageInCautionColor)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(pl.nodes.empty());

    // 最初の diagram ノードにエラーを設定
    const int diagram_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Diagram);
    ASSERT_GE(diagram_idx, 0);
    pl.cache.EnsureDiagram(static_cast<size_t>(diagram_idx)).error = { DiagramError::Kind::Render, L"Parse error on line 1" };

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f);

    const auto error_cmd = FindFirst<DrawTextLayoutCmd>(cmds);
    ASSERT_TRUE(error_cmd.has_value());
    EXPECT_EQ(text_layouts_.TextOf(error_cmd->layout), L"Parse error on line 1");
    EXPECT_EQ(error_cmd->brush_id, BrushId::AlertCaution);
}

// 言語で変わる文言は描くときに引くので、失敗した図も切り替えた言語の文言で出る。
TEST_F(DiagramPlaceholderTest, LocalizedErrorFollowsLanguageSwitch)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    const int diagram_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::Diagram);
    ASSERT_GE(diagram_idx, 0);
    auto& error = pl.cache.EnsureDiagram(static_cast<size_t>(diagram_idx)).error;
    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    const auto text_of = [&] {
        const auto cmd = FindFirst<DrawTextLayoutCmd>(gen_.GenerateMdPane({ .nodes = pl.nodes, .cache = pl.cache, .selection = TextSelection{}, .md_pane_rect = pane }, 1.0f));
        return cmd ? std::wstring(text_layouts_.TextOf(cmd->layout)) : std::wstring();
    };

    for (const i18n::Strings* language : { &i18n::kJa, &i18n::kEn }) {
        SCOPED_TRACE(std::wstring(language->lang_key));
        i18n::SetLanguage(language);
        error = { DiagramError::Kind::TooLarge };
        EXPECT_EQ(text_of(), language->diagram_too_large);
        error = { DiagramError::Kind::Render };
        EXPECT_EQ(text_of(), language->diagram_error);
    }
    i18n::SetLanguage(&i18n::kJa);
}
