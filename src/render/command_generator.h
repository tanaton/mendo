#pragma once
#include "block_h_scroll.h"
#include "doc_dwrite_bridge.h"
#include "draw_command.h"
#include "document_types.h"
#include "layout_cache.h"
#include "theme.h"
#include "ui_types.h"
#include "ui_constants.h"
#include "search_state.h"
#include "text_layout_cache.h"
#include <memory_resource>
#include <span>
#include <string_view>

// DWRITE_HIT_TEST_METRICS を origin 加算付きの D2D1_RECT_F に変換する。
inline D2D1_RECT_F RectFromHitTest(const DWRITE_HIT_TEST_METRICS& m, float origin_x = 0.0f, float origin_y = 0.0f) noexcept
{
    return D2D1::RectF(
        origin_x + m.left,
        origin_y + m.top,
        origin_x + m.left + m.width,
        origin_y + m.top + m.height);
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

    // ファイル切替時に呼ぶ。初期容量は次ファイルの描画 hot path で再拡大されないよう事前確保する。
    void ShrinkBuffers()
    {
        hit_test_buffer_.shrink_to_fit();
        hit_test_buffer_.reserve(HIT_TEST_METRICS_INITIAL_CAPACITY);
    }
    // 上の書式で描く文字列のレイアウトを作り置く先。null なら文字列を描かない。
    // 前フレームのレイアウトの破棄 (BeginFrame) は持ち主が行う。
    void SetTextLayoutCache(TextLayoutCache* cache) noexcept
    {
        text_layouts_ = cache;
    }

    void SetTheme(const Theme* theme) noexcept
    {
        theme_ = theme;
        cached_is_dark_ = theme->IsDark();
    }
    void SetFormats(const Formats& fmts) noexcept
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
        OverlayButtonHit hovered = {},
        float dpi_scale = 1.0f,
        const BlockHScrollContext& block_h_scroll = {});

