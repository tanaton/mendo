#pragma once
#include "doc_dwrite_bridge.h"
#include "draw_command.h"
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"
#include "ui_types.h"
#include "ui_constants.h"
#include "memory_resource.h"
#include "search_state.h"
#include <cassert>
#include <span>

// HitTestTextRange 初期バッファ容量。1 行中の inline code run が
// 折り返される想定最大数に合わせる。描画 hot path 中の resize を避けるのが目的。
inline constexpr size_t HIT_TEST_METRICS_INITIAL_CAPACITY = 64;

// HitTestTextRange をバッファ再利用付きで呼び出し、取得件数を返す。
inline UINT32 FetchHitTestMetrics(IDWriteTextLayout* layout, UINT32 start, UINT32 length, std::pmr::vector<DWRITE_HIT_TEST_METRICS>& buffer)
{
    if (buffer.size() < HIT_TEST_METRICS_INITIAL_CAPACITY) {
        buffer.resize(HIT_TEST_METRICS_INITIAL_CAPACITY);
    }
    UINT32 count = static_cast<UINT32>(buffer.size());
    HRESULT hr = layout->HitTestTextRange(start, length, 0, 0, buffer.data(), count, &count);
    if (hr == E_NOT_SUFFICIENT_BUFFER) {
        buffer.resize(count);
        layout->HitTestTextRange(start, length, 0, 0, buffer.data(), count, &count);
    }
    return count;
}

// インラインコードの背景矩形を描画する。
// bgsにはパディング適用済みのレイアウト原点相対矩形が格納されている。
inline void GenInlineCodeBgs(DrawCommandList& cmds, std::span<const InlineCodeBg> bgs, float origin_x, float origin_y, D2D1_COLOR_F color)
{
    for (const auto& bg : bgs) {
        const D2D1_RECT_F rect = D2D1::RectF(
            origin_x + bg.left,
            origin_y + bg.top,
            origin_x + bg.right,
            origin_y + bg.bottom);
        cmds.emplace_back(FillRoundedRectCmd{ rect, INLINE_CODE_CORNER, INLINE_CODE_CORNER, color });
    }
}

// テーブルセルのインラインコード背景を描画する。
// bgs は cell_index 昇順を維持しているため、cursor を進めるだけで O(N) 全体で済む。
// 戻り値は次回呼び出し向けに進めた cursor。
inline size_t GenCellInlineCodeBgs(DrawCommandList& cmds, std::span<const CellInlineCodeBg> bgs, size_t cursor, uint32_t cell_index, float origin_x, float origin_y, D2D1_COLOR_F color)
{
    while (cursor < bgs.size() && bgs[cursor].cell_index < cell_index) {
        ++cursor;
    }
    while (cursor < bgs.size() && bgs[cursor].cell_index == cell_index) {
        const auto& src = bgs[cursor].rect;
        const D2D1_RECT_F rect = D2D1::RectF(
            origin_x + src.left,
            origin_y + src.top,
            origin_x + src.right,
            origin_y + src.bottom);
        cmds.emplace_back(FillRoundedRectCmd{ rect, INLINE_CODE_CORNER, INLINE_CODE_CORNER, color });
        ++cursor;
    }
    return cursor;
}

// ドキュメントデータとビューポート状態から DrawCommandList を生成する。
// 「何を描画するか」と「どう描画するか」（エグゼキュータ）を分離する。
class CommandGenerator {
public:
    struct Formats {
        IDWriteTextFormat* list_number = nullptr;
        IDWriteTextFormat* icon_font = nullptr;
        IDWriteTextFormat* copy_btn_icon = nullptr;
        IDWriteTextFormat* placeholder_text = nullptr;
    };

    void SetHitTestBuffer(std::pmr::vector<DWRITE_HIT_TEST_METRICS>* buf) noexcept
    {
        hit_test_buffer_ = buf;
    }

