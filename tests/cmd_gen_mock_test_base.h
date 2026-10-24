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

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(MockLayoutTestBase::SetUp());
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ nullptr, nullptr, nullptr, nullptr });
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
        return gen_.GenerateMdPane(nodes_, cache_, md_pane, 0.0f, TextSelection{}, {}, 1.0f, {}, -1);
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
    return std::ranges::count_if(cmds, [](const DrawCommand& c) static {
        return std::holds_alternative<T>(c);
    });
}

template <typename T, typename Pred>
std::ptrdiff_t CountCmd(const DrawCommandList& cmds, Pred pred)
{
    return std::ranges::count_if(cmds, [&pred](const DrawCommand& c) {
        const auto* p = std::get_if<T>(&c);
        return p && pred(*p);
    });
}
