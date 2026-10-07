#include "renderer.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "ui_constants.h"
#include "profiler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <utility>

#ifdef MENDO_USE_TRACY
namespace {

// 累積カウンタ（UI スレッド単一前提のため非アトミック）。
struct EffectStats {
    int64_t set_drawing_effect = 0;   // ApplyNodeEffects/ApplyTableEffects 内の SetDrawingEffect
    int64_t hittest_range = 0;        // インラインコード背景計算の HitTestTextRange
    int64_t apply_node = 0;           // ApplyNodeEffects 呼び出し
    int64_t apply_table = 0;          // ApplyTableEffects 呼び出し
    int64_t inline_code_bg_added = 0; // ノード/セルに追加されたインラインコード背景数
};
EffectStats g_effect_stats;

void PublishEffectStats() noexcept
{
    MENDO_PLOT("effect.set_drawing_effect", g_effect_stats.set_drawing_effect);
    MENDO_PLOT("effect.hittest_range", g_effect_stats.hittest_range);
    MENDO_PLOT("effect.apply_node", g_effect_stats.apply_node);
    MENDO_PLOT("effect.apply_table", g_effect_stats.apply_table);
    MENDO_PLOT("effect.inline_code_bg_added", g_effect_stats.inline_code_bg_added);
}

} // namespace
#endif

bool Renderer::Init(HWND hwnd)
{
    theme_ = GetLightTheme();

    if (!backend_.Init(hwnd)) {
        return false;
    }

    ResolveThemeFonts();
    RecreateBrushes();
    RecreatePaneFormats();
    LoadAppIconBitmap();

    measurer_.SetFactory(backend_.GetDWriteFactory());
    if (!layout_.Init(&measurer_, theme_)) {
        return false;
    }

    cmd_generator_.SetTheme(&theme_);
    cmd_generator_.SetHitTestBuffer(&hit_test_buffer_);
    // 初回描画での拡大 resize を避けるため、共有バッファを事前に予約しておく。
    hit_test_buffer_.reserve(HIT_TEST_METRICS_INITIAL_CAPACITY);

    return true;
}

void Renderer::SetTheme(const Theme& theme)
{
    theme_ = theme;
    ResolveThemeFonts();
    ApplyThemeMetrics();
    if (!backend_.GetRenderTarget()) {
        return;
    }
    RecreateBrushes();
}

void Renderer::Resize(UINT width, UINT height) noexcept
{
    backend_.Resize(width, height);
}

void Renderer::SetDpi(float dpi) noexcept
{
    backend_.SetDpi(dpi);
    ResetSidePaneCaches();
}

void Renderer::ResetSidePaneCaches() noexcept
{
    for (auto& c : pane_caches_) {
        c.Reset();
    }
}

void Renderer::ApplyZoomFromBase(const Theme& base_theme, float new_zoom)
{
    theme_ = base_theme;
    ResolveThemeFonts();
    if (new_zoom != 1.0f) {
        theme_.ApplyZoom(new_zoom);
    }
    ApplyThemeMetrics();
}

void Renderer::ApplyThemeMetrics()
{
    layout_.UpdateTheme(theme_);
    layout_.RecreateFormats();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
}

// ApplyNodeEffects は IDWriteTextLayout の mutable state (SetDrawingEffect/SetUnderline) を
// 書き換える性質上、値型 DrawCommand には乗せられず、描画前パスとして実行する。
// effects_applied フラグで初回のみ走り、以降のフレームでは no-op。
void Renderer::PrepareVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache, float scroll_y, float md_pane_height)
{
    const float viewport_top = scroll_y;
    const float viewport_bottom = scroll_y + md_pane_height;
    const int first_visible = FindFirstVisibleNodeIndex(cache, nodes.size(), viewport_top);

    const uint32_t effects_gen = cache.GetEffectsGeneration();
    // テーブル行の effects は viewport 範囲に依存するため、スクロール追従が必要。
    // 4px 単位に量子化してサブピクセルスクロールでの過剰再実行を抑制する。
    const EffectsKey key{ effects_gen, first_visible, static_cast<int>(viewport_bottom) >> 2 };
    if (key != last_effects_) {
        MENDO_PROFILE("PrepareVisibleEffects");
        ApplyVisibleEffects(nodes, cache, first_visible, viewport_top, viewport_bottom);
        last_effects_ = key;
    }
}

