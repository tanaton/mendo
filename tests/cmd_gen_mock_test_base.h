#pragma once
// MockTextMeasurer + LayoutEngine + CommandGenerator のフィクスチャ基底。
// IDWriteTextLayout を必要としないコマンド生成テスト用。DirectWrite 経路を踏む
// テストは別途 dwrite_test_base.h を使う。
#include <gtest/gtest.h>
#include "command_generator.h"
#include "draw_command.h"
#include "mock_text_measurer.h"
#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

class CmdGenMockTestBase : public MockLayoutTestBase {
protected:
    CommandGenerator gen_;
    LayoutCache cache_;
    std::pmr::vector<Node> nodes_;
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(MockLayoutTestBase::SetUp());
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ nullptr, nullptr, nullptr, nullptr });
        gen_.SetHitTestBuffer(&hit_test_buffer_);
    }

    void Parse(std::string_view md, float viewport_w = 800.0f)
    {
        auto [nodes, cache] = ParseAndLayout(md, viewport_w);
        nodes_ = std::move(nodes);
        cache_ = std::move(cache);
    }
};

template <typename T>
std::optional<T> FindFirst(const DrawCommandList& cmds)
{
    for (const auto& c : cmds) {
        if (auto* p = std::get_if<T>(&c)) {
            return *p;
        }
    }
    return std::nullopt;
}

template <typename T, typename Pred>
std::optional<T> FindFirst(const DrawCommandList& cmds, Pred pred)
{
    for (const auto& c : cmds) {
        if (auto* p = std::get_if<T>(&c); p && pred(*p)) {
            return *p;
        }
    }
    return std::nullopt;
}

template <typename T>
std::ptrdiff_t CountCmd(const DrawCommandList& cmds)
{
    return std::ranges::count_if(cmds, [](const DrawCommand& c) {
        return std::holds_alternative<T>(c);
    });
}
