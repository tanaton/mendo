#include "renderer.h"
#include "d2d_util.h"
#include "doc_dwrite_bridge.h"
#include "hit_test_service.h"
#include "ui_constants.h"
#include "profiler.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <ranges>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

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

bool Renderer::Init(HWND hwnd, const Theme& theme)
{
    theme_ = theme;

    if (!backend_.Init(hwnd)) {
        return false;
    }

    ResolveThemeFonts();
    RecreatePaneFormats();
    DecodeAppIcon();

    measurer_.SetFactory(backend_.GetDWriteFactory());
    if (!layout_.Init(&measurer_, theme_)) {
        return false;
    }

    cmd_generator_.SetTheme(&theme_);
    cmd_generator_.SetHitTestBuffer(&hit_test_buffer_);
    cmd_generator_.SetTextLayoutCache(&text_layouts_);
    // 初回描画での拡大 resize を避けるため、共有バッファを事前に予約しておく。
    hit_test_buffer_.reserve(HIT_TEST_METRICS_INITIAL_CAPACITY);

    return true;
}

void Renderer::SetTheme(const Theme& theme)
{
    assert(theme.zoom == theme_.zoom && theme.font_size_body == theme_.font_size_body);
    theme_ = theme;
    ResolveThemeFonts();
    cmd_generator_.SetTheme(&theme_);
    RecreateBrushes();
}

void Renderer::Resize(UINT width, UINT height) noexcept
{
    backend_.Resize(width, height);
}

void Renderer::SetDpi(float dpi) noexcept
{
    backend_.SetDpi(dpi);
}

