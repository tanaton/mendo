#pragma once
#include "mermaid_util.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct ID2D1CommandList;
struct ID2D1DeviceContext5;
struct ID2D1RenderTarget;
struct ID2D1Factory;
struct IDWriteFactory;
struct IDWriteFontCollection;
struct IDWriteTextFormat;

// 図の背景。拡大表示の窓もそろえる。
inline constexpr uint32_t kDiagramLightBackground = 0xFFFFFF;
inline constexpr uint32_t kDiagramDarkBackground = 0x1E1E1E;

// CSS の font-family の候補のうち fonts にある書体の名前を順に返す。総称 (sans-serif など) は本文と同じ書体に読み替え、
// 重なりは除く。図の文字は先頭の書体で組み、残りをシステムより先にフォールバック先にする。
std::vector<std::wstring> ResolveFontFamilies(IDWriteFontCollection* fonts, std::wstring_view css_family);

// 図の文字の書式。family は CSS の font-family (候補の並び)。
struct DiagramFont {
    std::wstring_view family;
    float size = 16.0f;
    uint16_t weight = 400;
    bool italic = false;
};

// 図の文字の書式・幅・行の高さ。pele の配置 (measurer)、描画命令の文字の位置、描画 (RecordDiagram など) が同じ書式を使うので、
// 字形も幅も一致する。結果を覚えるので、スレッドごとに 1 つ持つ。
class DiagramTextMeasurer {
public:
    DiagramTextMeasurer(IDWriteFactory* dw, std::wstring locale);
    ~DiagramTextMeasurer();
    DiagramTextMeasurer(const DiagramTextMeasurer&) = delete;
    DiagramTextMeasurer& operator=(const DiagramTextMeasurer&) = delete;

    IDWriteTextFormat* Format(const DiagramFont& font);
    // 末尾の空白を含めた送り幅 (CSS px)。
    float Width(std::wstring_view text, const DiagramFont& font);
    // ベースラインから行の上端までと下端まで (どちらも正)。
    std::pair<float, float> Extent(const DiagramFont& font);

private:
    struct FontEntry {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        std::optional<std::pair<float, float>> extent;
    };
    FontEntry& Entry(const DiagramFont& font);

    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    std::wstring locale_;
    std::wstring key_;
    std::unordered_map<std::wstring, FontEntry> entries_;
    std::unordered_map<std::wstring, float> widths_;
};

// pele の math オプションに返した数式のラベル。pele は {svg} を <svg class="pele-math"> の中に入れるので、
// 中身を <g data-mendo-math="番号"/> にしておき、DrawingFromPeleSvg が番号でこれに置き換える。
struct PeleMathLabel {
    std::string tex;
    float font_size = 0.0f;
    std::shared_ptr<const MathDrawing> drawing;
};

// pele の SVG を描画命令にする。max_width は pele に渡した幅の上限で、SVG はブラウザと同じく max-width:100% で縮める (0 なら縮めない)。
// 書き出せない要素 (<foreignObject> や image など) を含む SVG と壊れた SVG は nullopt。
std::optional<mermaid_util::DiagramDrawing> DrawingFromPeleSvg(std::string_view svg, bool dark, float max_width, DiagramTextMeasurer& measurer,
                                                               std::span<const PeleMathLabel> maths = {});

// 描画命令をそのまま 1 枚の SVG (UTF-8) にする。クリップボード用で、Office や Inkscape が読めるよう CSS を使わず属性だけで書く。
// 図形は層の図形をそのまま、文字は 1 行ずつの <text> にする。数式のラベルは $$TeX$$ の文字のまま書く。背景は塗らない。
std::string DrawingToSvg(const mermaid_util::DiagramDrawing& drawing);

// target と同じデバイスで、図をコマンドリストに記録する DeviceContext を作る。描いている target とは別に要る。
Microsoft::WRL::ComPtr<ID2D1DeviceContext5> CreateRecordContext(ID2D1RenderTarget* target);
// 図をコマンドリストに記録する (毎フレームそれを描く)。記録できなければ null。
Microsoft::WRL::ComPtr<ID2D1CommandList> RecordDiagram(ID2D1DeviceContext5* record_dc, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                       const mermaid_util::DiagramDrawing& drawing, const wchar_t* locale);

// 図の描画命令を CPU で描き、画素を確定させたビットマップにする (保存・コピーの画像)。CSS px を dpr 倍した 32bpp PBGRA に描く。
// locale は文字の書式のロケール。描けなければ null。
Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                    const mermaid_util::DiagramDrawing& drawing, float dpr, const wchar_t* locale);

// ---------------------------------------------------------------------------------------------------------------
// KaTeX の HTML 出力を、ブラウザを使わずに配置して描く。

struct IDWriteFontFace;
struct ID2D1Brush;
struct ID2D1PathGeometry;

