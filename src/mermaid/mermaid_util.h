#pragma once
#include "utility.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

struct Node;
struct MathDrawing;

namespace mermaid_util {

uint64_t NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept;
// NodeDiagramHash のうちノードのテキストだけで決まる部分。幅とテーマは DiagramRequestHash で混ぜる。
// テキストのハッシュは図の大きさに比例するので、覚えておけば要求し直すたびに読み直さずに済む。
uint64_t DiagramCodeHash(const Node& node) noexcept;
// cached は DiagramEntry::code_hash。0 (未計算) のときだけテキストを読む。
uint64_t EnsureDiagramCodeHash(uint64_t& cached, const Node& node) noexcept;
uint64_t DiagramRequestHash(uint64_t code_hash, float max_width, bool dark_mode) noexcept;
// pele は幅の上限より狭く描けた図をどの幅でも同じに描く (mermaid の文書の例と test.md の 559 図で確かめた)。
// その結果は幅を混ぜないこのキーでも引けるようにし、幅が変わっても描き直さない。
uint64_t DiagramWidthFreeHash(uint64_t code_hash, bool dark_mode) noexcept;
constexpr bool IsWidthFree(float css_width, float max_width) noexcept
{
    return css_width + 1.0f < max_width;
}

// mermaid.html の message の受け口に送る要求。"<種類>:<要求 ID>:..." の形で、図のコードと TeX はエスケープせずに送る。
// 図は "<種類>:<ID>:<ダーク 0/1>:<幅の上限 (CSS px)>:<本文のロケール>:<コード>" とコードを末尾に置く。
// ロケールは日本語などのフォールバック先を本文 (Theme::text_locale) とそろえるのに使う。
enum class PageRequest : uint8_t {
    Mermaid, // 表示用の図 (render-result で大きさを返す)
    Svg,     // クリップボード用の SVG (svg-result)
    View,    // 拡大表示用の SVG (svg-result)
    Math,    // インライン数式。ページには送らず、PeleEngine が KaTeX で描く
};
std::wstring PageRenderMessage(PageRequest kind, unsigned id, bool dark, float max_width, std::wstring_view locale, std::string_view code_utf8);
// 新しい大きさで描き直したら capture-ready を返させる。
std::wstring PageCaptureMessage(unsigned id);

// mermaid_viewer.js の message の受け口に送る要求。
// "init:<ダーク 0/1>:" に続けて、文言を改行で区切って並べる (i18n の決まった文言なので改行を含まない)。
std::wstring ViewerInitMessage(bool dark, std::span<const std::wstring_view> labels);
// "svg:<ダーク 0/1>:<SVG>"。SVG を末尾に置き、エスケープしない。
std::wstring ViewerSvgMessage(bool dark, std::wstring_view svg);

int QuantizeWidth(float max_width) noexcept;

// ページから届く値は、pele に XSS があれば任意に偽れる。描画とキャッシュに使う前に確かめる。
constexpr bool IsPlausibleDpr(float dpr) noexcept
{
    return dpr >= 0.25f && dpr <= 8.0f;
}

// キャプチャする物理ピクセルが max_side_px (ビットマップの最大辺) と 1 枚あたりの確保量の上限に収まるか。
// 収まらない図はビットマップを作れず、描き直しを繰り返すことになる。
bool FitsCaptureLimit(float css_width, float css_height, float dpr, unsigned max_side_px) noexcept;
// 1 枚の画素数の上限。
inline constexpr double kMaxCapturePixels = 64.0 * 1024 * 1024;

// エラーメッセージの表示用整形: 空白・改行・制御文字の連続を空白1個に潰し、max_len で
// 切り詰める。切り詰め時は末尾に U+2026 を付ける。
std::wstring SanitizeErrorMessage(std::wstring_view msg, size_t max_len);

struct RequestPrefix {
    unsigned int id = 0;
    bool valid = false;
    bool has_payload = false;
    std::wstring_view payload;
};
RequestPrefix ParseRequestPrefix(std::wstring_view body) noexcept;

enum class WebMessageKind : uint8_t {
    Unknown,
    Ready,         // "mermaid-ready:<dpr>"
    RenderResult,  // "render-result:<id>:<width>:<height>:<dpr>"
    DrawResult,    // "draw-result:<id>:<payload>" (ParseDrawing)
    CaptureReady,  // "capture-ready:<id>"
    SvgResult,     // "svg-result:<id>:<payload>"
    RenderError,   // "render-error:<id>:<message>"
    Failed,        // "mermaid-failed"
};

// パース済み WebMessage。kind に応じてどのフィールドが有効か変わる:
//  - Ready:       ready_dpr (devicePixelRatio)
//  - Render*/Capture/Svg: request (id, payload)
//  - Failed/Unknown: フィールドなし
struct ParsedWebMessage {
    WebMessageKind kind = WebMessageKind::Unknown;
    float ready_dpr = 0.0f;
    RequestPrefix request{};
};

ParsedWebMessage ParseWebMessage(std::wstring_view msg) noexcept;

// RenderResult の payload ("<width>:<height>:<dpr>")。欠けた寸法と有限でない寸法 (nan / inf) は 0、ありえない dpr は 1。
struct RenderSize {
    float width = 0.0f;
    float height = 0.0f;
    float dpr = 0.0f;