void Renderer::TrimForMinimize() noexcept
{
    backend_.Trim();
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
// 適用済みのノードとテーブル行はフラグで即座に飛ばすので、毎フレーム可視範囲をなめてよい。
void Renderer::PrepareVisibleEffects(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float scroll_y, float md_pane_height)
{
    MENDO_PROFILE("PrepareVisibleEffects");
    // ブラシが無い間に適用済みにすると、色の抜けた状態が残る。
    if (!rt()) {
        return;
    }
    const float viewport_top = scroll_y;
    const float viewport_bottom = scroll_y + md_pane_height;
    const int first_visible = FindFirstVisibleNodeIndex(cache, viewport_top);
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

// DWRITE_HIT_TEST_METRICS からパディング適用済みの InlineCodeBg を生成する。
static InlineCodeBg MakeInlineCodeBg(const DWRITE_HIT_TEST_METRICS& m) noexcept
{
    const D2D1_RECT_F r = RectFromHitTest(m);
    return D2D1::RectF(r.left - INLINE_CODE_PAD_X, r.top - INLINE_CODE_PAD_Y, r.right + INLINE_CODE_PAD_X, r.bottom + INLINE_CODE_PAD_Y);
}

void Renderer::ApplyTableEffects(const Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom)
{
    MENDO_COUNT_INC(g_effect_stats.apply_table);
    // 未計測のテーブルには適用先のセル layout が無いので、済みの印を付けずに戻る。
    auto* const tl_ptr = entry.table_layout();
    if (!tl_ptr) {
        return;
    }
    const auto* tbl = node.table_data();
    if (!tbl || tbl->row_count == 0) {
        entry.set_effects_applied(true);
        return;
    }

    const auto row_count = static_cast<size_t>(tbl->row_count);
    const auto col_count = static_cast<size_t>(tbl->col_count);
    auto& tl = *tl_ptr;
    // セルを作り直す経路 (ClearEffects / MeasureTable) は effects_applied を倒し、ここで行フラグを作り直すので、
    // 済みの間は行フラグが row_count 分ある。確保が例外で途切れても済みにならないよう印は最後に付ける。
    if (!entry.effects_applied()) {
        tl.cell_inline_code_bgs.clear();
        tl.row_bgs_computed.assign(row_count, 0);
        tl.row_links_applied.assign(row_count, 0);
        entry.set_effects_applied(true);
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
            Utf16OffsetCursor cursor{ tbl->GetCellText(r, c) };
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
    const size_t insert_at = can_append ? old_size : static_cast<size_t>(std::ranges::upper_bound(bgs, cell_index, {}, &CellInlineCodeBg::cell_index) - bgs.begin());
    bgs.reserve(old_size + count);
    for (UINT32 i = 0; i < count; i++) {
        bgs.emplace_back(CellInlineCodeBg{ cell_index, MakeInlineCodeBg(hit_test_buffer_[i]) });
    }
    if (insert_at != old_size) {
        std::rotate(bgs.begin() + insert_at, bgs.begin() + old_size, bgs.end());
    }
}

void Renderer::ApplyNodeEffects(const Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom)
{
    // テーブルノード: ビューポートカリング付きの行単位増分処理を行う。
    if (node.type == NodeType::Table) {
        ApplyTableEffects(node, entry, entry_text_top, viewport_top, viewport_bottom);
        return;
    }
    // コードブロックは run もインラインコード背景も持たず、構文色は描画時に付ける。
    if (node.type == NodeType::CodeBlock) {
        return;
    }

    // 適用先の text_layout が無いノードには済みの印を付けない。
    IDWriteTextLayout* const layout = entry.text_layout();
    if (!layout) {
        return;
    }
    if (entry.effects_applied() && !entry.inline_code_bgs_stale()) {
        return;
    }
    // SetMaxWidth だけの再計測後は描画エフェクト/下線が残っているので、インラインコード背景だけ作り直す。
    const bool apply_brushes = !entry.effects_applied();
    entry.set_effects_applied(true);
    entry.set_inline_code_bgs_stale(false);
    MENDO_COUNT_INC(g_effect_stats.apply_node);

    // alert_label / run の offset/length は UTF-8 byte 単位なので、IDWriteTextLayout が要求する
    // UTF-16 textPosition に変換する。各パス内で byte 位置は単調なので、全文の UTF-16 化はせず
    // パスごとのカーソルで前進させる。
    const std::string_view text = node.GetText();

    if (apply_brushes && node.type == NodeType::BlockQuote && node.alert_label_length() > 0) {
        if (const auto idx = AlertColorIndex(node.alert_type); idx < ALERT_TYPE_COUNT) {
            Utf16OffsetCursor cursor{ text };
            layout->SetDrawingEffect(Brush(AlertBrushIdFromIndex(idx)), cursor.WideRange(0, node.alert_label_length()));
            MENDO_COUNT_INC(g_effect_stats.set_drawing_effect);
        }
    }

    Utf16OffsetCursor cursor{ text };
    for (const auto& run : node.runs()) {
        const bool apply_link = apply_brushes && run.has_link();
        const bool add_bg = run.code() && run.length > 0;
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

void Renderer::PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& sp)
{
    chrome_cmds_.clear();
    GenTitleBar(titlebar);

    const auto gen_splitter = [&](const PaneRect& r) {
        const float x = r.x + r.width;
        chrome_cmds_.push_back(FillRectCmd{ .rect = D2D1::RectF(x, r.y, x + theme_.splitter_width, r.y + r.height), .brush_id = BrushId::Splitter });
    };
    const auto& fp = sp.Get(PaneTarget::File);
    if (fp.show) {
        GenFileExplorer(sp.file_entries, fp);
        gen_splitter(fp.rect);
    }
    const auto& tp = sp.Get(PaneTarget::Toc);
    if (tp.show) {
        GenToc(sp.toc_entries, sp.nodes, tp, sp.active_toc_index);
        gen_splitter(tp.rect);
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

    text_layouts_.BeginFrame();
    PrepareChrome(titlebar, side_panes);
    // GPU パイプライン詰まり時の CPU バックプレッシャを Present(Vsync) ではなく
    // フレーム頭の Waitable で吸収する。スクロール連打時の遅延を 1 フレーム短縮できる。
    backend_.WaitForFrameLatency();

    rt()->BeginDraw();
    rt()->Clear(theme_.bg_color);
    cmd_executor_.Execute(chrome_cmds_, rt(), &brushes_);
    return true;
}

void Renderer::DrawLoadingSpinner(float angle, const PaneRect& md_pane_rect)
{
    auto* const text_brush = Brush(BrushId::Text);
    if (!text_brush) {
        return;
    }
    // alpha はドットインデックスのみに依存するためコンパイル時に決定する。
    static constexpr auto kSpinnerAlphas = []() static noexcept {
        std::array<float, spinner::DOT_COUNT> a{};
        for (int i = 0; i < spinner::DOT_COUNT; ++i) {
            a[i] = 1.0f - i * (spinner::DOT_FADE_FACTOR / spinner::DOT_COUNT);
        }
        return a;
    }();
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    // ループ内で毎回 SetOpacity を上書きする。guard は scope 終了時の 1.0f 復帰のみ担う。
    OpacityScope guard{ text_brush, 1.0f };
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

    if (!toast.message.empty()) {
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
        const float dpi_scale = DpiScaleFrom(backend_.GetDpi());
        const auto& cmds = cmd_generator_.GenerateMdPane(p.nodes, p.cache, p.md_pane_rect, p.scroll_y, p.selection, p.hovered, dpi_scale, p.block_h_scroll);
        MENDO_PLOT("draw.command_count", static_cast<int64_t>(cmds.size()));
        cmd_executor_.Execute(cmds, rt(), &brushes_);
    }

    if (p.can_go_back || p.can_go_forward) {
        DrawNavOverlay(p.md_pane_rect, p.can_go_back, p.can_go_forward, p.nav_hovered);
    }

    if (p.gesture.trail_active && p.gesture.trail_points) {
        DrawGestureTrail(*p.gesture.trail_points);
    }

    if (p.gesture.overlay_visible) {
        DrawGestureOverlay(p.gesture.direction, p.md_pane_rect);
    }

    if (!p.toast.message.empty()) {
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
        RecreateDevice();
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
    if (RecreateDevice()) {
        InvalidateRect(backend_.GetHwnd(), nullptr, FALSE);
    }
    return true;
}

bool Renderer::CreateDevice()
{
    if (!backend_.CreateDevice()) {
        return false;
    }
    RecreateBrushes();
    LoadAppIconBitmap();
    return true;
}

bool Renderer::RecreateDevice()
{
    // 旧レンダーターゲットに紐付いたブラシと動的色ブラシを捨てる
    InvalidateBrushes();
    cmd_executor_ = CommandExecutor{};
    if (!CreateDevice()) {
        return false;
    }
    if (on_device_lost_) {
        on_device_lost_(backend_.GetRenderTarget());
    }
    return true;
}

void Renderer::GenTitleBar(const TitleBarRenderState& tb)
{
    if (tb.height <= 0.0f) {
        return;
    }

    // 完全不透明でガラス効果を隠す
    chrome_cmds_.push_back(FillRectCmd{ .rect = D2D1::RectF(0.0f, 0.0f, tb.window_width, tb.height), .brush_id = BrushId::TitleBarBg });

    const float text_alpha = tb.window_active ? 1.0f : 0.5f;
    const auto is_hovered = [&](TitleBarHitZone z) noexcept { return tb.hovered_zone == z; };

    const auto gen_button = [&](const DipRect& dip_rect, wchar_t icon, bool show_bg, BrushId bg_id, BrushId text_id, float alpha) {
        const D2D1_RECT_F rect = ToD2DRect(dip_rect);
        if (show_bg) {
            chrome_cmds_.push_back(FillRectCmd{ .rect = rect, .brush_id = bg_id });
        }
        EmitChromeText({ &icon, 1 }, fmt_.titlebar_icon.Get(), rect, text_id, alpha);
    };

    const auto gen_plain = [&](TitleBarHitZone zone, wchar_t icon) {
        gen_button(tb.Button(zone), icon, is_hovered(zone), BrushId::TitleBarButtonHover, BrushId::TitleBarText, text_alpha);
    };
    // active > hover の優先度
    const auto gen_toggle = [&](TitleBarHitZone zone, wchar_t icon, bool active) {
        gen_button(
            tb.Button(zone),
            icon,
            active || is_hovered(zone),
            active ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
            BrushId::TitleBarText,
            text_alpha);
    };

    gen_plain(TitleBarHitZone::OpenFile, L'\uE838');
    gen_plain(TitleBarHitZone::Help, L'\uE897');
    gen_plain(TitleBarHitZone::ThemeToggle, tb.is_dark_mode ? L'\uE706' : L'\uE708');
    gen_toggle(TitleBarHitZone::Search, L'\uE721', tb.search_active);
    gen_toggle(TitleBarHitZone::FileToggle, L'\uE8B7', tb.file_pane_visible);
    gen_toggle(TitleBarHitZone::TocToggle, L'\uE8FD', tb.toc_pane_visible);
    gen_plain(TitleBarHitZone::Minimize, L'\uE921');
    gen_plain(TitleBarHitZone::Maximize, tb.is_maximized ? L'\uE923' : L'\uE922');
    if (is_hovered(TitleBarHitZone::Close)) {
        gen_button(tb.Button(TitleBarHitZone::Close), L'\uE8BB', true, BrushId::TitleBarCloseRed, BrushId::TitleBarCloseWhite, 1.0f);
    }
    else {
        gen_plain(TitleBarHitZone::Close, L'\uE8BB');
    }

    if (app_icon_bitmap_) {
        chrome_cmds_.push_back(DrawBitmapCmd{ .bitmap = app_icon_bitmap_.Get(), .dest = ToD2DRect(tb.icon_rect), .opacity = text_alpha });
    }

    if (!tb.title_text.empty()) {
        EmitChromeText(tb.title_text, fmt_.titlebar_text.Get(), ToD2DRect(tb.title_text_rect), BrushId::TitleBarText, text_alpha);
    }
}

void Renderer::EmitChromeText(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float opacity, D2D1_SIZE_F box)
{
    if (auto* layout = text_layouts_.Get(fmt, text, box)) {
        chrome_cmds_.push_back(DrawTextLayoutCmd{ .origin = TextLayoutCache::Origin(rect, box), .layout = layout, .brush_id = brush_id, .opacity = opacity });
    }
}

void Renderer::DrawCenteredText(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box)
{
    auto* const brush = Brush(brush_id);
    auto* const layout = text_layouts_.Get(fmt, text, box);
    if (!brush || !layout) {
        return;
    }
    OpacityScope guard{ brush, alpha };
    rt()->DrawTextLayout(TextLayoutCache::Origin(rect, box), layout, brush);
}

void Renderer::DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, NavButtonHover hovered)
{
    const bool is_dark = theme_.IsDark();

    // SetColor は SetOpacity より重いため、固定色ブラシを is_dark で選んで
    // 透明度のみ切り替える。
    const BrushId overlay_id = is_dark ? BrushId::OverlayWhite : BrushId::OverlayBlack;
    ID2D1SolidColorBrush* const overlay_brush = Brush(overlay_id);

    if (!overlay_brush) {
        return;
    }

    const auto draw_button = [&](const D2D1_RECT_F& rect, bool enabled, bool is_hovered, std::wstring_view arrow) {
        float bg_alpha;
        if (!enabled) {
            bg_alpha = is_dark ? 0.08f : 0.05f;
        }
        else if (is_hovered) {
            bg_alpha = is_dark ? 0.35f : 0.25f;
        }
        else {
            bg_alpha = is_dark ? 0.15f : 0.10f;
        }

        {
            OpacityScope guard{ overlay_brush, bg_alpha };
            const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, NAV_BTN_CORNER, NAV_BTN_CORNER);
            rt()->FillRoundedRectangle(rrect, overlay_brush);
        }

        float text_alpha;
        if (!enabled) {
            text_alpha = is_dark ? 0.2f : 0.15f;
        }
        else if (is_hovered) {
            text_alpha = 1.0f;
        }
        else {
            text_alpha = is_dark ? 0.6f : 0.5f;
        }
        DrawCenteredText(arrow, fmt_.nav_button.Get(), rect, overlay_id, text_alpha, D2D1::SizeF(NAV_BTN_SIZE, NAV_BTN_SIZE));
    };

    // クリック判定 (NavButtonHitTest) と同じ矩形 API を使い、描画とヒットのズレを防ぐ
    draw_button(NavBackButtonRect(md_pane_rect), can_back, hovered == NavButtonHover::Back, L"\x25C0");
    draw_button(NavForwardButtonRect(md_pane_rect), can_forward, hovered == NavButtonHover::Forward, L"\x25B6");
}

void Renderer::DrawGestureTrail(const std::pmr::deque<GesturePoint>& points)
{
    if (points.size() < 2) {
        return;
    }
    auto* const trail_brush = Brush(BrushId::GestureTrail);
    if (!trail_brush || !d2d()) {
        return;
    }

    // パスジオメトリで一筆描きすることで、結合部のアルファ蓄積（節）を防ぐ
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(d2d()->CreatePathGeometry(&path))) {
        return;
    }

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return;
    }

    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); i++) {
        sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
        return;
    }

    if (!gesture_stroke_style_) {
        // 滑らかなジェスチャー軌跡のための丸型キャップと結合（初回のみ生成）
        const D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2d()->CreateStrokeStyle(ssp, nullptr, 0, &gesture_stroke_style_);
    }
    OpacityScope guard{ trail_brush, 0.5f };
    rt()->DrawGeometry(path.Get(), trail_brush, GESTURE_TRAIL_STROKE_WIDTH, gesture_stroke_style_.Get());
}

