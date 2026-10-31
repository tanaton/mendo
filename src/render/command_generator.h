#pragma once
#include "block_h_scroll.h"
#include "doc_dwrite_bridge.h"
#include "draw_command.h"
#include "document_types.h"
#include "layout_cache.h"
#include "render_params.h"
#include "theme.h"
#include "ui_types.h"
#include "ui_constants.h"
#include "search_state.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// 繰り返し描く短い文字列 (アイコン・リスト番号・プレースホルダ・サイドペインの項目等) のテキストレイアウトを
// (書式, 文字列, 箱の大きさ) ごとに作り置く。DrawText は呼ぶたびに内部でレイアウトを作るため、
// これで得たレイアウトを DrawTextLayout で描く。キーで書式を参照して持つので、書式を作り直しても
// 解放したアドレスが新しい書式に使い回されて別の大きさのレイアウトが返ることはなく、古いものは上限で捨てる。
class TextLayoutCache {
public:
    void SetFactory(IDWriteFactory* factory) noexcept
    {
        factory_ = factory;
        entries_.clear();
    }

    // 作れなければ nullptr。返したレイアウトは次の BeginFrame / SetFactory まで有効。
    IDWriteTextLayout* Get(IDWriteTextFormat* fmt, std::wstring_view text, D2D1_SIZE_F box)
    {
        if (!factory_ || !fmt || text.empty()) {
            return nullptr;
        }
        if (const auto it = entries_.find(KeyView{ fmt, text, box.width, box.height }); it != entries_.end()) {
            it->second.last_used = frame_;
            return it->second.layout.Get();
        }
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, box.width, box.height, &layout))) {
            return nullptr;
        }
        auto* const raw = layout.Get();
        entries_.emplace(Key{ Microsoft::WRL::ComPtr<IDWriteTextFormat>(fmt), std::wstring(text), box.width, box.height }, Entry{ std::move(layout), frame_ });
        return raw;
    }

    // box はスクロール位置に依らない値を渡す。矩形の差から求めると丸めでキャッシュが外れる。
    void Emit(DrawCommandList& out, std::wstring_view text, IDWriteTextFormat* fmt, D2D1_POINT_2F origin, D2D1_SIZE_F box,
              BrushId brush_id, float opacity)
    {
        if (auto* layout = Get(fmt, text, box)) {
            out.push_back(DrawTextLayoutCmd{ .origin = origin, .layout = layout, .brush_id = brush_id, .opacity = opacity });
        }
    }
    // 中央揃え書式で大きさ 0 のレイアウトを作り、rect の中心に置く。
    void EmitCentered(DrawCommandList& out, std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float opacity)
    {
        Emit(out, text, fmt, D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f), {}, brush_id, opacity);
    }

    // 箱の大きさや番号ごとに増え続けないよう、描画の合間 (前フレームのレイアウトを参照し終えた後) に上限で捨てる。
    // 全消去すると目次やファイル一覧が上限を超えるたびに表示中の項目まで作り直すので、直前のフレームで使ったものは残す。
    void BeginFrame() noexcept
    {
        if (entries_.size() > kMaxEntries) {
            std::erase_if(entries_, [prev = frame_](const auto& kv) noexcept { return kv.second.last_used != prev; });
        }
        ++frame_;
    }

    // layout を作った文字列。見つからなければ空。
    std::wstring_view TextOf(const IDWriteTextLayout* layout) const noexcept
    {
        const auto it = std::ranges::find(entries_, layout, [](const auto& kv) static noexcept { return kv.second.layout.Get(); });
        if (it == entries_.end()) {
            return {};
        }
        return it->first.text;
    }

    static constexpr size_t kMaxEntries = 512;