void Renderer::ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache, int first_visible, float viewport_top, float viewport_bottom)
{
    const int node_count = static_cast<int>(nodes.size());
    for (int i = first_visible; i < node_count; i++) {
        const float entry_top = cache.Top(static_cast<size_t>(i));
        if (entry_top > viewport_bottom) {
            break;
        }
        ApplyNodeEffects(nodes[i], cache[i], entry_top, viewport_top, viewport_bottom);
    }
    MENDO_IF_TRACY(PublishEffectStats());
}

ID2D1SolidColorBrush* Renderer::GetSyntaxBrush(SyntaxTokenType type) const noexcept
{
    static constexpr BrushId SYNTAX_MAP[] = {
        BrushId::Text, // Plain（未使用、フォールバックとしてテキストブラシを返す）
        BrushId::SyntaxKeyword,
        BrushId::SyntaxType,
        BrushId::SyntaxString,
        BrushId::SyntaxNumber,
        BrushId::SyntaxComment,
        BrushId::SyntaxPreprocessor,
        BrushId::SyntaxFunction,
    };
    // SYNTAX_MAP が SyntaxKeyword..SyntaxFunction の連続値前提で書かれていることを担保。
    // brush_id.h で間に新規 BrushId を挿入するとここが落ちて気付ける。
    static_assert(
        std::to_underlying(BrushId::SyntaxFunction) - std::to_underlying(BrushId::SyntaxKeyword) == 6,
        "SYNTAX_MAP は SyntaxKeyword..SyntaxFunction の連続値に依存している");
    const auto idx = std::to_underlying(type);
    if (idx >= std::size(SYNTAX_MAP)) {
        return nullptr;
    }
    return Brush(SYNTAX_MAP[idx]);
}

// DWRITE_HIT_TEST_METRICS からパディング適用済みの InlineCodeBg を生成する。
static InlineCodeBg MakeInlineCodeBg(const DWRITE_HIT_TEST_METRICS& m) noexcept
{
    const D2D1_RECT_F r = RectFromHitTest(m);
    return D2D1::RectF(r.left - INLINE_CODE_PAD_X, r.top - INLINE_CODE_PAD_Y, r.right + INLINE_CODE_PAD_X, r.bottom + INLINE_CODE_PAD_Y);
}

void Renderer::ApplyTableEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom)
{
    MENDO_COUNT_INC(g_effect_stats.apply_table);
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0 || !entry.has_table_layout()) {
        entry.effects_applied = true;
        return;
    }

    const auto row_count = static_cast<size_t>(tbl->row_count);
    const auto col_count = static_cast<size_t>(tbl->col_count);
    auto& tl = *entry.table_layout;
    if (!entry.effects_applied) {
        entry.effects_applied = true;
        tl.cell_inline_code_bgs.clear();
        tl.row_bgs_computed.assign(row_count, 0);
        tl.row_links_applied.assign(row_count, 0);
    }
    else if (tl.row_bgs_computed.size() != row_count || tl.row_links_applied.size() != row_count) {
        tl.row_bgs_computed.resize(row_count, 0);
        tl.row_links_applied.resize(row_count, 0);
    }

    // リンク色・インラインコード背景とも可視行だけに適用し、行単位フラグで再適用を省く。
    // 全行を毎回なめると巨大テーブルで effects 世代が変わるたびに O(行×列) になる。
    const auto [r_begin, r_end] = tl.RowsInViewport(row_count, viewport_top - entry_text_top, viewport_bottom - entry_text_top);

    for (size_t r = r_begin; r < r_end; r++) {
        if (tl.row_links_applied[r] && tl.row_bgs_computed[r]) {
            continue;
        }
        // evict 済みで未復元のセルが残る行は、復元後に改めて適用させるため完了扱いにしない
        // (背景矩形は二重登録を避けるため完了時にだけ積む。リンク色は冪等なので先行適用してよい)。
        const bool row_complete = std::ranges::none_of(std::views::iota(size_t{ 0 }, col_count), [&](size_t c) {
            return !tl.GetCellLayout(r, c) && !tbl->GetCellText(r, c).empty();
        });
        const bool need_links = !tl.row_links_applied[r];
        const bool need_bgs = !tl.row_bgs_computed[r] && row_complete;

        for (size_t c = 0; c < col_count; c++) {
            IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);
            if (!cell_layout) {
                continue;
            }
            // セル全文の UTF-16 化を避け、run 順に前進するカーソルで位置を変換する。
            mendo::Utf16OffsetCursor cursor{ tbl->GetCellText(r, c) };
            for (const auto& run : tbl->GetCellRuns(r, c)) {
                const bool apply_link = need_links && run.has_link();
                const bool add_bg = need_bgs && run.code() && run.length > 0;
                if (!apply_link && !add_bg) {
                    continue;
                }
                const auto range = cursor.WideRange(run.start, run.length);
                if (apply_link) {
                    cell_layout->SetDrawingEffect(Brush(BrushId::Link), range);
                    MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
                }
                if (add_bg) {
                    MENDO_COUNT_INC(g_effect_stats.hittest_range);
                    const UINT32 count = FetchHitTestMetrics(cell_layout, range.startPosition, range.length, hit_test_buffer_);
                    MENDO_COUNT_ADD(g_effect_stats.inline_code_bg_added, count);
                    AppendCellInlineCodeBgs(tl, static_cast<uint32_t>(tl.CellIndex(r, c)), count);
                }
            }
        }
        if (row_complete) {
            tl.row_links_applied[r] = 1;
            tl.row_bgs_computed[r] = 1;
        }
    }
}