    bool operator==(const RenderSize&) const = default;
};
RenderSize ParseRenderSize(std::wstring_view payload) noexcept;

// 図の文字の並び 1 本。DirectWrite で 1 行の TextLayout にして、ベースラインの始点 (x, y) に置く。
struct DrawingText {
    // SVG の matrix(a b c d e f)。文字の要素の座標から、図の左上を原点とする CSS px へ。
    std::array<float, 6> transform{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    float x = 0.0f;
    float y = 0.0f;
    float font_size = 0.0f;
    uint16_t weight = 400;
    bool italic = false;
    bool underline = false;
    uint32_t rgb = 0; // 0xRRGGBB
    float alpha = 1.0f;
    std::wstring family; // CSS の font-family (候補の並び)
    std::wstring text;

    bool operator==(const DrawingText&) const = default;
};
// 図のラベルの数式 ($$...$$)。KaTeX で組んだ描画命令を、ベースラインの左端 (x, y) に置いて文字の色で描く。
struct DrawingMath {
    std::array<float, 6> transform{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }; // DrawingText と同じ
    float x = 0.0f;
    float y = 0.0f;
    float font_size = 0.0f; // ラベルの文字の大きさ。KaTeX と同じくこの 1.21 倍で組む
    uint32_t rgb = 0;       // 0xRRGGBB
    float alpha = 1.0f;
    std::string tex; // UTF-8。ディスクキャッシュには描画命令でなくこれを置き、読んだときに組み直す
    // 組めなかった (読んだ後にまだ組んでいない) なら null。比べない。
    std::shared_ptr<const MathDrawing> drawing;

    bool operator==(const DrawingMath& o) const noexcept
    {
        return transform == o.transform && x == o.x && y == o.y && font_size == o.font_size && rgb == o.rgb && alpha == o.alpha && tex == o.tex;
    }
};
// 図形を描いてから文字を描く 1 層。図形は文字を除き、色・不透明度・変換を属性に確定させた SVG (UTF-8) で、
// ID2D1SvgDocument が読める要素だけを使う。
struct DrawingLayer {
    std::string shapes_svg;
    std::vector<DrawingText> texts;
    std::vector<DrawingMath> maths; // 文字の後に描く

    bool operator==(const DrawingLayer&) const = default;
};
// ページが pele の SVG から書き出した描画命令 (DrawResult)。キャプチャの代わりにこれを CPU で描く。
// 層は SVG の文書の順の重なりを保つためのもの。図形がその層の文字と重なるときだけ、ページが次の層を始める。
struct DiagramDrawing {
    RenderSize size;         // 図の大きさ (CSS px、切り上げ済み) と dpr
    uint32_t background = 0; // 0xRRGGBB
    std::vector<DrawingLayer> layers;

    bool operator==(const DiagramDrawing&) const = default;
};
// DrawResult の payload。
// "<幅>:<高さ>:<dpr>:<背景 (10 進)>:<層の数>:" に続けて、層ごとに "<SVG の長さ>:<SVG><文字の並びの数>:" と並び、
// "<数式の数>:" と数式を置く。
// 並びは "<a>,<b>,<c>,<d>,<e>,<f>,<x>,<y>,<大きさ>,<太さ>,<斜体 0/1>,<下線 0/1>,<色 (10 進)>,<不透明度>:<書体の長さ>:<書体><文字の長さ>:<文字>"。
// 数式は "<a>,<b>,<c>,<d>,<e>,<f>,<x>,<y>,<大きさ>,<色 (10 進)>,<不透明度>:<TeX の長さ>:<TeX>" で、drawing は null のまま返す。
// 長さは UTF-16 の単位。ページは XSS で偽れるので、欠け・余り・ありえない値があれば nullopt。
std::optional<DiagramDrawing> ParseDrawing(std::wstring_view payload);
// ParseDrawing が読む形の UTF-8。C++ で作った描画命令をディスクキャッシュに置くのに使う。
std::string SerializeDrawing(const DiagramDrawing& drawing);
// メモリのキャッシュの上限に数える、描画命令のおおよその大きさ (バイト)。
size_t DrawingBytes(const DiagramDrawing& drawing) noexcept;

} // namespace mermaid_util