// KaTeX の dist/fonts の 20 書体。KatexAssets に渡す順 (埋め込みリソースも scripts/update_js_libs.ps1 もこの順)。
inline constexpr std::wstring_view kKatexFontFiles[]{
    L"KaTeX_AMS-Regular",       L"KaTeX_Caligraphic-Bold", L"KaTeX_Caligraphic-Regular", L"KaTeX_Fraktur-Bold",     L"KaTeX_Fraktur-Regular",
    L"KaTeX_Main-Bold",         L"KaTeX_Main-BoldItalic",  L"KaTeX_Main-Italic",         L"KaTeX_Main-Regular",     L"KaTeX_Math-BoldItalic",
    L"KaTeX_Math-Italic",       L"KaTeX_SansSerif-Bold",   L"KaTeX_SansSerif-Italic",    L"KaTeX_SansSerif-Regular", L"KaTeX_Script-Regular",
    L"KaTeX_Size1-Regular",     L"KaTeX_Size2-Regular",    L"KaTeX_Size3-Regular",       L"KaTeX_Size4-Regular",    L"KaTeX_Typewriter-Regular",
};

// 数式の描画命令。座標は CSS px で、原点は箱の左端のベースライン (y は下向き)。
struct MathDrawing {
    // \color などで指定した色 (0xRRGGBBAA)。指定が無ければ描くときの色 (本文の色) で塗るので、ダークモードやリンクの色で
    // 描き直さずに済む。指定した色はダークモードでも変えない (GitHub と同じ。#412 でユーザーが決めた)。
    using Color = std::optional<uint32_t>;
    struct Glyphs {
        Microsoft::WRL::ComPtr<IDWriteFontFace> face;
        float size = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        bool mirror = false; // \reflectbox。字形の並びの中央で左右を反転する
        Color color;
        std::vector<uint16_t> glyphs;
        std::vector<float> advances;
    };
    // 塗りの矩形 (分数の線・\rule・枠・\colorbox の背景)。外側の端を装置の画素に合わせ、枠の太さは CSS px に切り捨てる。
    // 細くても 1px は残す (Chrome の border と同じ)。Fill は太さを持たない背景。
    struct Rect {
        enum class Side : uint8_t { Top, Right, Bottom, Left, Fill };
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        Side side = Side::Bottom;
        bool dashed = false;
        Color color;
    };
    // SVG の path の形。ジオメトリは UI スレッドで描くときに、描く先のファクトリで作る。同じ形は数式をまたいで共有する
    // (\sqrt や矢印は同じ path を何度も使う)。
    struct Shape {
        std::string d;
        mutable Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        mutable Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    };
    // SVG の path (塗り) と line (線)。transform は形の座標から箱の座標へ、clip は箱の座標。
    struct Path {
        std::shared_ptr<const Shape> shape;
        std::array<float, 6> transform{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        std::array<float, 4> clip{}; // left, top, right, bottom
        float stroke = 0.0f;         // 0 なら塗る
        Color color;
    };
    using Item = std::variant<Glyphs, Rect, Path>;

    float width = 0.0f;
    float height = 0.0f; // ベースラインより上
    float depth = 0.0f;  // ベースラインより下
    std::vector<Item> items;
};

// KaTeX の書体 (woff2) と、KaTeX の書体に無い文字のフォールバック先、数式の間で共有する SVG の形。どのスレッドからでも使える。
class KatexAssets {
public:
    // woff2 は kKatexFontFiles の順。バイト列はこのオブジェクトより長く生かす (埋め込みリソース)。
    // locale は KaTeX の書体に無い文字 (\text{} の日本語など) のフォールバック先を本文とそろえるのに使う。
    KatexAssets(IDWriteFactory* dw, std::span<const std::span<const std::byte>> woff2, std::wstring locale);
    ~KatexAssets();
    KatexAssets(const KatexAssets&) = delete;
    KatexAssets& operator=(const KatexAssets&) = delete;

    bool ok() const noexcept
    {
        return ok_;
    }
    // simulations は DWRITE_FONT_SIMULATIONS。
    IDWriteFontFace* Face(size_t file, uint32_t simulations);
    // KaTeX の書体に無い文字の書体。見つからなければ null。
    IDWriteFontFace* Fallback(uint32_t cp, bool bold, bool italic);
    std::shared_ptr<const MathDrawing::Shape> Shape(std::string_view d);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ok_ = false;
};

// KaTeX の renderToString (output: 'html'、displayMode) の結果を配置する。font_size は数式を置く場所の文字の大きさ (CSS px) で、
// KaTeX の CSS と同じく 1.21 倍して組む。align などの自動の式番号は数式ごとに (1) から振る。解釈できない要素があれば nullopt。
std::optional<MathDrawing> LayoutKatexHtml(std::string_view html, float font_size, KatexAssets& assets);
// (x, y) を数式のベースラインの左端として、色の指定の無い部分を brush で描く。指定した色には brush の不透明度を掛ける。
// 罫線・SVG の箱・ベースラインは、rt の今の変換と DPI から
// 装置の画素に合わせる (変換は拡大と平行移動だけとみなす)。
void DrawMath(ID2D1RenderTarget* rt, const MathDrawing& math, float x, float y, ID2D1Brush* brush);