// cell_index 昇順を維持するため、新規行が末尾以降ならば append、それ以外 (上方向スクロールで
// 前段の行が後から追加される稀ケース) は末尾に一括 push してから rotate で upper_bound 位置へ移す
// (tail shift が 1 回で済む)。可視ノードが下方向に増える典型ケースは完全 append (O(1)/elem) で済む。
void Renderer::AppendCellInlineCodeBgs(TableLayoutData& tl, uint32_t cell_index, UINT32 count)
{
    auto& bgs = tl.cell_inline_code_bgs;
    const size_t old_size = bgs.size();
    const bool can_append = bgs.empty() || bgs.back().cell_index <= cell_index;
    const size_t insert_at = can_append
                                 ? old_size
                                 : static_cast<size_t>(std::ranges::upper_bound(bgs, cell_index, {}, &CellInlineCodeBg::cell_index) - bgs.begin());
    bgs.reserve(old_size + count);
    for (UINT32 i = 0; i < count; i++) {
        bgs.emplace_back(CellInlineCodeBg{ cell_index, MakeInlineCodeBg(hit_test_buffer_[i]) });
    }
    if (insert_at != old_size) {
        std::rotate(bgs.begin() + insert_at, bgs.begin() + old_size, bgs.end());
    }
}

// 同じ type の隣接トークンをマージして SetDrawingEffect の呼び出し回数を減らす
// (内部で range tree を再構築するため)。
void Renderer::ApplySyntaxEffects(IDWriteTextLayout* layout, const Node& node)
{
    mendo::Utf16OffsetCursor cursor{ node.GetText() };
    SyntaxTokenType pending_type = SyntaxTokenType::Plain;
    uint32_t pending_start = 0;
    uint32_t pending_end = 0;
    const auto flush = [&]() {
        if (pending_type != SyntaxTokenType::Plain && pending_end > pending_start) {
            if (auto* brush = GetSyntaxBrush(pending_type)) {
                layout->SetDrawingEffect(brush, cursor.WideRange(pending_start, pending_end - pending_start));
                MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
            }
        }
        pending_type = SyntaxTokenType::Plain;
    };
    for (const auto& token : node.syntax_tokens()) {
        if (token.type == SyntaxTokenType::Plain) {
            flush();
            continue;
        }
        if (pending_type == token.type && pending_end == token.start) {
            pending_end = token.start + token.length;
            continue;
        }
        flush();
        pending_type = token.type;
        pending_start = token.start;
        pending_end = token.start + token.length;
    }
    flush();
}

