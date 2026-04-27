#pragma once
// MockTextMeasurer + LayoutEngine + CommandGenerator のフィクスチャ基底。
// IDWriteTextLayout を必要としないコマンド生成テスト用。DirectWrite 経路を踏む
// テストは別途 dwrite_test_base.h を使う。
#include <gtest/gtest.h>
#include "command_generator.h"
#include "draw_command.h"
#include "layout.h"
#include "mock_text_measurer.h"
#include "parser.h"
#include "theme.h"
#include <memory_resource>
#include <variant>

class CmdGenMockTestBase : public ::testing::Test {
protected:
    MockTextMeasurer mock_;
    LayoutEngine engine_;
    CommandGenerator gen_;
    Theme theme_;
    LayoutCache cache_;
    std::pmr::vector<Node> nodes_;

    void SetUp() override
    {
        theme_ = GetLightTheme();
        ASSERT_TRUE(engine_.Init(&mock_, theme_));
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ nullptr, nullptr, nullptr, nullptr });
    }

    void Parse(const std::string& md, float viewport_w = 800.0f)
    {
        nodes_ = ParseMarkdown(md).nodes;
        cache_.Resize(nodes_.size());
        engine_.ComputeLayout(nodes_, cache_, viewport_w);
    }
};

template <typename T>
const T* FindFirst(const DrawCommandList& cmds)
{
    for (const auto& c : cmds) {
        if (auto* p = std::get_if<T>(&c)) {
            return p;
        }
    }
    return nullptr;
}