private:
    struct KeyView {
        IDWriteTextFormat* fmt;
        std::wstring_view text;
        float width;
        float height;
        bool operator==(const KeyView&) const = default;
    };
    struct Key {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        std::wstring text;
        float width;
        float height;
        operator KeyView() const noexcept
        {
            return { fmt.Get(), text, width, height };
        }
    };
    struct KeyHash {
        using is_transparent = void;
        static size_t operator()(const KeyView& k) noexcept
        {
            size_t h = std::hash<std::wstring_view>{}(k.text);
            const auto mix = [&h](size_t v) noexcept { h ^= v + kGoldenRatio64 + (h << 6) + (h >> 2); };
            mix(std::hash<const void*>{}(k.fmt));
            mix(std::bit_cast<uint32_t>(k.width));
            mix(std::bit_cast<uint32_t>(k.height));
            return h;
        }
    };
    struct KeyEqual {
        using is_transparent = void;
        static bool operator()(const KeyView& a, const KeyView& b) noexcept
        {
            return a == b;
        }
    };

    struct Entry {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        uint32_t last_used;
    };

    IDWriteFactory* factory_ = nullptr;
    uint32_t frame_ = 0;
    std::unordered_map<Key, Entry, KeyHash, KeyEqual> entries_;
};

// DWRITE_HIT_TEST_METRICS を origin 加算付きの D2D1_RECT_F に変換する。
inline D2D1_RECT_F RectFromHitTest(const DWRITE_HIT_TEST_METRICS& m, float origin_x = 0.0f, float origin_y = 0.0f) noexcept
{
    return D2D1::RectF(
        origin_x + m.left,
        origin_y + m.top,
        origin_x + m.left + m.width,
        origin_y + m.top + m.height);
}

// 本文の文字列 (リスト番号・チェックボックス・アイコン・プレースホルダ) の書式。持ち主は Renderer。
struct MdPaneFormats {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> checkbox;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> copy_btn_icon;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> list_number;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> placeholder_text;
};

// ペインの縦スクロールバーのつまみ。出さないときは nullopt。
// ドラッグ処理 (ScrollFromThumbY) と同じ計算を共有し、見た目と挙動のズレを防ぐ。
std::optional<FillRoundedRectCmd> VScrollThumb(const PaneRect& pane, float header_height, float scroll_y, float total_content_height,
                                               bool may_grow = false) noexcept;

// 番号付きリストの番号の文字列 ("3." / "iii." / "c.")。段ごとに 10 進・小文字ローマ数字・小文字アルファベットの順で、
// 4 段目からは繰り返す (箇条書きの記号と同じ)。
// ローマ数字は 1〜3999、アルファベットは 1 以上しか表せないので、それ以外の番号は CSS と同じく 10 進にする。
inline constexpr size_t kListNumberMaxLen = 16; // 最長は 3888 のローマ数字 "mmmdccclxxxviii."
std::wstring_view FormatListNumber(std::span<wchar_t, kListNumberMaxLen> buf, int32_t number, int list_depth) noexcept;

// 検索欄の文字列を測った結果。x は文字列の左端から測る。
struct SearchInputMetrics {
    IDWriteTextLayout* layout = nullptr; // null なら文字列も選択範囲も描かない
    float caret_x = 0.0f;
    float selection_x = 0.0f;
    float selection_width = 0.0f; // 0 なら選択なし
};

