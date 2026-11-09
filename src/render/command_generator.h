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
// (書式, フォントの対応, 文字列, 箱の大きさ, 下線) ごとに作り置く。DrawText は呼ぶたびに内部でレイアウトを作るため、
// これで得たレイアウトを DrawTextLayout で描く。キーで書式を参照して持つので、書式を作り直しても
// 解放したアドレスが新しい書式に使い回されて別の大きさのレイアウトが返ることはなく、古いものは上限で捨てる。
class TextLayoutCache {
public:
    void SetFactory(IDWriteFactory* factory) noexcept
    {
        factory_ = factory;
        entries_.clear();
    }

    // 作れなければ nullptr。返したレイアウトは次の BeginFrame / SetFactory まで有効。underline は下線を引く範囲 (IME の合成中の文字列)。
    // fonts は fmt の主フォントとロケールに合うものだけを渡す。
    IDWriteTextLayout* Get(IDWriteTextFormat* fmt, std::wstring_view text, D2D1_SIZE_F box, DWRITE_TEXT_RANGE underline = {}, const FallbackFontMap* fonts = nullptr)
    {
        const KeyTail tail{ fonts, box.width, box.height, underline.startPosition, underline.length };
        if (const auto it = entries_.find(KeyView{ fmt, text, tail }); it != entries_.end()) {
            it->second.last_used = frame_;
            return it->second.layout.Get();
        }
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (factory_ && fmt && !text.empty()) {
            factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, box.width, box.height, &layout);
        }
        if (!layout) {
            return nullptr;
        }
        if (fonts) {
            ApplyFallbackFonts(layout.Get(), text, *fonts);
        }
        if (underline.length > 0) {
            layout->SetUnderline(TRUE, underline);
        }
        auto* const raw = layout.Get();
        entries_.emplace(Key{ Microsoft::WRL::ComPtr<IDWriteTextFormat>(fmt), std::wstring(text), tail }, Entry{ std::move(layout), frame_ });
        return raw;
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
    // 書式と文字列以外のキー。
    struct KeyTail {
        const FallbackFontMap* fonts;
        float width;
        float height;
        uint32_t underline_start;
        uint32_t underline_length;
        bool operator==(const KeyTail&) const = default;
    };
    struct KeyView {
        IDWriteTextFormat* fmt;
        std::wstring_view text;
        KeyTail tail;
        bool operator==(const KeyView&) const = default;
    };
    struct Key {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        std::wstring text;
        KeyTail tail;
        operator KeyView() const noexcept
        {
            return { fmt.Get(), text, tail };
        }
    };
    struct KeyHash {
        using is_transparent = void;
        // KeyTail は詰め物の無い値の並びなのでバイト列のまま渡し、文字列と書式は seed に混ぜる。
        static_assert(sizeof(KeyTail) == sizeof(void*) + 4 * sizeof(uint32_t));
        static size_t operator()(const KeyView& k) noexcept
        {
            return rapidhash_withSeed(&k.tail, sizeof(k.tail), RapidHash(k.text) ^ std::bit_cast<uintptr_t>(k.fmt));
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

inline D2D1_RECT_F RectFromHitTest(const DWRITE_HIT_TEST_METRICS& m, float origin_x, float origin_y) noexcept
{
    return D2D1::RectF(
        origin_x + m.left,
        origin_y + m.top,
        origin_x + m.left + m.width,
        origin_y + m.top + m.height);
}

// ボタンの背景。brush が None なら塗らない。角の半径が 0 なら矩形で塗る (不透明度は掛けない)。
struct ButtonBg {
    BrushId brush = BrushId::None;
    float corner = 0.0f;
    float opacity = 1.0f;
};

// 描画命令の積み先。文字列は text_layouts に作り置いたレイアウトで描き、fmt が null なら描かない。
struct CmdWriter {
    DrawCommandList& out;
    TextLayoutCache& text_layouts;

    // 書式の揃えに従って origin から box の範囲に text を描く。
    // box はスクロール位置に依らない値を渡す。矩形の差から求めると丸めでキャッシュが外れる。
    void Text(std::wstring_view text, IDWriteTextFormat* fmt, D2D1_POINT_2F origin, D2D1_SIZE_F box, BrushId brush_id, float opacity = 1.0f,
              const FallbackFontMap* fonts = nullptr) const
    {
        if (auto* layout = text_layouts.Get(fmt, text, box, {}, fonts)) {
            out.push_back(DrawTextLayoutCmd{ .origin = origin, .layout = layout, .brush_id = brush_id, .opacity = opacity });
        }
    }
    // 中央揃え書式で大きさ 0 のレイアウトを作り、rect の中心に置く。
    void Centered(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float opacity) const
    {
        const DipPoint c = RectCenter(rect);
        Text(text, fmt, D2D1::Point2F(c.x, c.y), {}, brush_id, opacity);
    }
    // アイコンや短い文字列を rect の中央に置くボタン。書式は左右・上下とも中央揃え。
    void Button(IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, std::wstring_view label, ButtonBg bg, BrushId fg, float fg_opacity) const;
};

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
std::optional<FillRoundedRectCmd> VScrollThumb(const PaneRect& pane, const PaneScrollInfo& info, float scroll_y, bool may_grow = false) noexcept;

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

// 検索欄の文字列のレイアウトを text_layouts から引き、選択範囲とキャレットの位置を測る。
SearchInputMetrics MeasureSearchInput(TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, const SearchBarRenderState& sb, float width);
// 文字列の左端から local_x の位置に置くキャレットの文字位置。ドラッグ選択中は WM_MOUSEMOVE ごとに呼ばれるので、描画と同じレイアウトを引く。
int HitTestSearchInput(TextLayoutCache& text_layouts, IDWriteTextFormat* fmt, std::wstring_view query, float local_x, float width);

// 本文ペインに重ねるもの (変換は恒等、クリップ無しで描く)。
namespace overlay {
// 検索バー。sbl は sb の検索語の有無で組んだもの (ComputeSearchBarLayout)。input は sbl の入力欄の幅で測ったもの。
void EmitSearchBar(CmdWriter w, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* count_fmt, const SearchBarLayout& sbl, const SearchBarRenderState& sb,
                   const SearchInputMetrics& input);
// 読み込み中のスピナー。ペインの中央に点を回して並べ、後ろの点ほど薄くする。
void EmitLoadingSpinner(DrawCommandList& out, float angle, const PaneRect& md_pane_rect);
// 戻る/進むのボタン。押せないボタンも薄く描く。
void EmitNavButtons(CmdWriter w, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect, bool dark, bool can_back, bool can_forward, NavDirection hovered);
// ジェスチャー・スワイプで戻る/進む方向。None なら何も積まない。
void EmitGestureOverlay(CmdWriter w, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect, bool dark, NavDirection direction);
// 空なら何も積まない。
void EmitToast(CmdWriter w, IDWriteTextFormat* fmt, const PaneRect& md_pane_rect, bool dark, std::wstring_view message);
// 指しているリンクの行き先を band (LinkStatusBand) の dock の側に、文字列の幅に合わせて置く。空なら何も積まない。
void EmitLinkStatus(CmdWriter w, IDWriteTextFormat* fmt, const D2D1_RECT_F& band, LinkStatusDock dock, std::wstring_view text);
} // namespace overlay

// タイトルバーとサイドペイン。デバイスに依存しないので、デバイスの作成前にも組み立てられる。
namespace chrome {
// app_icon が null ならアイコンを描かない。
void EmitTitleBar(CmdWriter w, IDWriteTextFormat* icon_fmt, IDWriteTextFormat* title_fmt, ID2D1Bitmap* app_icon, bool dark, const TitleBarRenderState& tb);
// 一覧の書式はペインごとの倍率で作る。
struct SidePaneFormats {
    IDWriteTextFormat* header = nullptr;
    IDWriteTextFormat* header_icon = nullptr;
    IDWriteTextFormat* file_item = nullptr;
    IDWriteTextFormat* toc_item = nullptr;
    // ファイル一覧のアイコン。
    IDWriteTextFormat* file_icon = nullptr;
    // file_item と toc_item だけに掛ける。header は UI のロケールなので、掛けると字形が変わる。
    const FallbackFontMap* item_fonts = nullptr;
};
// 表示中のサイドペインとスプリッタ。
void EmitSidePanes(CmdWriter w, const SidePaneFormats& fmts, const Theme& theme, const SidePaneState& sp);
} // namespace chrome

// ドキュメントデータとビューポート状態から DrawCommandList を生成する。
// 「何を描画するか」と「どう描画するか」（エグゼキュータ）を分離する。
class CommandGenerator {
public:
    // theme・formats・text_layouts は持ち主 (Renderer) のもので、作り直しても同じ場所を指したままでよい。
    // formats と text_layouts は文字列 (リスト番号・チェックボックス・アイコン・プレースホルダ) に使う。
    // 前フレームのレイアウトの破棄 (TextLayoutCache::BeginFrame) は持ち主が行う。
    void Init(const Theme& theme, const MdPaneFormats& formats, TextLayoutCache& text_layouts) noexcept
    {
        theme_ = &theme;
        formats_ = &formats;
        text_layouts_ = &text_layouts;
    }

    // Markdownコンテンツペインのすべての描画コマンドを out の後ろに積む。
    void GenerateMdPane(DrawCommandList& out, const MdPaneParams& params, float dpi_scale);

private:
    const Theme* theme_ = nullptr;
    const MdPaneFormats* formats_ = nullptr;
    TextLayoutCache* text_layouts_ = nullptr;
    // HitTestTextRange の結果。フレームをまたいで容量を使い回す。
    std::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;
};
