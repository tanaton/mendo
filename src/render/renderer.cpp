#include "renderer.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "syntax.h"
#include "ui_constants.h"
#include "profiler.h"
#include <algorithm>
#include <cmath>
#include <optional>
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

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

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
    cmd_generator_.SetFormats({ fmt_.list_number.Get(), fmt_.icon_font.Get(), fmt_.copy_btn_icon.Get(), fmt_.placeholder_text.Get() });
    cmd_generator_.SetHitTestBuffer(&hit_test_buffer_);
    // 初回描画での拡大 resize を避けるため、共有バッファを事前に予約しておく。
    hit_test_buffer_.reserve(HIT_TEST_METRICS_INITIAL_CAPACITY);

    return true;
}

void Renderer::SetTheme(const Theme& theme)
{
    theme_ = theme;
    ResolveThemeFonts();
    UpdateLayoutTheme();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
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
    UpdateLayoutTheme();
    RecreatePaneFormats();
    cmd_generator_.SetTheme(&theme_);
}

void Renderer::UpdateLayoutTheme()
{
    layout_.UpdateTheme(theme_);
    layout_.RecreateFormats();
}

// ノード描画ロジックは CommandGenerator に抽出済み。ApplyNodeEffects は
// IDWriteTextLayout の mutable state (SetDrawingEffect/SetUnderline) を書き換える性質上、
// 値型 DrawCommand には乗せられず、描画前パスとしてここに残る。
// effects_applied フラグで初回のみ走り、以降のフレームでは no-op。

void Renderer::PrepareVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache, float scroll_y, float md_pane_height)
{
    const float viewport_top = scroll_y;
    const float viewport_bottom = scroll_y + md_pane_height;
    const int first_visible = FindFirstVisibleNodeIndex(cache, nodes.size(), viewport_top);

    const uint32_t effects_gen = cache.GetEffectsGeneration();
    // テーブル行の effects は viewport 範囲に依存するため、スクロール追従が必要。
    // 4px 単位に量子化してサブピクセルスクロールでの過剰再実行を抑制する。
    const int viewport_bottom_q = static_cast<int>(viewport_bottom) >> 2;
    if (effects_gen != last_effects_gen_ || first_visible != last_effects_first_ || viewport_bottom_q != last_effects_bottom_q_) {
        MENDO_PROFILE("PrepareVisibleEffects");
        ApplyVisibleEffects(nodes, cache, first_visible, viewport_top, viewport_bottom);
        last_effects_gen_ = effects_gen;
        last_effects_first_ = first_visible;
        last_effects_bottom_q_ = viewport_bottom_q;
    }
}

