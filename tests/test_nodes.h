#pragma once
#include "document_types.h"
#include "parser.h"
#include "text_arena.h"
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// テストで作るノードのテキスト・runs と、ParseNodes が受け取った arena はテストプロセスの終了まで残す。
// ノードを ParseResult やテスト用の文字列より長く持つテストでも寿命を気にせず使える。
namespace test_nodes_detail {

inline std::mutex& Mutex()
{
    static std::mutex m;
    return m;
}

inline TextArena& Arena()
{
    static TextArena arena;
    return arena;
}

inline void KeepAlive(std::unique_ptr<TextArena> arena)
{
    static std::vector<std::unique_ptr<TextArena>> kept;
    const std::scoped_lock lock{ Mutex() };
    kept.push_back(std::move(arena));
}

} // namespace test_nodes_detail

// ParseMarkdown の nodes だけを使うテスト向け。ParseResult の arena は手放しても解放されない。
// 原文を指すノードがあるので、markdown_text はノードより長く生きること (ParseMarkdown と同じ)。
inline std::pmr::vector<Node> ParseNodes(std::string_view markdown_text)
{
    auto [nodes, arena] = ParseMarkdown(markdown_text).TakeNodes();
    test_nodes_detail::KeepAlive(std::move(arena));
    return std::move(nodes);
}

template <class S>
    requires(!std::is_lvalue_reference_v<S> &&
             (std::same_as<std::remove_cv_t<S>, std::string> || std::same_as<std::remove_cv_t<S>, std::pmr::string>))
std::pmr::vector<Node> ParseNodes(S&& markdown_text) = delete;

// ノードのテキストが raw (パース入力) を直接指しているか。false なら arena にコピーされている。
inline bool TextPointsIntoRaw(const Node& n, std::string_view raw) noexcept
{
    const auto p = reinterpret_cast<uintptr_t>(n.GetText().data());
    const auto base = reinterpret_cast<uintptr_t>(raw.data());
    return n.HasText() && p >= base && p < base + raw.size();
}

inline void SetTestText(Node& n, std::string_view text, int32_t line_count)
{
    const std::scoped_lock lock{ test_nodes_detail::Mutex() };
    n.SetText(test_nodes_detail::Arena().CopyText(text), line_count);
}

inline void SetTestRuns(Node& n, std::span<const TextRun> runs)
{
    const std::scoped_lock lock{ test_nodes_detail::Mutex() };
    n.SetRuns(test_nodes_detail::Arena().Copy(runs));
}

// n のリンク URL の末尾に url を足し、その link_url_index を返す。
inline int16_t AddTestLinkUrl(Node& n, std::string_view url)
{
    const std::scoped_lock lock{ test_nodes_detail::Mutex() };
    auto& arena = test_nodes_detail::Arena();
    std::vector<std::string_view> urls{ n.view_link_urls().begin(), n.view_link_urls().end() };
    urls.push_back(arena.CopyText(url));
    n.SetLinkUrls(arena.Copy(urls));
    return static_cast<int16_t>(urls.size() - 1);
}