private:
    // GenerateMdPane の 1 フレーム呼び出しスコープでだけ意味を持つ入力をまとめる。
    // ノードの [text_top, text_top + height] の外へはみ出して描く量の全種別での上限
    // (見出し下線・空 LI の bullet・コード背景・引用装飾等)。ノード単位のカリングはこの余裕幅で
    // 判定し、実際の切り抜きは md ペインのクリップに任せる。
    struct NodeOverhang {
        float above;
        float below;
    };
    NodeOverhang MaxNodeOverhang() const noexcept;

    struct FrameContext {
        float offset_x = 0.0f;
        // フレーム座標系でのビューポート左右端。各セル x を offset_x と
        // 合成したあとカリング判定に使う。
        float viewport_left = 0.0f;
        float viewport_right = 0.0f;
        float content_width = 0.0f;
        float dpi_scale = 1.0f;
        float snapped_scroll_y = 0.0f;
        // ブロック横スクロールの clip 内で平行移動を合成し、抜けたあと復元するための基底 Translation。
        D2D1::Matrix3x2F pane_transform;
        const TextSelection& selection;
        OverlayButtonHit hovered;
        BlockHScrollContext h_scroll;
        NodeOverhang overhang;
    };

    // 選択範囲外に出たノードの SelectionHlCache を前フレームとの差分区間だけ解放する。
    void ReleaseStaleSelectionHlCaches(const LayoutCache& cache, const TextSelection& selection);

    void GenerateNode(const FrameContext& fc, const Node& node, const NodeLayoutEntry& entry, const DiagramEntry* diagram, int node_index, float entry_text_top);

    // ベースカラー、インラインコード背景、検索/選択ハイライト、本文テキストを描画する。
    void GenNodeTextDecorations(const FrameContext& fc, const Node& node,
        const NodeLayoutEntry& entry, int node_index, float text_x, float entry_text_top);
    // text_layout 非依存。loose task list (空 TaskListItem) でも GenNodeTextDecorations の
    // early return を経由せず描画したいため独立メソッド化。
    void GenTaskListCheckbox(const Node& node, float x, float entry_text_top);

    static BrushId GetNodeBaseBrush(const Node& node) noexcept;

    void GenHorizontalRule(float x, float w, float entry_text_top);
    void GenTable(const FrameContext& fc, const Node& node, const NodeLayoutEntry& entry, int node_index, float offset_x, float entry_text_top, float h_scroll_x = 0.0f);
    // テーブル 1 行分の幾何。GenTableRowBg と内部ループで使い回す。
    struct TableRowGeom {
        float x;
        float y;
        float table_width;
        float row_h;
        float border;
    };
    void GenTableRowBg(const TableRowGeom& geom, bool is_header, bool is_even_row);
    // テーブルセル 1 個分の描画コンテキスト。GenTable から GenTableCellContent へ橋渡しする。
    // selection 関連は (has_selection, sel_start, sel_end, flat_offset) を 1 まとまりで扱う。
    struct CellDrawContext {
        IDWriteTextLayout* layout = nullptr;
        float text_x = 0.0f;
        float text_y = 0.0f;
        float height = 0.0f;
        uint32_t flat_offset = 0;
        uint32_t sel_start = 0;
        uint32_t sel_end = 0;
        bool has_selection = false;
        bool is_header = false;
    };
    void GenTableCellContent(std::string_view cell_text, const CellDrawContext& ctx);
    // box は padding 込みのコードブロック矩形 (ペインローカル)。
    void GenCodeBlockBg(const D2D1_RECT_F& box);
    void GenOverlayButtons(const FrameContext& fc, const Node& node, const DiagramEntry* diagram, float x, float cw, float entry_text_top, int node_index);
    void GenOverlayButton(D2D1_RECT_F btn, wchar_t icon, bool is_hovered);
    // ブロックローカルの水平スクロールバー。ホバー中 / ドラッグ中の対象ブロックでのみ emit する。
    // block_x はブロック左端、bar_y はバー上端 (ペイン内ローカル座標)。
    // geom.visible_width / natural_width は BlockHScrollGeometry と同じ意味。
    void EmitBlockHScrollbarIfActive(const FrameContext& fc, int node_index, float block_x, float bar_y, const BlockHScrollGeometry& geom, float scroll_x);
    void GenListBullet(const FrameContext& fc, const Node& node, const NodeLayoutEntry& entry, float x, float entry_text_top);
    void GenBlockQuoteGroupDecorations(const FrameContext& fc, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int node_count, int first_visible);
    // nodes[first, last) が 1 つの引用グループ。group_top / group_bottom はペインローカル Y。
    void GenBlockQuoteGroup(const FrameContext& fc, const std::pmr::vector<Node>& nodes, const LayoutCache& cache, int first, int last, int max_depth, float group_top, float group_bottom);
    // error 非空時は「読み込み中...」の代わりにエラーメッセージを表示する (issue #271)。
    void GenDiagramPlaceholder(float x, float y, float w, float h, std::wstring_view error = {});
    // 共有バッファへ HitTestTextRange を発行し、取得したメトリクスを返す。次回呼び出しまで有効。
    std::span<const DWRITE_HIT_TEST_METRICS> HitTestRange(IDWriteTextLayout* layout, uint32_t start, uint32_t length);
    // 可視 Y 範囲 [cull_top_, cull_bottom_] と重なる矩形だけ FillRectCmd を積む。
    void EmitFillRectIfVisible(const D2D1_RECT_F& r, BrushId brush);
    // HitTestTextRange の結果をレイアウト原点相対の D2D1_RECT_F に変換し out へ append する。
    // SearchHlCache / SelectionHlCache の rebuild に共用する。
    void CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::pmr::vector<D2D1_RECT_F>& out);
    void GenSelectionHighlight(IDWriteTextLayout* layout, uint32_t start, uint32_t length, float origin_x, float origin_y);
    // 本文ノード（テーブル外）専用の選択ハイライト発行。
    // (layout, doc_start, doc_length) 一致でフレーム間キャッシュをヒットさせ
    // HitTestTextRange と UTF-8→UTF-16 decode の両方を省く。doc_start / doc_length は UTF-8 byte 単位。
    void GenSelectionHighlightCached(const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y);
    // 本文ノードのマッチのうち可視範囲の前後の行にかかるものを描く。
    void GenSearchHighlights(const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y);

    // ノードのマッチと、現在の検索世代に揃えたキャッシュ。マッチが無ければ matches は空。
    struct NodeSearchMatches {
        std::span<const SearchMatch> matches;
        size_t first_global = 0;
        SearchHlCache* cache = nullptr;
    };
    NodeSearchMatches PrepareNodeSearchMatches(const NodeLayoutEntry& entry, int node_index);
    void GenCellSearchHighlights(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t row, size_t col, float origin_x, float origin_y);
    // matches[begin, end) のうち未計算のものを layout で計算し、FillRectCmd を積む。layout が null なら計算を見送る。
    void EmitSearchMatches(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t begin, size_t end, float origin_x, float origin_y);

    // origin_y に置いた本文 layout の、可視範囲の上下に 1 画面ずつ足した行のテキスト範囲。
    const LineWindow& LineWindowAround(const NodeLayoutEntry& entry, float origin_y) const;

    // 書式の揃えに従って origin から box の範囲に text を描く。
    void EmitText(std::wstring_view text, IDWriteTextFormat* fmt, D2D1_POINT_2F origin, D2D1_SIZE_F box, BrushId brush_id, float opacity = 1.0f);

    const Theme* theme_ = nullptr;
    Formats formats_;
    TextLayoutCache* text_layouts_ = nullptr;

    // 戻り値は同一スコープで ExecuteDrawCommands に渡されて消費されるので、毎フレーム clear して容量を使い回す。
    DrawCommandList cmds_;

    const std::pmr::vector<SearchMatch>* search_matches_ = nullptr;
    int current_match_index_ = -1;
    uint32_t search_generation_ = 0;

    // 可視 Y 範囲 (ペインローカル = 0 〜 pane_height)。entry_text_top も同じローカル Y 系で、
    // ドキュメント Y が 10^7 オーダーで float32 が桁落ちする問題 (#216) を避ける。ノード・行・
    // ハイライト矩形・グリフランのカリングに共通で使う。
    static constexpr float cull_top_ = 0.0f;
    float cull_bottom_ = 0.0f;

    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    // SetTheme 時に theme_ から導出する。
    bool cached_is_dark_ = false;

    // 前フレームの選択ノード範囲。範囲外に出たノードの selection_hl_cache を
    // 解放するために使う。-1/-1 は「前フレームは非アクティブ」を示す。
    int prev_sel_start_node_ = -1;
    int prev_sel_end_node_ = -1;
};