void Renderer::ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache, int first_visible, float viewport_top, float viewport_bottom)
{
    const int node_count = static_cast<int>(nodes.size());
    for (int i = first_visible; i < node_count; i++) {
        if (cache[i].text_top > viewport_bottom) {
            break;
        }
        ApplyNodeEffects(nodes[i], cache[i], cache[i].text_top, viewport_top, viewport_bottom);
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
    return D2D1::RectF(
        m.left - INLINE_CODE_PAD_X,
        m.top - INLINE_CODE_PAD_Y,
        m.left + m.width + INLINE_CODE_PAD_X,
        m.top + m.height + INLINE_CODE_PAD_Y);
}

void Renderer::ApplyTableEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom)
{
    MENDO_COUNT_INC(g_effect_stats.apply_table);
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0 || !entry.has_table_layout()) {
        entry.effects_applied = true;
        return;
    }

    const bool first_pass = !entry.effects_applied;
    const auto row_count = static_cast<size_t>(tbl->row_count);
    const auto col_count = static_cast<size_t>(tbl->col_count);
    auto& tl = *entry.table_layout;
    if (first_pass) {
        entry.effects_applied = true;
        tl.cell_inline_code_bgs.clear();
        tl.row_bgs_computed.resize(row_count, 0);
    }

    const float border = TABLE_BORDER_WIDTH;

    // 2回目以降のパスは row_cum_y で可視範囲を二分探索し、その帯だけループする。
    // 1万行テーブルの大半をスキップしていたフレーム毎の row_count 線形 continue を排除する。
    size_t r_begin = 0;
    size_t r_end = row_count;
    const bool cull_by_viewport = !first_pass && viewport_top >= 0.0f && tl.row_cum_y.size() == row_count + 1;
    if (cull_by_viewport) {
        const float local_top = viewport_top - entry_text_top;
        const float local_bottom = viewport_bottom - entry_text_top;
        // row_cum_y[r+1] は行 r の下端 (= 次行の上端、border 込み)。最初に local_top を超える
        // 境界点を upper_bound で求め、その 1 つ手前が viewport 先頭にかかる行。
        auto upper = std::ranges::upper_bound(tl.row_cum_y, local_top);
        if (upper != tl.row_cum_y.begin()) {
            r_begin = static_cast<size_t>(std::ranges::distance(tl.row_cum_y.begin(), upper)) - 1;
        }
        auto lower = std::ranges::lower_bound(tl.row_cum_y, local_bottom);
        if (lower != tl.row_cum_y.end()) {
            const auto idx = static_cast<size_t>(std::ranges::distance(tl.row_cum_y.begin(), lower));
            r_end = std::min(row_count, idx);
        }
    }

    float row_y = entry_text_top;
    if (r_begin > 0) {
        row_y = entry_text_top + tl.row_cum_y[r_begin];
    }

    for (size_t r = r_begin; r < r_end; r++) {
        const float row_h = (r < tl.row_heights.size()) ? tl.row_heights[r] : (theme_.font_size_body * TABLE_ROW_HEIGHT_FACTOR);
        const float row_bottom = row_y + row_h + border;

        const bool row_visible = (viewport_top < 0.0f) || (row_bottom >= viewport_top && row_y <= viewport_bottom);
        // 2回目以降のパスではオフスクリーン行の背景走査をスキップ
        if (!first_pass && !row_visible) {
            row_y = row_bottom;
            continue;
        }
        // row_bgs_computed フラグで O(1) 判定（O(cells × runs) の走査を排除）
        const bool bgs_done = !first_pass && r < tl.row_bgs_computed.size() && tl.row_bgs_computed[r];
        const bool need_bgs = row_visible && !bgs_done;

        // 2回目以降: インラインコード背景の計算が不要な行はスキップ
        if (!first_pass && !need_bgs) {
            row_y = row_bottom;
            continue;
        }

        for (size_t c = 0; c < col_count; c++) {
            IDWriteTextLayout* cell_layout = tl.GetCellLayout(r, c);
            if (!cell_layout) {
                continue;
            }
            const auto& runs = tbl->GetCellRuns(r, c);
            if (runs.empty()) {
                continue;
            }
            // WideView は UTF-8→UTF-16 decode + offsets テーブルを伴うため、
            // link/code を一切含まないセル (大半を占める) では構築を回避する。
            std::optional<mendo::WideViewForDWrite> wv;
            for (const auto& run : runs) {
                // リンク色: 初回パスで全行に適用（軽量・冪等）
                if (first_pass && run.has_link()) {
                    if (!wv) {
                        wv.emplace(tbl->GetCellText(r, c));
                    }
                    const auto range = wv->WideRange(run.start, run.length);
                    cell_layout->SetDrawingEffect(Brush(BrushId::Link), range);
                    MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
                }
                // インラインコード背景: 可視かつ未計算の行のみ。
                // cell_index 昇順を維持するため、新規行が末尾以降ならば append、
                // それ以外（上方向スクロールで前段の行が後から追加される稀ケース）は
                // upper_bound 位置に insert する。可視ノードが下方向に増える典型ケースは
                // 完全 append (O(1)/elem) で済む。
                if (need_bgs && run.code() && run.length > 0) {
                    if (!wv) {
                        wv.emplace(tbl->GetCellText(r, c));
                    }
                    MENDO_COUNT_INC(g_effect_stats.hittest_range);
                    const auto wr = wv->WideRange(run.start, run.length);
                    const UINT32 count = FetchHitTestMetrics(cell_layout, wr.startPosition, wr.length, hit_test_buffer_);
                    MENDO_COUNT_ADD(g_effect_stats.inline_code_bg_added, count);
                    const auto cell_index = static_cast<uint32_t>(tl.CellIndex(r, c));
                    auto& bgs = tl.cell_inline_code_bgs;
                    const bool can_append = bgs.empty() || bgs.back().cell_index <= cell_index;
                    if (can_append) {
                        bgs.reserve(bgs.size() + count);
                        for (UINT32 hi = 0; hi < count; hi++) {
                            bgs.emplace_back(CellInlineCodeBg{ cell_index, MakeInlineCodeBg(hit_test_buffer_[hi]) });
                        }
                    }
                    else {
                        const auto func = [](uint32_t v, const CellInlineCodeBg& e) noexcept { return v < e.cell_index; };
                        // 上方向スクロールで前段の行が後から追加される稀ケース。
                        // 末尾に一括 push してから rotate で正しい位置に移すと、tail shift が 1 回で済む。
                        const size_t insert_at_index = static_cast<size_t>(std::upper_bound(bgs.begin(), bgs.end(), cell_index, func) - bgs.begin());
                        bgs.reserve(bgs.size() + count);
                        const size_t old_size = bgs.size();
                        for (UINT32 hi = 0; hi < count; hi++) {
                            bgs.emplace_back(CellInlineCodeBg{ cell_index, MakeInlineCodeBg(hit_test_buffer_[hi]) });
                        }
                        std::rotate(bgs.begin() + insert_at_index, bgs.begin() + old_size, bgs.end());
                    }
                }
            }
        }
        // この行のインラインコード背景計算が完了したことを記録
        if (need_bgs && r < tl.row_bgs_computed.size()) {
            tl.row_bgs_computed[r] = true;
        }
        row_y = row_bottom;
    }
}

