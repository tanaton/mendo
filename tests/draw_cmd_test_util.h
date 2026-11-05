#pragma once
// DrawCommandList から型で描画コマンドを拾う helper。フィクスチャを問わず使えるよう、
// cmd_gen_mock_test_base.h (MockLayoutTestBase を引き込む) から分けてある。
#include "command_generator.h"
#include "draw_command.h"
#include "search_state.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

// ● と ○ は半径 = 半辺の角丸矩形、■ は FillRect、□ は角の無い DrawRoundedRect で描く。
inline constexpr auto kDisc = [](const FillRoundedRectCmd& r) static { return r.brush_id == BrushId::Text && r.radius > 0.0f; };
inline constexpr auto kCircle = [](const DrawRoundedRectCmd& r) static { return r.brush_id == BrushId::Text && r.radius > 0.0f; };
inline constexpr auto kFilledSquare = [](const FillRectCmd& r) static { return r.brush_id == BrushId::Text; };
inline constexpr auto kOutlinedSquare = [](const DrawRoundedRectCmd& r) static { return r.brush_id == BrushId::Text && r.radius == 0.0f; };

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
    return std::ranges::count_if(cmds, [](const DrawCommand& c) static noexcept {
        return std::holds_alternative<T>(c);
    });
}

template <typename T>
std::vector<T> CollectCmds(const DrawCommandList& cmds)
{
    std::vector<T> out;
    for (const auto& c : cmds) {
        if (const auto* p = std::get_if<T>(&c)) {
            out.push_back(*p);
        }
    }
    return out;
}

template <typename T, typename Pred>
std::ptrdiff_t CountCmd(const DrawCommandList& cmds, Pred pred)
{
    return std::ranges::count_if(cmds, [&pred](const DrawCommand& c) {
        const auto* p = std::get_if<T>(&c);
        return p && pred(*p);
    });
}

// factory が無いのでレイアウトを作らず、文字列を描かない。
inline TextLayoutCache& NoTextLayouts()
{
    static TextLayoutCache text_layouts;
    return text_layouts;
}

inline void InitWithoutText(CommandGenerator& gen, const Theme& theme)
{
    static const MdPaneFormats formats{};
    gen.Init(theme, formats, NoTextLayouts());
}

// ファイル一覧を閉じ、目次だけを rect に出すサイドペイン。toc と nodes は使い終わるまで生かしておく。
inline SidePaneState TocOnlySidePane(PaneRect rect, float item_height, const std::vector<TocEntry>& toc, const std::vector<Node>& nodes, int active_toc_index = -1)
{
    static const std::vector<FileEntry> no_files;
    return {
        .panes = { {}, SidePaneInstance{ .rect = rect, .show = true, .zoom = 1.0f, .item_height = item_height } },
        .file_entries = no_files,
        .toc_entries = toc,
        .nodes = nodes,
        .active_toc_index = active_toc_index,
    };
}

// 選択も検索も無い本文を pane に等倍で描いた命令。
inline DrawCommandList GenerateMdPaneAt(CommandGenerator& gen, const std::vector<Node>& nodes, const LayoutCache& cache, PaneRect pane,
                                        float scroll_y = 0.0f, int hovered_diagram = -1)
{
    static const TextSelection no_selection{};
    DrawCommandList cmds;
    gen.GenerateMdPane(cmds, { .nodes = nodes, .cache = cache, .selection = no_selection, .md_pane_rect = pane, .scroll_y = scroll_y, .hovered_diagram = hovered_diagram }, 1.0f);
    return cmds;
}

// 検索を区切らずに最後まで進める。
inline void RunSearchToEnd(SearchState& s, const std::vector<Node>& nodes, std::string_view query)
{
    s.BeginSearch(query);
    s.ContinueSearch(nodes, std::chrono::steady_clock::time_point::max());
}