    constexpr void SetTheme(const Theme* theme) noexcept
    {
        theme_ = theme;
        cached_is_dark_ = theme->IsDark();
        float a = cached_is_dark_ ? TABLE_STRIPE_ALPHA_DARK : TABLE_STRIPE_ALPHA_LIGHT;
        cached_stripe_color_ = cached_is_dark_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f, a) : D2D1::ColorF(0.0f, 0.0f, 0.0f, a);
    }
    constexpr void SetFormats(const Formats& fmts) noexcept
    {
        formats_ = fmts;
    }
    void SetSearchMatches(const std::pmr::vector<SearchMatch>* matches, int current_index, uint32_t generation) noexcept
    {
        search_matches_ = matches;
        current_match_index_ = current_index;
        search_generation_ = generation;
    }

    // Markdownコンテンツペインのすべての描画コマンドを生成する。
    // 内部バッファへの参照を返す。次回呼び出しまで有効。
    const DrawCommandList& GenerateMdPane(
        const std::pmr::vector<Node>& nodes, const LayoutCache& cache,
        const PaneRect& md_pane_rect, float scroll_y,
        const TextSelection& selection,
        int first_visible = -1,
        HoveredButtons hovered = {},
        float dpi_scale = 1.0f);

private:
    void GenerateNode(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, const DiagramEntry& diagram, int node_index, float entry_text_top);

    // ベースカラー、インラインコード背景、検索/選択ハイライト、本文テキスト、
    // タスクリストチェックボックスを描画する。
    void GenNodeTextDecorations(DrawCommandList& cmds, const Node& node,
                                const NodeLayoutEntry& entry, int node_index, float x, float text_x, float entry_text_top);

    struct NodeBaseStyle {
        D2D1_COLOR_F color;
        BrushId brush;
    };
    NodeBaseStyle GetNodeBaseStyle(const Node& node) const noexcept;

    void GenHorizontalRule(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w, float entry_text_top);
    void GenTable(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, int node_index, float x, float entry_text_top);
    void GenTableRowBg(DrawCommandList& cmds, bool is_header, bool is_even_row, float x, float y, float table_width, float row_h, float border);
    void GenTableCellContent(DrawCommandList& cmds, std::string_view cell_text, bool is_header, IDWriteTextLayout* cell_layout, float text_x, float text_y, bool has_selection, uint32_t sel_start, uint32_t sel_end, uint32_t flat_offset);
    void GenCodeBlockBg(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w, float entry_text_top);
    void GenOverlayButton(DrawCommandList& cmds, D2D1_RECT_F btn, wchar_t icon, bool is_hovered);
    void GenCopyButton(DrawCommandList& cmds, const NodeLayoutEntry& entry, float x, float w, bool is_hovered, float entry_text_top);
    void GenSaveButton(DrawCommandList& cmds, float bitmap_right, float bitmap_top, bool is_hovered);
    void GenSvgCopyButton(DrawCommandList& cmds, float bitmap_right, float bitmap_top, bool is_hovered);
    void GenListBullet(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, float x, float entry_text_top);
    void GenBlockQuoteGroupDecorations(DrawCommandList& cmds, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int node_count, int first_visible);
    void GenDiagramPlaceholder(DrawCommandList& cmds, float x, float y, float w, float h);
    void EmitHighlightRects(DrawCommandList& cmds, IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y, D2D1_COLOR_F color, BrushId brush_id = BrushId::Custom);
    // HitTestTextRange の結果をレイアウト原点相対の D2D1_RECT_F に変換し out へ append する。
    // SearchHlCache / SelectionHlCache の rebuild に共用する。
    void CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::pmr::vector<D2D1_RECT_F>& out);
    void GenSelectionHighlight(DrawCommandList& cmds, IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y);
    // 本文ノード（テーブル外）専用の選択ハイライト発行。
    // (layout, doc_start, doc_length) 一致でフレーム間キャッシュをヒットさせ
    // HitTestTextRange と UTF-8→UTF-16 decode の両方を省く。doc_start / doc_length は UTF-8 byte 単位。
    void GenSelectionHighlightCached(DrawCommandList& cmds, const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y);
    void GenSearchHighlights(DrawCommandList& cmds, const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y, int table_row = -1, int table_col = -1);

    // 検索ハイライトのキャッシュが古い場合に再構築する。
    // matches[first_global, first_global + node_match_count) が当該ノードのマッチ。
    void RebuildSearchHlCache(SearchHlCache& cache, const NodeLayoutEntry& entry,
                              std::span<const SearchMatch> matches, size_t first_global, size_t node_match_count);

    // 構築済みキャッシュから FillRectCmd を発行する。
    // table_row / table_col に該当するマッチのみ origin に加算して描画。
    void EmitSearchHlCommands(DrawCommandList& cmds, const SearchHlCache& cache,
                              std::span<const SearchMatch> matches, size_t first_global,
                              float origin_x, float origin_y, int table_row, int table_col);

    const Theme* theme_ = nullptr;
    Formats formats_;

    // フレーム単位の monotonic リソース。
    // GenerateMdPane の戻り値は同一スコープ内で即 CommandExecutor::Execute に
    // 渡されて消費される（renderer.cpp の呼び出しを参照）ため、
    // 前フレームの内容を生かしておく必要はなくシングルバッファで十分。
    // 大きめドキュメント (1 万コマンド規模) を初期バッファだけで賄えるサイズに設定し、
    // 上流 pool への再 allocate を平常時はゼロにする。
    MonotonicResource frame_resource_{ 1024 * 1024 };
    DrawCommandList cmds_{ frame_resource_.resource() };
    // 直前フレームのコマンド数。次フレーム冒頭で reserve に使い、
    // 倍々再確保で発生する死蔵バッファを抑える。
    size_t last_cmds_size_ = 0;

    const std::pmr::vector<SearchMatch>* search_matches_ = nullptr;
    int current_match_index_ = -1;
    uint32_t search_generation_ = 0;

    std::pmr::vector<DWRITE_HIT_TEST_METRICS>* hit_test_buffer_ = nullptr;

    DrawTextCmd MakeTextCmd(const wchar_t* src, size_t len, D2D1_RECT_F r, IDWriteTextFormat* fmt, D2D1_COLOR_F col, BrushId brush_id = BrushId::Custom)
    {
        assert(len <= 255 && "DrawTextCmd text exceeds uint8_t range");
        DrawTextCmd c{};
        c.text_len = static_cast<uint8_t>((std::min)(len, size_t(255)));
        c.rect = r;
        c.format = fmt;
        c.color = col;
        c.brush_id = brush_id;
        if (c.text_len == 0) {
            return c;
        }
        if (c.text_len <= DrawTextCmd::INLINE_TEXT_CAPACITY) {
            std::char_traits<wchar_t>::copy(c.inline_buf, src, c.text_len);
            c.is_inline = true;
        }
        else {
            auto* buf = static_cast<wchar_t*>(frame_resource_.resource()->allocate(c.text_len * sizeof(wchar_t), alignof(wchar_t)));
            std::char_traits<wchar_t>::copy(buf, src, c.text_len);
            c.text_ptr = buf;
            c.is_inline = false;
        }
        return c;
    }

    D2D1_COLOR_F cached_stripe_color_{};
    bool cached_is_dark_ = false;

    // GenerateMdPane のスコープ内で不変のフレーム単位コンテキスト。
    // GenerateNode 等の private 関数から参照される。
    float frame_offset_x_ = 0.0f;
    float frame_viewport_top_ = 0.0f;
    float frame_viewport_bottom_ = 0.0f;
    // フレーム座標系でのビューポート左右端。各セル x を frame_offset_x_ と
    // 合成したあとカリング判定に使う。GenerateMdPane で
    // -theme_->margin_left / md_pane_rect.width - theme_->margin_left を設定。
    float frame_viewport_left_ = 0.0f;
    float frame_viewport_right_ = 0.0f;
    float frame_content_width_ = 0.0f;
    float frame_dpi_scale_ = 1.0f;
    // bullet 等で SetTransform を一時 Identity に戻したあと復元するため、
    // フレーム冒頭で設定した Translation を保持する。
    float frame_md_pane_x_ = 0.0f;
    float frame_snapped_scroll_y_ = 0.0f;
    D2D1::Matrix3x2F frame_pane_transform_ = D2D1::Matrix3x2F::Identity();
    const TextSelection* frame_selection_ = nullptr;
    HoveredButtons frame_hovered_;

    // 前フレームの選択ノード範囲。範囲外に出たノードの selection_hl_cache を
    // 解放するために使う。-1/-1 は「前フレームは非アクティブ」を示す。
    int prev_sel_start_node_ = -1;
    int prev_sel_end_node_ = -1;

    // 選択ハイライトの UTF-8→UTF-16 decode を、本文ノードとテーブルセルそれぞれで
    // 連続フレーム間に渡って再利用する。GenerateMdPane 冒頭で ResetIfBufferChanged を
    // 呼び、ドキュメント切り替え時に string_view の dangling を防ぐ。
    mendo::WideViewCache node_wv_;
    mendo::WideViewCache cell_wv_;
};
