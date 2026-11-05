#pragma once
#include "utility.h"
#include <array>
#include <memory>
#include <optional>
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
uint64_t DiagramCodeHash(std::string_view code) noexcept;
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

int QuantizeWidth(float max_width) noexcept;

// エラーメッセージの表示用整形: 空白・改行・制御文字の連続を空白1個に潰し、max_len で
// 切り詰める。切り詰め時は末尾に U+2026 を付ける。
std::wstring SanitizeErrorMessage(std::wstring_view msg, size_t max_len);

// 図の文字と数式のラベルの大きさ (CSS px) の上限 (IsStorable)。文字の大きさは CSS の計算値をここへ丸める。
// 数式のラベルは、pele が style や fontSize の指定によらず決まった大きさで組ませるので外れない。
inline constexpr float kMaxFontSize = 1000.0f;

// 図の文字と数式のラベルを置く位置と色。(x, y) はベースラインの左端。
struct DrawingPlacement {
    // SVG の matrix(a b c d e f)。要素の座標から、図の左上を原点とする CSS px へ。
    std::array<float, 6> transform{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    float x = 0.0f;
    float y = 0.0f;
    // 数式はラベルの文字の大きさで、KaTeX と同じくこの 1.21 倍で組む。
    float font_size = 0.0f;
    uint32_t rgb = 0; // 0xRRGGBB
    float alpha = 1.0f;

    bool operator==(const DrawingPlacement&) const = default;
};
// ディスクキャッシュに書ける置き方か。変換と位置が有限で、大きさが 0 より大きく kMaxFontSize 以下、色が 24 ビット、
// 不透明度が 0〜1。図を作る側は満たさない文字と数式を描かず、読む側は満たさないものを壊れた値として読まない。
// 両側の値域をここでそろえる (外れたものを書くと読み戻せず、開くたびに描き直す)。
bool IsStorable(const DrawingPlacement& p) noexcept;
// 図の文字の並び 1 本。DirectWrite で 1 行の TextLayout にして置く。
struct DrawingText {
    DrawingPlacement at;
    uint16_t weight = 400;
    bool italic = false;
    bool underline = false;
    std::wstring family; // CSS の font-family (候補の並び)
    std::wstring text;

    bool operator==(const DrawingText&) const = default;
};
// 図のラベルの数式 ($$...$$)。KaTeX で組んだ描画命令を、文字の色で置く。
struct DrawingMath {
    DrawingPlacement at;
    std::string tex; // UTF-8。ディスクキャッシュには描画命令でなくこれを置き、読んだときに組み直す
    // 組めなかった (読んだ後にまだ組んでいない) なら null。比べない。
    std::shared_ptr<const MathDrawing> drawing;

    bool operator==(const DrawingMath& o) const noexcept
    {
        return at == o.at && tex == o.tex;
    }
};
// 図形を描いてから文字を描く 1 層。図形は文字を除き、色・不透明度・変換を属性に確定させた SVG (UTF-8) の <svg> の中身で、
// ID2D1SvgDocument が読める要素だけを使う。<svg> と <defs> は描くときに付ける (層ごとに同じものを持たない)。
struct DrawingLayer {
    std::string shapes;
    std::vector<DrawingText> texts;
    std::vector<DrawingMath> maths; // 文字の後に描く

    bool operator==(const DrawingLayer&) const = default;
};
// pele の SVG から作った描画命令。
// 層は SVG の文書の順の重なりを保つためのもの。図形がその層の文字と重なるときだけ、次の層を始める。
struct DiagramDrawing {
    float width = 0.0f; // 図の大きさ (CSS px、切り上げ済み)
    float height = 0.0f;
    uint32_t background = 0; // 0xRRGGBB
    std::string defs;        // 層の図形が参照する <defs> の中身 (グラデーション)
    std::vector<DrawingLayer> layers;

    bool operator==(const DiagramDrawing&) const = default;
};
// ディスクキャッシュに置く描画命令の書式。
// "<幅>:<高さ>:<背景 (10 進)>:<defs の長さ>:<defs><層の数>:" に続けて、層ごとに "<図形の長さ>:<図形><文字の並びの数>:" と並び、
// "<数式の数>:" と数式を置く。
// 置き方は "<a>,<b>,<c>,<d>,<e>,<f>,<x>,<y>,<大きさ>,<色 (10 進)>,<不透明度>"。
// 並びは "<置き方>,<太さ>,<斜体 0/1>,<下線 0/1>:<書体の長さ>:<書体><文字の長さ>:<文字>"。
// 数式は "<置き方>:<TeX の長さ>:<TeX>" で、drawing は null のまま返す。
// 長さは UTF-16 の単位。ファイルは壊れていることがあるので、欠け・余り・ありえない値 (IsStorable を満たさない置き方など)
// があれば nullopt。
std::optional<DiagramDrawing> ParseDrawing(std::wstring_view payload);
// ParseDrawing が読む形の UTF-8。C++ で作った描画命令をディスクキャッシュに置くのに使う。
std::string SerializeDrawing(const DiagramDrawing& drawing);
// メモリのキャッシュの上限に数える、描画命令のおおよその大きさ (バイト)。
size_t DrawingBytes(const DiagramDrawing& drawing) noexcept;

} // namespace mermaid_util
