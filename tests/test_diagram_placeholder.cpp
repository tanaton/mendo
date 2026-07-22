// ダイアグラムプレースホルダのエラー表示 (issue #271)。
// DrawTextCmd の内容検証に実 IDWriteTextFormat が必要なため DWriteTestBase を使う。
#include <gtest/gtest.h>
#include "dwrite_test_base.h"
#include "command_generator.h"
#include "draw_command.h"
#include "i18n.h"
#include "syntax.h"
#include <variant>

using Microsoft::WRL::ComPtr;

namespace {

class DiagramPlaceholderTest : public DWriteTestBase {
protected:
    CommandGenerator gen_;
    ComPtr<IDWriteTextFormat> placeholder_fmt_;
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    void SetUp() override
    {
        DWriteTestBase::SetUp();
        const HRESULT hr = dwrite_factory_->CreateTextFormat(
            theme_.font_family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body,
            L"ja-jp", &placeholder_fmt_);
        ASSERT_TRUE(SUCCEEDED(hr));
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ nullptr, nullptr, nullptr, placeholder_fmt_.Get() });
        gen_.SetHitTestBuffer(&hit_test_buffer_);
    }

    // 生成コマンドから最初の DrawTextCmd のテキストを返す (無ければ空)。
    static std::wstring_view FirstDrawText(const DrawCommandList& cmds)
    {
        for (const auto& c : cmds) {
            if (auto* t = std::get_if<DrawTextCmd>(&c)) {
                return { t->text(), t->text_len };
            }
        }
        return {};
    }
};

} // namespace

TEST_F(DiagramPlaceholderTest, NoErrorShowsLoadingText)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(pl.nodes.empty());

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{});

    EXPECT_EQ(FirstDrawText(cmds), i18n::S().loading);
}

TEST_F(DiagramPlaceholderTest, ErrorShowsErrorMessageInCautionColor)
{
    auto pl = ParseAndLayout("```mermaid\ngraph TD;A-->B\n```\n");
    ASSERT_FALSE(pl.nodes.empty());

    // 最初の diagram ノードにエラーを設定
    bool found = false;
    for (size_t i = 0; i < pl.nodes.size(); ++i) {
        if (pl.nodes[i].type == NodeType::CodeBlock && IsDiagramLanguage(pl.nodes[i].code_language())) {
            pl.cache.GetDiagram(i).error = L"Parse error on line 1";
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    const PaneRect pane{ 0.0f, 0.0f, 800.0f, 600.0f };
    auto& cmds = gen_.GenerateMdPane(pl.nodes, pl.cache, pane, 0.0f, TextSelection{});

    const DrawTextCmd* error_cmd = nullptr;
    for (const auto& c : cmds) {
        if (auto* t = std::get_if<DrawTextCmd>(&c)) {
            error_cmd = t;
            break;
        }
    }
    ASSERT_NE(error_cmd, nullptr);
    EXPECT_EQ(std::wstring_view(error_cmd->text(), error_cmd->text_len), L"Parse error on line 1");
    EXPECT_EQ(error_cmd->brush_id, BrushId::AlertCaution);
}