void Renderer::ApplyNodeEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom)
{
    // テーブルノード: ビューポートカリング付きの行単位増分処理を行う。
    if (node.type == NodeType::Table) {
        ApplyTableEffects(node, entry, entry_text_top, viewport_top, viewport_bottom);
        return;
    }

    if (entry.effects_applied && !entry.inline_code_bgs_stale) {
        return;
    }
    // SetMaxWidth だけの再計測後は描画エフェクト/下線が残っているので、インラインコード背景だけ作り直す。
    const bool apply_brushes = !entry.effects_applied;
    entry.effects_applied = true;
    entry.inline_code_bgs_stale = false;
    MENDO_COUNT_INC(g_effect_stats.apply_node);

    if (node.type == NodeType::Image || !entry.text_layout) {
        return;
    }
    IDWriteTextLayout* const layout = entry.text_layout.Get();

    // token / alert_label / run の offset/length は UTF-8 byte 単位なので、IDWriteTextLayout が要求する
    // UTF-16 textPosition に変換する。各パス内で byte 位置は単調なので、全文の UTF-16 化はせず
    // パスごとのカーソルで前進させる。
    const std::string_view text = node.GetText();

    if (apply_brushes && node.type == NodeType::CodeBlock) {
        ApplySyntaxEffects(layout, node);
    }

    if (apply_brushes && node.type == NodeType::BlockQuote && node.alert_type != AlertType::None && node.alert_label_length() > 0) {
        const auto idx = AlertColorIndex(node.alert_type);
        if (idx < ALERT_TYPE_COUNT) {
            mendo::Utf16OffsetCursor cursor{ text };
            layout->SetDrawingEffect(Brush(AlertBrushIdFromIndex(idx)), cursor.WideRange(0, node.alert_label_length()));
            MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
        }
    }

    const bool has_inline_code_bgs = node.type != NodeType::CodeBlock;
    mendo::Utf16OffsetCursor cursor{ text };
    for (const auto& run : node.runs) {
        const bool apply_link = apply_brushes && run.has_link();
        const bool add_bg = has_inline_code_bgs && run.code() && run.length > 0;
        if (!apply_link && !add_bg) {
            continue;
        }
        const auto range = cursor.WideRange(run.start, run.length);
        if (apply_link) {
            layout->SetUnderline(TRUE, range);
            layout->SetDrawingEffect(Brush(BrushId::Link), range);
            MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
        }
        if (add_bg) {
            MENDO_COUNT_INC(g_effect_stats.hittest_range);
            const UINT32 count = FetchHitTestMetrics(layout, range.startPosition, range.length, hit_test_buffer_);
            if (count > 0) {
                auto& bgs = entry.ensure_inline_code_bgs();
                MENDO_COUNT_ADD(g_effect_stats.inline_code_bg_added, count);
                for (UINT32 i = 0; i < count; i++) {
                    bgs.emplace_back(MakeInlineCodeBg(hit_test_buffer_[i]));
                }
            }
        }
    }
}

void Renderer::DrawSidePanes(const SidePaneState& sp)
{
    const auto& fp = sp.Get(PaneTarget::File);
    if (fp.show) {
        DrawFileExplorer(sp.file_entries, fp);
        DrawSplitter(fp.rect.x + fp.rect.width, fp.rect.y, fp.rect.y + fp.rect.height);
    }
    const auto& tp = sp.Get(PaneTarget::Toc);
    if (tp.show) {
        DrawToc(sp.toc_entries, sp.nodes, tp, sp.active_toc_index);
        DrawSplitter(tp.rect.x + tp.rect.width, tp.rect.y, tp.rect.y + tp.rect.height);
    }
}

bool Renderer::BeginFrame(const TitleBarRenderState& titlebar, const SidePaneState& side_panes)
{
    if (HandleDeviceLost()) {
        return false;
    }
    if (!rt()) {
        return false;
    }

    // GPU パイプライン詰まり時の CPU バックプレッシャを Present(Vsync) ではなく
    // フレーム頭の Waitable で吸収する。スクロール連打時の遅延を 1 フレーム短縮できる。
    backend_.WaitForFrameLatency();

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    DrawTitleBar(titlebar);
    DrawSidePanes(side_panes);
    return true;
}