// 検索欄の文字列のレイアウト。表示テキスト (IME の合成中テキストを挿入したもの) と幅が前回と同じ間は使い回す。
// ドラッグ選択中は WM_MOUSEMOVE ごとに HitTest が来るので、描画と同じレイアウトを引く。
class SearchInputLayout {
public:
    // 書式を作り直したら呼ぶ。前の書式のレイアウトは捨てる。
    void SetFormat(IDWriteFactory* factory, IDWriteTextFormat* fmt) noexcept
    {
        factory_ = factory;
        fmt_ = fmt;
        Forget();
    }
    // レイアウトを用意して選択範囲とキャレットの位置を測る。返す layout は次の Measure / SetFormat まで有効。
    SearchInputMetrics Measure(const SearchBarRenderState& sb, float width);
    // 文字列の左端から local_x の位置に置くキャレットの文字位置。表示用のレイアウトが同じ文字列・幅なら使い回す。
    int HitTest(std::wstring_view query, float local_x, float width) const;

private:
    bool Holds(std::wstring_view text, DWRITE_TEXT_RANGE underline, float width) const noexcept
    {
        return layout_ && width_ == width && underline_.startPosition == underline.startPosition && underline_.length == underline.length && text_ == text;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> Create(std::wstring_view text, float width) const;
    void Forget() noexcept
    {
        layout_.Reset();
        text_.clear();
        width_ = -1.0f;
        underline_ = {};
    }

    IDWriteFactory* factory_ = nullptr;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout_;
    std::wstring text_;
    std::wstring scratch_;
    float width_ = -1.0f;
    DWRITE_TEXT_RANGE underline_{};
};

// 本文ペインに重ねるもの (変換は恒等、クリップ無しで描く)。文字列は text_layouts に作り置き、fmt が null なら描かない。
namespace overlay {
// 検索バー。sbl は sb の検索語の有無で組んだもの (ComputeSearchBarLayout)。input は sbl の入力欄の幅で測ったもの。
void EmitSearchBar(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* count_fmt,
                   const SearchBarLayout& sbl, const SearchBarRenderState& sb, const SearchInputMetrics& input);
// 読み込み中のスピナー。ペインの中央に点を回して並べ、後ろの点ほど薄くする。
void EmitLoadingSpinner(DrawCommandList& out, float angle, const PaneRect& md_pane_rect);
// 戻る/進むのボタン。押せないボタンも薄く描く。
void EmitNavButtons(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect,
                    bool dark, bool can_back, bool can_forward, NavDirection hovered);
// ジェスチャー・スワイプで戻る/進む方向。None なら何も積まない。
void EmitGestureOverlay(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect,
                        bool dark, NavDirection direction);
// 空なら何も積まない。
void EmitToast(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect,
               bool dark, std::wstring_view message);
} // namespace overlay

// タイトルバーとサイドペイン。デバイスに依存しないので、デバイスの作成前にも組み立てられる。
namespace chrome {
// app_icon が null ならアイコンを描かない。
void EmitTitleBar(DrawCommandList& out, TextLayoutCache& text_layouts, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* title_fmt,
                  ID2D1Bitmap* app_icon, bool dark, const TitleBarRenderState& tb);
// 一覧の書式はペインごとの倍率で作るので、PaneTarget で引く。
struct SidePaneFormats {
    IDWriteTextFormat* header = nullptr;
    IDWriteTextFormat* header_icon = nullptr;
    IDWriteTextFormat* item[2]{};
    // ファイル一覧のアイコン。
    IDWriteTextFormat* file_icon = nullptr;
};
// 表示中のサイドペインとスプリッタ。toc_titles は目次項目の UTF-16 で、描いた項目だけ作る。
void EmitSidePanes(DrawCommandList& out, TextLayoutCache& text_layouts, const SidePaneFormats& fmts, const Theme& theme,
                   const SidePaneState& sp, std::vector<std::wstring>& toc_titles);
} // namespace chrome

// ドキュメントデータとビューポート状態から DrawCommandList を生成する。
// 「何を描画するか」と「どう描画するか」（エグゼキュータ）を分離する。
class CommandGenerator {
public:
    // theme・formats・text_layouts は持ち主 (Renderer) のもので、作り直しても同じ場所を指したままでよい。
    // formats と text_layouts は文字列 (リスト番号・チェックボックス・アイコン・プレースホルダ) に使い、null なら文字列を描かない。
    // 前フレームのレイアウトの破棄 (TextLayoutCache::BeginFrame) は持ち主が行う。
    void Init(const Theme& theme, const MdPaneFormats* formats = nullptr, TextLayoutCache* text_layouts = nullptr) noexcept
    {
        theme_ = &theme;
        formats_ = formats ? formats : &kNoFormats;
        text_layouts_ = text_layouts;
    }

    // Markdownコンテンツペインのすべての描画コマンドを out の後ろに積む。
    void GenerateMdPane(DrawCommandList& out, const MdPaneParams& params, float dpi_scale);

private:
    // ノードの [text_top, text_top + height] の外へはみ出して描く量の全種別での上限
    // (見出し下線・空 LI の bullet・コード背景・引用装飾等)。ノード単位のカリングはこの余裕幅で
    // 判定し、実際の切り抜きは md ペインのクリップに任せる。
    struct NodeOverhang {
        float above;
        float below;
    };
    NodeOverhang MaxNodeOverhang() const noexcept;

