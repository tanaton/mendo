#pragma once
#include "app_state.h"
#include "document.h"
#include "reducer.h"
#include "side_effect.h"
#include "theme.h"
#include <string>
#include <string_view>

// reducer が読む Theme の非ゼロ値を設定する。
// theme は value-init ({}) 済みであること: Theme は zoom 以外に default member initializer を持たない集約。
inline void InitReducerState(AppState& state, Theme& theme)
{
    theme.pane_item_height = 28.0f;
    theme.pane_header_height = 32.0f;
    theme.splitter_width = 4.0f;
    theme.zoom = 1.0f;
    state.theme = &theme;
}

// 計測を頼まれても何もしない Reduce。テストのキャッシュは計測済みとして組む。
inline SideEffectList Reduce(AppState& state, const AppAction& action, std::pmr::memory_resource* resource = std::pmr::get_default_resource())
{
    struct NoLayout final : NodeLayoutAccess {
        void EnsureNodeLayout(int) override {}
    };
    static NoLayout no_layout;
    return Reduce(state, action, no_layout, resource);
}

// reducer を App 相当の前提 (Theme 設定済み・レイアウト確定済み) で
// 駆動するテスト用ハーネス。ランダム列テストで同じ初期状態を何度も組み立てるために使う。
// state が theme を指すため、生成後は移動・コピーしない。
class ReducerHarness {
public:
    static constexpr float kNodeHeight = 60.0f;
    // 先頭ノードの Top。本番でも margin_top + spacing_above で常に正になる。
    static constexpr float kFirstTop = 20.0f;
    static constexpr float kMdWidth = 600.0f;

    struct Options {
        std::string_view markdown;
        float md_height = 400.0f;
        // コードブロックの自然幅。可視幅 (kMdWidth - 左右マージン) を超えれば横スクロール可能。
        float code_natural_width = 1500.0f;
    };

    explicit ReducerHarness(const Options& opt)
    {
        theme.margin_top = 12.0f;
        theme.margin_left = 20.0f;
        theme.margin_right = 20.0f;
        theme.heading_spacing_above = 8.0f;
        InitReducerState(state, theme);

        state.document.doc = Document::FromMarkdown(std::string(opt.markdown), L"C:\\docs\\a.md");
        const auto& nodes = state.document.doc.GetNodes();
        auto& cache = state.document.layout_cache;
        cache = LayoutCache(nodes.size());
        float y = kFirstTop;
        for (size_t i = 0; i < nodes.size(); ++i) {
            cache.SetTop(i, y);
            cache[i].height = kNodeHeight;
            if (nodes[i].type == NodeType::CodeBlock) {
                cache[i].set_natural_code_width(opt.code_natural_width);
            }
            y += kNodeHeight;
        }

        layout_.file_rect = { 0.0f, 0.0f, 150.0f, opt.md_height };
        layout_.toc_rect = { 154.0f, 0.0f, 150.0f, 120.0f };
        layout_.md_rect = { 308.0f, 0.0f, kMdWidth, opt.md_height };
        state.pane_layout_cache.Set(layout_);
    }

    ReducerHarness(const ReducerHarness&) = delete;
    ReducerHarness& operator=(const ReducerHarness&) = delete;

    // App::Dispatch と同じく、無効化されたペインレイアウトを確定させてから Reduce する。
    SideEffectList Dispatch(const AppAction& action, std::pmr::memory_resource* resource = std::pmr::get_default_resource())
    {
        if (!state.pane_layout_cache.IsValid()) {
            state.pane_layout_cache.Set(layout_);
        }
        return Reduce(state, action, resource);
    }

    // App::ClampScrollToContent 相当。
    void ClampScrollToContent()
    {
        state.view.viewport.ClampScroll(MdMaxScroll(state));
    }

    // ウィンドウリサイズ相当: md ペイン高を変えて scroll_y を上限に収める。
    void ResizeMdPane(float md_height)
    {
        layout_.md_rect.height = md_height;
        state.pane_layout_cache.Set(layout_);
        ClampScrollToContent();
    }

    int NodeCount() const noexcept
    {
        return static_cast<int>(state.document.doc.GetNodes().size());
    }

    Theme theme{};
    AppState state;

private:
    PaneLayout layout_{};
};

// 見出し・段落・横スクロール可能なコードブロックを含む標準文書。
inline std::string MakeHarnessMarkdown(int sections)
{
    std::string md;
    for (int i = 0; i < sections; ++i) {
        md += "# Heading " + std::to_string(i) + "\n\nparagraph text " + std::to_string(i) + " alpha beta\n\n";
        if (i % 3 == 1) {
            md += "```\nlong code line " + std::to_string(i) + "\n```\n\n";
        }
    }
    return md;
}