void Renderer::DrawLoadingSpinner(float angle, const PaneRect& md_pane_rect)
{
    auto* const text_brush = Brush(BrushId::Text);
    if (!text_brush) {
        return;
    }
    // alpha はドットインデックスのみに依存するためコンパイル時に決定する。
    static constexpr auto kSpinnerAlphas = []() noexcept {
        std::array<float, spinner::DOT_COUNT> a{};
        for (int i = 0; i < spinner::DOT_COUNT; ++i) {
            a[i] = 1.0f - i * (spinner::DOT_FADE_FACTOR / spinner::DOT_COUNT);
        }
        return a;
    }();
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    // ループ内で毎回 SetOpacity を上書きする。guard は scope 終了時の 1.0f 復帰のみ担う。
    mendo::OpacityScope guard{ text_brush, 1.0f };
    for (int i = 0; i < spinner::DOT_COUNT; i++) {
        const float a = angle - i * (TWO_PI / spinner::DOT_COUNT);
        const float dx = cx + spinner::RADIUS * std::cos(a);
        const float dy = cy + spinner::RADIUS * std::sin(a);

        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(dx, dy), spinner::DOT_RADIUS, spinner::DOT_RADIUS);
        text_brush->SetOpacity(kSpinnerAlphas[i]);
        rt()->FillEllipse(ellipse, text_brush);
    }
}

void Renderer::DrawLoading(
    float angle,
    const PaneRect& md_pane_rect,
    const SidePaneState& sp,
    const TitleBarRenderState& titlebar,
    const GestureRenderState& gesture,
    const ToastRenderState& toast)
{
    if (!BeginFrame(titlebar, sp)) {
        return;
    }

    DrawLoadingSpinner(angle, md_pane_rect);

    // ローディング中もジェスチャーオーバーレイは表示する
    if (gesture.overlay_visible) {
        DrawGestureOverlay(gesture.direction, md_pane_rect);
    }

    if (toast.visible) {
        DrawToastOverlay(toast, md_pane_rect);
    }

    CheckEndDraw();
}

void Renderer::Render(const RenderParams& p)
{
    MENDO_PROFILE("Render");

    if (!BeginFrame(p.titlebar, p.side_panes)) {
        return;
    }

    {
        // 開始ノードは GenerateMdPane が描画のはみ出し幅込みで求める (first_visible = -1)。
        const float dpi_scale = DpiScaleFrom(backend_.GetDpi());
        const auto& cmds = cmd_generator_.GenerateMdPane(p.nodes, p.cache, p.md_pane_rect, p.scroll_y, p.selection, -1, p.hovered, dpi_scale, p.block_h_scroll);
        cmd_executor_.Execute(cmds, rt(), &brushes_);
    }

    if (p.can_go_back || p.can_go_forward) {
        DrawNavOverlay(p.md_pane_rect, p.can_go_back, p.can_go_forward, p.nav_hovered);
    }

    if (p.gesture.trail_active && p.gesture.trail_points && p.gesture.trail_points->size() >= 2) {
        DrawGestureTrail(*p.gesture.trail_points);
    }

    if (p.gesture.overlay_visible) {
        DrawGestureOverlay(p.gesture.direction, p.md_pane_rect);
    }

    if (p.toast.visible) {
        DrawToastOverlay(p.toast, p.md_pane_rect);
    }

    if (p.search_bar.visible) {
        DrawSearchBar(p.search_bar, p.md_pane_rect);
    }

    DrawMdScrollbar(p.md_pane_rect, p.scroll_y, p.total_content_height, p.has_dirty_nodes);

    CheckEndDraw();
}

void Renderer::CheckEndDraw()
{
    const HRESULT hr = rt()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        RecreateRenderTarget();
        InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
        return;
    }
    if (SUCCEEDED(hr)) {
        backend_.Present();
        if (backend_.IsDeviceLost()) {
            // REMOVED/RESET/HUNG/DRIVER_INTERNAL_ERROR で backend が device_lost_ をセット済み。
            // 次フレーム冒頭の HandleDeviceLost で再作成するため再描画を予約する。
            InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
        }
    }
}

bool Renderer::HandleDeviceLost()
{
    if (!backend_.IsDeviceLost()) {
        return false;
    }
    if (RecreateRenderTarget()) {
        InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
    }
    return true;
}

bool Renderer::RecreateRenderTarget()
{
    if (!backend_.RecreateRenderTarget()) {
        return false;
    }

    // 旧レンダーターゲットに紐付いたブラシは全て無効化し、RecreateBrushes で再生成する
    InvalidateBrushes();
    RecreateBrushes();
    LoadAppIconBitmap();
    ResetSidePaneCaches();
    // 旧 RT に紐付いたブラシプールと bound RT を捨てる
    cmd_executor_ = CommandExecutor{};

    if (on_device_lost_) {
        on_device_lost_(backend_.GetRenderTarget());
    }

    return true;
}