    // GenerateMdPane の呼び出しの間だけ意味を持つ。
    struct FrameContext {
        const MdPaneParams* params = nullptr;
        float content_width = 0.0f;
        float dpi_scale = 1.0f;
        float snapped_scroll_y = 0.0f;
        // 可視 Y 範囲 (ペインローカル = 0 〜 pane_height)。entry_text_top も同じローカル Y 系で、
        // ドキュメント Y が 10^7 オーダーで float32 が桁落ちする問題 (#216) を避ける。ノード・行・
        // ハイライト矩形・グリフランのカリングに共通で使う。
        YRange visible = YRange::None();
        // ブロック横スクロールの clip 内で平行移動を合成し、抜けたあと復元するための基底 Translation。
        D2D1::Matrix3x2F pane_transform;
        NodeOverhang overhang{};
    };

    // 選択範囲外に出たノードの SelectionHlCache を前フレームとの差分区間だけ解放する。
    void ReleaseStaleSelectionHlCaches(const LayoutCache& cache, const TextSelection& selection);

    void GenerateNode(const Node& node, const NodeLayoutEntry& entry, const DiagramEntry* diagram, int node_index, float entry_text_top);

    // ベースカラー、インラインコード背景、検索/選択ハイライト、本文テキストを描画する。
    void GenNodeTextDecorations(const Node& node, const NodeLayoutEntry& entry, int node_index, float text_x, float entry_text_top);
    // text_layout 非依存。loose task list (空 TaskListItem) でも GenNodeTextDecorations の
    // early return を経由せず描画したいため独立メソッド化。
    void GenTaskListCheckbox(const Node& node, float x, float entry_text_top);

    void GenHorizontalRule(float x, float w, float entry_text_top);
    // 表は横スクロールの clip [offset_x, offset_x + clip_width] の中に描くので、列はその幅で間引く。
    void GenTable(const Node& node, const NodeLayoutEntry& entry, int node_index, float offset_x, float clip_width, float entry_text_top,
                  float h_scroll_x);
    // テーブルセル 1 個分の描画コンテキスト。GenTable から GenTableCellContent へ橋渡しする。
    // selection はテーブル全体の選択範囲で、セルの開始位置 flat_offset と同じく concat_text 上の UTF-8 byte。
    struct CellDrawContext {
        IDWriteTextLayout* layout = nullptr;
        float text_x = 0.0f;
        float text_y = 0.0f;
        float height = 0.0f;
        uint32_t flat_offset = 0;
        std::span<const TextRun> runs;
        TextSelection::NodeRange selection{ 0, 0 };
        bool is_header = false;
    };
    void GenTableCellContent(std::string_view cell_text, const CellDrawContext& ctx);
    // box は padding 込みのコードブロック矩形 (ペインローカル)。
    // 区切ったコードブロックの継ぎ目の側は角を丸めず、隣の背景とつなげる。
    void GenCodeBlockBg(const D2D1_RECT_F& box, bool round_top, bool round_bottom);
    void GenOverlayButtons(const Node& node, const DiagramEntry* diagram, float x, float cw, float entry_text_top, int node_index);
    void GenOverlayButton(D2D1_RECT_F btn, wchar_t icon, bool is_hovered);
    // ブロックローカルの水平スクロールバー。ホバー中 / ドラッグ中の対象ブロックでのみ emit する。
    // block_x はブロック左端、bar_y はバー上端 (ペイン内ローカル座標)。
    void EmitBlockHScrollbarIfActive(int node_index, float block_x, float bar_y, const BlockHScrollGeometry& geom, float scroll_x);
    void GenListBullet(const Node& node, const NodeLayoutEntry& entry, float x, float entry_text_top);
    void GenBlockQuoteGroupDecorations(int first_visible);
    // nodes[first, last) が 1 つの引用グループ。group_top / group_bottom はペインローカル Y。
    void GenBlockQuoteGroup(int first, int last, int max_depth, float group_top, float group_bottom);
    // error 非空時は「読み込み中...」の代わりにエラーメッセージを表示する (issue #271)。
    void GenDiagramPlaceholder(float x, float y, float w, float h, std::wstring_view error = {});
    // 共有バッファへ HitTestTextRange を発行し、取得したメトリクスを返す。次回呼び出しまで有効。
    std::span<const DWRITE_HIT_TEST_METRICS> HitTestRange(IDWriteTextLayout* layout, uint32_t start, uint32_t length);
    // 可視 Y 範囲と重なる矩形だけ FillRectCmd を積む。
    void EmitFillRectIfVisible(const D2D1_RECT_F& r, BrushId brush);
    // HitTestTextRange の結果をレイアウト原点相対の D2D1_RECT_F に変換し out へ append する。
    // SearchHlCache / SelectionHlCache の rebuild に共用する。
    void CollectHitTestRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length, std::vector<D2D1_RECT_F>& out);
    // 本文ノード（テーブル外）専用の選択ハイライト発行。
    // (layout, doc_start, doc_length) 一致でフレーム間キャッシュをヒットさせ
    // HitTestTextRange と UTF-8→UTF-16 decode の両方を省く。doc_start / doc_length は UTF-8 byte 単位。
    void GenSelectionHighlightCached(const Node& node, const NodeLayoutEntry& entry, uint32_t doc_start, uint32_t doc_length, float origin_x, float origin_y);
    // 本文ノードのマッチのうち可視範囲の前後の行にかかるものを描く。
    void GenSearchHighlights(const NodeLayoutEntry& entry, int node_index, float origin_x, float origin_y);