// ApplyNodeEffects は IDWriteTextLayout の内部状態 (SetDrawingEffect/SetUnderline) を直接書き換える
// pre-render パス。CommandGenerator の DrawCommand には乗らない (text-layout 状態は不可搬なため)。
void Renderer::ApplyNodeEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom)
{
    // テーブルノード: ビューポートカリング付きの増分処理を行う。
    // リンク色は全行に適用（軽量・冪等）、インラインコード背景は可視行のみ計算する。
    if (node.type == NodeType::Table) {
        ApplyTableEffects(node, entry, entry_text_top, viewport_top, viewport_bottom);
        return;
    }

    if (entry.effects_applied) {
        return;
    }
    entry.effects_applied = true;
    MENDO_COUNT_INC(g_effect_stats.apply_node);

    if (node.type == NodeType::Image) {
        return;
    }

    if (!entry.text_layout) {
        return;
    }

    // run / token / alert_label の offset/length は UTF-8 byte 単位なので、
    // IDWriteTextLayout が要求する UTF-16 textPosition に変換する。
    const mendo::WideViewForDWrite wv{ node.GetText() };

    // 同じ type の隣接トークンをマージして SetDrawingEffect の呼び出し回数を減らす
    // (内部で range tree を再構築するため)。
    if (node.type == NodeType::CodeBlock) {
        SyntaxTokenType pending_type = SyntaxTokenType::Plain;
        uint32_t pending_start = 0;
        uint32_t pending_end = 0;
        const auto flush = [&]() {
            if (pending_type != SyntaxTokenType::Plain && pending_end > pending_start) {
                if (auto* brush = GetSyntaxBrush(pending_type)) {
                    const auto range = wv.WideRange(pending_start, pending_end - pending_start);
                    entry.text_layout->SetDrawingEffect(brush, range);
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
            }
            else {
                flush();
                pending_type = token.type;
                pending_start = token.start;
                pending_end = token.start + token.length;
            }
        }
        flush();
    }

    if (node.type == NodeType::BlockQuote && node.alert_type != AlertType::None && node.alert_label_length() > 0) {
        static constexpr BrushId ALERT_BRUSH[] = {
            BrushId::AlertNote,
            BrushId::AlertTip,
            BrushId::AlertImportant,
            BrushId::AlertWarning,
            BrushId::AlertCaution,
        };
        static_assert(std::size(ALERT_BRUSH) == ALERT_TYPE_COUNT);
        const auto idx = AlertColorIndex(node.alert_type);
        if (idx < ALERT_TYPE_COUNT) {
            const auto range = wv.WideRange(0, node.alert_label_length());
            entry.text_layout->SetDrawingEffect(Brush(ALERT_BRUSH[idx]), range);
            MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
        }
    }

    for (const auto& run : node.runs) {
        if (run.has_link()) {
            const auto range = wv.WideRange(run.start, run.length);
            entry.text_layout->SetUnderline(TRUE, range);
            entry.text_layout->SetDrawingEffect(Brush(BrushId::Link), range);
            MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
        }
        if (run.code() && node.type != NodeType::CodeBlock && run.length > 0) {
            MENDO_COUNT_INC(g_effect_stats.hittest_range);
            const auto wr = wv.WideRange(run.start, run.length);
            const UINT32 count = FetchHitTestMetrics(entry.text_layout.Get(), wr.startPosition, wr.length, hit_test_buffer_);
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
        DrawFileExplorer(sp.file_entries, fp.rect, fp.scroll, fp.hovered_index, fp.close_hovered, fp.refresh_hovered);
        DrawSplitter(fp.rect.x + fp.rect.width, fp.rect.y, fp.rect.y + fp.rect.height);
    }
    const auto& tp = sp.Get(PaneTarget::Toc);
    if (tp.show) {
        DrawToc(sp.toc_entries, sp.nodes, tp.rect, tp.scroll, tp.hovered_index, tp.close_hovered, sp.active_toc_index);
        DrawSplitter(tp.rect.x + tp.rect.width, tp.rect.y, tp.rect.y + tp.rect.height);
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
    if (HandleDeviceLost()) {
        return;
    }
    if (!rt()) {
        return;
    }

    // GPU パイプライン詰まり時の CPU バックプレッシャを Present(Vsync) ではなく
    // フレーム頭の Waitable で吸収する。スクロール連打時の遅延を 1 フレーム短縮できる。
    backend_.WaitForFrameLatency();

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    DrawTitleBar(titlebar);

    DrawSidePanes(sp);

    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    // alpha はドットインデックスのみに依存するためコンパイル時に決定する。
    static constexpr auto kSpinnerAlphas = []() noexcept {
        std::array<float, spinner::DOT_COUNT> a{};
        for (int i = 0; i < spinner::DOT_COUNT; ++i) {
            a[i] = 1.0f - i * (spinner::DOT_FADE_FACTOR / spinner::DOT_COUNT);
        }
        return a;
    }();
    if (auto* const text_brush = Brush(BrushId::Text)) {
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

    // ローディング中でもフェードアウト中は表示
    if (gesture.overlay_visible && gesture.overlay_alpha > 0.0f) {
        DrawGestureOverlay(gesture.direction, gesture.overlay_alpha, md_pane_rect);
    }

    if (toast.visible) {
        DrawToastOverlay(toast, md_pane_rect);
    }

    if (!CheckEndDraw()) {
        return;
    }
}

void Renderer::Render(const RenderParams& p)
{
    MENDO_PROFILE("Render");

    if (HandleDeviceLost()) {
        return;
    }
    if (!rt()) {
        return;
    }

    backend_.WaitForFrameLatency();

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);

    DrawTitleBar(p.titlebar);

    DrawSidePanes(p.side_panes);

    {
        // ヒットテストとの座標一致のためスナップ前の scroll_y を使う。
        const float viewport_top = p.scroll_y;
        const int first_visible = FindFirstVisibleNodeIndex(p.cache, p.nodes.size(), viewport_top);
        const float dpi_scale = DpiScaleFrom(backend_.GetDpi());
        const auto& cmds = cmd_generator_.GenerateMdPane(p.nodes, p.cache, p.md_pane_rect, p.scroll_y, p.selection, first_visible, p.hovered, dpi_scale, p.block_h_scroll);

        cmd_executor_.Execute(cmds, rt(), &fixed_brushes_cache_);
    }

    if (p.can_go_back || p.can_go_forward) {
        DrawNavOverlay(p.md_pane_rect, p.can_go_back, p.can_go_forward, p.nav_hovered);
    }

    if (p.gesture.trail_active && p.gesture.trail_points && p.gesture.trail_points->size() >= 2) {
        DrawGestureTrail(*p.gesture.trail_points);
    }

    if (p.gesture.overlay_visible && p.gesture.overlay_alpha > 0.0f) {
        DrawGestureOverlay(p.gesture.direction, p.gesture.overlay_alpha, p.md_pane_rect);
    }

    if (p.toast.visible) {
        DrawToastOverlay(p.toast, p.md_pane_rect);
    }

    if (p.search_bar.visible) {
        DrawSearchBar(p.search_bar, p.md_pane_rect);
    }

    DrawMdScrollbar(p.md_pane_rect, p.scroll_y, p.total_content_height, p.has_dirty_nodes);

    if (!CheckEndDraw()) {
        return;
    }
}

bool Renderer::CheckEndDraw()
{
    const HRESULT hr = rt()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        RecreateRenderTarget();
        InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
        return false;
    }
    if (SUCCEEDED(hr)) {
        backend_.Present();
        if (backend_.IsDeviceLost()) {
            // REMOVED/RESET/HUNG/DRIVER_INTERNAL_ERROR で backend が device_lost_ をセット済み。
            // 次フレーム冒頭の HandleDeviceLost で再作成するため再描画を予約する。
            InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
            return false;
        }
    }
    return SUCCEEDED(hr);
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
    for (auto& c : pane_caches_) {
        c.Reset();
    }
    // バインドされたレンダーターゲットをリセットする
    cmd_executor_ = CommandExecutor{};

    if (on_device_lost_) {
        on_device_lost_(backend_.GetRenderTarget());
    }

    return true;
}