void Renderer::DrawGestureOverlay(NavDirection direction, const PaneRect& md_pane_rect)
{
    if (direction == NavDirection::None) {
        return;
    }

    const float rect_w = GESTURE_OVERLAY_WIDTH;
    const float rect_h = GESTURE_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float cy = md_pane_rect.y + md_pane_rect.height / 2.0f;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, cy - rect_h / 2, cx + rect_w / 2, cy + rect_h / 2);
    FillOverlayPanel(rect, GESTURE_OVERLAY_CORNER, theme_.IsDark() ? 0.8f : 0.6f);

    DrawCenteredText((direction == NavDirection::Back) ? L"\x2190 \x623B\x308B" : L"\x2192 \x9032\x3080", fmt_.gesture_overlay.Get(), rect, BrushId::OverlayWhite, 1.0f, D2D1::SizeF(rect_w, rect_h));
}

void Renderer::FillOverlayPanel(const D2D1_RECT_F& rect, float corner, float alpha)
{
    // dark テーマは灰色 (0.2,0.2,0.2)、light テーマは純黒 (0,0,0) で背景。
    // 専用色ブラシ + OpacityScope で毎フレーム SetColor を回避する。
    auto* const bg_brush{ Brush(theme_.IsDark() ? BrushId::OverlayGestureBg : BrushId::OverlayBlack) };
    if (!bg_brush) {
        return;
    }
    OpacityScope guard{ bg_brush, alpha };
    rt()->FillRoundedRectangle(D2D1::RoundedRect(rect, corner, corner), bg_brush);
}

void Renderer::DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect)
{
    const float rect_w = TOAST_OVERLAY_WIDTH;
    const float rect_h = TOAST_OVERLAY_HEIGHT;
    const float cx = md_pane_rect.x + md_pane_rect.width / 2.0f;
    const float bottom_y = md_pane_rect.y + md_pane_rect.height - NAV_BTN_MARGIN - NAV_BTN_SIZE - TOAST_OVERLAY_BOTTOM_OFFSET;
    const D2D1_RECT_F rect = D2D1::RectF(cx - rect_w / 2, bottom_y - rect_h, cx + rect_w / 2, bottom_y);

    FillOverlayPanel(rect, TOAST_OVERLAY_CORNER, theme_.IsDark() ? 0.85f : 0.7f);

    DrawCenteredText(toast.message, fmt_.toast_text.Get(), rect, BrushId::OverlayWhite, 1.0f, D2D1::SizeF(rect_w, rect_h));
}
