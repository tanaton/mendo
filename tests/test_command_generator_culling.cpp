// GenerateMdPane のカリング (開始ノードの二分探索・ノード単位/行単位の可視判定) の差分テスト。
// カリングの結果は「カリングなしで文書全体を描き、ビューポートで切り取ったもの」と一致しなければならない。
// ノード高さの外にはみ出す描画 (見出し下線、空 LI の bullet/checkbox、コードブロック背景の上下
// padding とその中の横スクロールバー、アラート背景など) がビューポート端で消えないことを守る。
// 空 LI の高さ 0 は MockTextMeasurer では再現できないため DWriteTestBase を使う。
// インラインコード背景は Renderer の effects パスで作られるため対象外。
#include <gtest/gtest.h>
#include "cmd_gen_mock_test_base.h"
#include "command_generator.h"
#include "document_test_helpers.h"
#include "draw_command.h"
#include "dwrite_test_base.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

namespace {

constexpr float PANE_W = 640.0f;
constexpr float PANE_H = 300.0f;
// 1 物理 px (dpi=1) 未満しか食い込まない描画 (罫線の線幅の半分など) は可視とみなさない。
constexpr float VISIBLE_EPS = 1.0f;
constexpr float COORD_EPS = 0.05f;

constexpr std::string_view CULL_DOC_MD = R"(# Heading one

## Heading two

- a

- b

- [ ] task a

- [x] task b

1. first

2. second

Plain paragraph

```cpp
int a_very_long_line_of_code_that_should_need_horizontal_scrolling_in_this_pane = 0; // more text to be sure it overflows
int b = 1;
```

> [!NOTE]
> Alert body text

> plain quote
> > nested quote

| A | B |
|---|---|
| 1 | 2 |
| 3 | 4 |

---

Last paragraph
)";

struct Box {
    size_t kind;
    float l, t, r, b;
};

std::optional<Box> Extent(const DrawCommand& cmd)
{
    const size_t kind = cmd.index();
    const auto rect_box = [kind](const D2D1_RECT_F& r) {
        return Box{ kind, r.left, r.top, r.right, r.bottom };
    };
    if (const auto* c = std::get_if<FillRectCmd>(&cmd)) {
        return rect_box(c->rect);
    }
    if (const auto* c = std::get_if<FillRoundedRectCmd>(&cmd)) {
        return rect_box(c->rect);
    }
    if (const auto* c = std::get_if<DrawTextCmd>(&cmd)) {
        return rect_box(c->rect);
    }
    if (const auto* c = std::get_if<DrawBitmapCmd>(&cmd)) {
        return rect_box(c->dest);
    }
    if (const auto* c = std::get_if<DrawLineCmd>(&cmd)) {
        // 生成される線は水平/垂直のみ。線幅は線に直交する向きにだけ広がる (端は flat cap)。
        const float hx = (c->p0.x == c->p1.x) ? c->stroke_width * 0.5f : 0.0f;
        const float hy = (c->p0.y == c->p1.y) ? c->stroke_width * 0.5f : 0.0f;
        return Box{ kind, std::min(c->p0.x, c->p1.x) - hx, std::min(c->p0.y, c->p1.y) - hy, std::max(c->p0.x, c->p1.x) + hx, std::max(c->p0.y, c->p1.y) + hy };
    }
    if (const auto* c = std::get_if<FillEllipseCmd>(&cmd)) {
        return Box{ kind, c->center.x - c->rx, c->center.y - c->ry, c->center.x + c->rx, c->center.y + c->ry };
    }
    if (const auto* c = std::get_if<DrawEllipseCmd>(&cmd)) {
        const float h = c->stroke_width * 0.5f;
        return Box{ kind, c->center.x - c->rx - h, c->center.y - c->ry - h, c->center.x + c->rx + h, c->center.y + c->ry + h };
    }
    if (const auto* c = std::get_if<DrawTextLayoutCmd>(&cmd)) {
        DWRITE_TEXT_METRICS m{};
        if (FAILED(c->layout->GetMetrics(&m))) {
            return std::nullopt;
        }
        const float l = c->origin.x + m.left;
        const float t = c->origin.y + m.top;
        return Box{ kind, l, t, l + m.widthIncludingTrailingWhitespace, t + m.height };
    }
    // Clip / Transform は描画しない
    return std::nullopt;
}

std::vector<Box> AllBoxes(const DrawCommandList& cmds)
{
    std::vector<Box> out;
    for (const auto& c : cmds) {
        if (auto b = Extent(c)) {
            out.push_back(*b);
        }
    }
    return out;
}

// dy だけ上にずらしたうえでビューポート [0, PANE_H] に食い込むものだけを残す。
std::vector<Box> VisibleBoxes(const std::vector<Box>& boxes, float dy)
{
    std::vector<Box> out;
    for (auto b : boxes) {
        b.t -= dy;
        b.b -= dy;
        if (b.b > VISIBLE_EPS && b.t < PANE_H - VISIBLE_EPS) {
            out.push_back(b);
        }
    }
    return out;
}

bool SameBox(const Box& a, const Box& b)
{
    return a.kind == b.kind && std::abs(a.l - b.l) < COORD_EPS && std::abs(a.t - b.t) < COORD_EPS &&
           std::abs(a.r - b.r) < COORD_EPS && std::abs(a.b - b.b) < COORD_EPS;
}

