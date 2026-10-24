// ダイアグラムプレースホルダのエラー表示 (issue #271)。
// 文字列のテキストレイアウトを作るのに実 DirectWrite が必要なため DWriteTestBase を使う。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "command_generator.h"
#include "document_test_helpers.h"
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
    ComPtr<IDWriteTextFormat> placeholder_fmt_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        placeholder_fmt_ = CreateThemeFormat(theme_.font_size_body);
        ASSERT_NE(placeholder_fmt_, nullptr);
        gen_.SetTheme(&theme_);
        text_layouts_.SetFactory(dwrite_factory_.Get());
        gen_.SetFormats({ nullptr, nullptr, nullptr, placeholder_fmt_.Get() });
        gen_.SetTextLayoutCache(&text_layouts_);
    }

    // 生成コマンドから最初の文字列描画を返す (無ければ null)。
    static const DrawTextLayoutCmd* FirstDrawText(const DrawCommandList& cmds)
    {
        for (const auto& c : cmds) {
            if (auto* t = std::get_if<DrawTextLayoutCmd>(&c)) {
                return t;
            }
        }
        return nullptr;
    }
};

} // namespace

TEST_F(DiagramPlaceholderTest, NoErrorShowsLoadingText)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(pl.nodes.empty());

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{}, {}, 1.0f, {}, -1);

    const auto* text_cmd = FirstDrawText(cmds);
    ASSERT_NE(text_cmd, nullptr);
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
    auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{}, {}, 1.0f, {}, -1);

    const auto* error_cmd = FirstDrawText(cmds);
    ASSERT_NE(error_cmd, nullptr);
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
        const auto* cmd = FirstDrawText(gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{}, {}, 1.0f, {}, -1));
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