    // ノードのマッチと、現在の検索世代に揃えたキャッシュ。マッチが無ければ matches は空。
    // spill_length_w は前のノードのマッチ (first_global - 1 番) がこのノードの先頭へはみ出した長さで、
    // キャッシュの最後の span を使う。
    struct NodeSearchMatches {
        std::span<const SearchMatch> matches;
        size_t first_global = 0;
        SearchHlCache* cache = nullptr;
        uint32_t spill_length_w = 0;
    };
    NodeSearchMatches PrepareNodeSearchMatches(const NodeLayoutEntry& entry, int node_index);
    void GenCellSearchHighlights(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t row, size_t col, float origin_x, float origin_y);
    // matches[begin, end) のうち未計算のものを layout で計算し、FillRectCmd を積む。layout が null なら計算を見送る。
    void EmitSearchMatches(const NodeSearchMatches& nm, IDWriteTextLayout* layout, size_t begin, size_t end, float origin_x, float origin_y);
    void EmitSearchSpan(SearchHlCache& cache, size_t span_index, IDWriteTextLayout* layout, uint32_t start_w, uint32_t length_w, size_t global_index,
                        float origin_x, float origin_y);

    // origin_y に置いた本文 layout の、可視範囲の上下に 1 画面ずつ足した行のテキスト範囲。
    const LineWindow& LineWindowAround(const NodeLayoutEntry& entry, float origin_y) const;

    // 書式の揃えに従って origin から box の範囲に text を描く。
    void EmitText(std::wstring_view text, IDWriteTextFormat* fmt, D2D1_POINT_2F origin, D2D1_SIZE_F box, BrushId brush_id, float opacity = 1.0f);

    inline static const MdPaneFormats kNoFormats{};
    const Theme* theme_ = nullptr;
    const MdPaneFormats* formats_ = &kNoFormats;
    TextLayoutCache* text_layouts_ = nullptr;
    FrameContext frame_;

    // 積む先。GenerateMdPane の呼び出しの間だけ意味を持つ。
    DrawCommandList* cmds_ = nullptr;

    std::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    // 前フレームの選択ノード範囲。範囲外に出たノードの selection_hl_cache を
    // 解放するために使う。-1/-1 は「前フレームは非アクティブ」を示す。
    int prev_sel_start_node_ = -1;
    int prev_sel_end_node_ = -1;
};