std::string Format(const Box& b)
{
    return std::format("kind={} [{:.2f},{:.2f}]-[{:.2f},{:.2f}]", b.kind, b.l, b.t, b.r, b.b);
}

// 多重集合として比較し、片側にしか無いものを列挙する (空なら一致)。
std::string DiffBoxes(const std::vector<Box>& expected, std::vector<Box> actual)
{
    std::string diff;
    for (const auto& e : expected) {
        const auto it = std::ranges::find_if(actual, [&e](const Box& a) { return SameBox(e, a); });
        if (it == actual.end()) {
            diff += "  missing " + Format(e) + "\n";
        }
        else {
            actual.erase(it);
        }
    }
    for (const auto& a : actual) {
        diff += "  extra   " + Format(a) + "\n";
    }
    return diff;
}

class CullingParityTest : public DWriteTestBase {
protected:
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_;
    CommandGenerator gen_;
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_buf_;

    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(DWriteTestBase::SetUp());
        // bullet 以外の list 番号 / checkbox / コピーボタンも発行させるため全 format を埋める。
        ASSERT_TRUE(SUCCEEDED(dwrite_factory_->CreateTextFormat(
            L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, theme_.font_size_body, L"", &fmt_)));
        gen_.SetTheme(&theme_);
        gen_.SetFormats({ fmt_.Get(), fmt_.Get(), fmt_.Get(), fmt_.Get() });
        gen_.SetHitTestBuffer(&hit_buf_);
    }

    const DrawCommandList& Generate(const ParsedLayout& pl, float scroll_y, float pane_h, const BlockHScrollContext& h_scroll)
    {
        return gen_.GenerateMdPane(pl.nodes, pl.cache, PaneRect{ 0.0f, 0.0f, PANE_W, pane_h }, scroll_y, TextSelection{}, -1, HoveredButtons{}, 1.0f, h_scroll);
    }
};

} // namespace

TEST_F(CullingParityTest, ViewportCullingMatchesUnculledRendering)
{
    const auto pl = ParseAndLayout(CULL_DOC_MD, PANE_W);
    const size_t n = pl.nodes.size();
    ASSERT_GT(n, 10u);
    // 前提: 高さ 0 の空 LI (loose list) を含む。
    ASSERT_TRUE(std::ranges::any_of(std::views::iota(size_t{ 0 }, n), [&](size_t i) {
        return IsEmptyListItemContainer(pl.nodes[i]) && pl.cache[i].height == 0.0f;
    }));
    const int code_idx = FindFirstNodeIndexByType(pl.nodes, NodeType::CodeBlock);
    ASSERT_GE(code_idx, 0);
    // コードブロックの横スクロールバーはホバー中だけ描かれる。
    const BlockHScrollContext h_scroll{ .hovered_block = code_idx };

    // 文書全体が余白付きで収まるペインで描いたものを正解とする (カリングが一切効かない)。
    constexpr float ORACLE_SCROLL = -64.0f;
    const float total_h = pl.cache.Bottom(n - 1) + theme_.margin_top;
    const auto oracle = AllBoxes(Generate(pl, ORACLE_SCROLL, total_h - ORACLE_SCROLL + 64.0f, h_scroll));

    std::vector<float> probes;
    for (size_t i = 0; i < n; i++) {
        for (const float d : { -3.0f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 8.0f, 12.0f, 16.0f, 20.0f, 24.0f }) {
            probes.push_back(pl.cache.Bottom(i) + d);        // ノードが上端をちょうど抜けた直後
            probes.push_back(pl.cache.Top(i) + d);
            probes.push_back(pl.cache.Top(i) - PANE_H - d);  // ノードが下端から入る直前
            probes.push_back(pl.cache.Bottom(i) - PANE_H - d);
        }
    }

    for (const float scroll_y : probes) {
        // 生成側は scroll_y を物理ピクセルにスナップしてから描画 Y を出す (dpi=1 なので整数)。
        const float dy = std::round(scroll_y) - ORACLE_SCROLL;
        const auto expected = VisibleBoxes(oracle, dy);
        const auto actual = VisibleBoxes(AllBoxes(Generate(pl, scroll_y, PANE_H, h_scroll)), 0.0f);
        const auto diff = DiffBoxes(expected, actual);
        if (!diff.empty()) {
            const int first = FindFirstVisibleNodeIndex(pl.cache, n, scroll_y);
            FAIL() << std::format("scroll_y={} (first_visible={}) でカリング結果が不一致:\n", scroll_y, first) << diff;
        }
    }
}

// issue #237 の上端版: loose list の空 LI (高さ 0) は bullet だけがノード下にはみ出して描かれる。
// 空 LI の Top/Bottom を少しだけ通り過ぎたスクロール位置でも bullet は消えない。
TEST_F(CullingParityTest, EmptyLooseListItemBulletSurvivesTopEdge)
{
    const auto pl = ParseAndLayout("- a\n\n- b", PANE_W);
    ASSERT_EQ(pl.nodes.size(), 4u);
    ASSERT_TRUE(IsEmptyListItemContainer(pl.nodes[0]));
    ASSERT_EQ(pl.cache[0].height, 0.0f);

    const auto& cmds = Generate(pl, pl.cache.Top(0) + 2.0f, PANE_H, {});
    EXPECT_EQ(CountCmd<FillEllipseCmd>(cmds), 2) << "1 個目の bullet が消えている";
}
