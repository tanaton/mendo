#pragma once
// MockTextMeasurer + LayoutEngine + CommandGenerator のフィクスチャ基底。
// IDWriteTextLayout を必要としないコマンド生成テスト用。DirectWrite 経路を踏む
// テストは別途 dwrite_test_base.h を使う。
#include <gtest/gtest.h>
#include "command_generator.h"
#include "draw_cmd_test_util.h"
#include "draw_command.h"
#include "mock_text_measurer.h"
#include <string_view>
#include <utility>
#include <vector>

class CmdGenMockTestBase : public MockLayoutTestBase {
protected:
    CommandGenerator gen_;
    LayoutCache cache_;
    std::vector<Node> nodes_;

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(MockLayoutTestBase::SetUp());
        InitWithoutText(gen_, theme_);
    }

    static constexpr float kViewportW = 800.0f;

    void Parse(std::string_view md)
    {
        auto [nodes, cache] = ParseAndLayout(md, kViewportW);
        nodes_ = std::move(nodes);
        cache_ = std::move(cache);
    }

    // 先頭から描いたコマンド。呼び出し後も nodes_ / cache_ で期待値を組み立てられる。
    DrawCommandList Generate(std::string_view md)
    {
        Parse(md);
        const PaneRect md_pane{ 0, 0, kViewportW, 2000.0f };
        DrawCommandList cmds;
        gen_.GenerateMdPane(cmds, { .nodes = nodes_, .cache = cache_, .selection = TextSelection{}, .md_pane_rect = md_pane }, 1.0f);
        return cmds;
    }
};
