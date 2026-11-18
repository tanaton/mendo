#pragma once
// KaTeX の HTML 出力を、ブラウザを使わずに配置して描く。本文の数式を描く描画側 (render/) が、図の API 一式 (mermaid_util.h・
// wincodec.h) を読み込まずに済むよう diagram_raster.h から分けた。実装は CSS の値の解析や SVG の経路 (WalkPath) を図と
// 共有するので diagram_raster.cpp にある。
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct ID2D1Brush;
struct ID2D1Device;
struct ID2D1Factory;
struct ID2D1PathGeometry;
struct ID2D1RenderTarget;

// KaTeX の dist/fonts の 20 書体。KatexAssets に渡す順 (埋め込みリソースも scripts/update_js_libs.ps1 もこの順)。
inline constexpr std::wstring_view kKatexFontFiles[]{
    L"KaTeX_AMS-Regular",
    L"KaTeX_Caligraphic-Bold",
    L"KaTeX_Caligraphic-Regular",
    L"KaTeX_Fraktur-Bold",
    L"KaTeX_Fraktur-Regular",
    L"KaTeX_Main-Bold",
    L"KaTeX_Main-BoldItalic",
    L"KaTeX_Main-Italic",
    L"KaTeX_Main-Regular",
    L"KaTeX_Math-BoldItalic",
    L"KaTeX_Math-Italic",
    L"KaTeX_SansSerif-Bold",
    L"KaTeX_SansSerif-Italic",
    L"KaTeX_SansSerif-Regular",
    L"KaTeX_Script-Regular",
    L"KaTeX_Size1-Regular",
    L"KaTeX_Size2-Regular",
    L"KaTeX_Size3-Regular",
    L"KaTeX_Size4-Regular",
    L"KaTeX_Typewriter-Regular",
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
        // 色のフォント (絵文字の Segoe UI Emoji) の字形。本文と同じく DrawColorGlyphRun で描く。毎フレーム書体に
        // 問い合わせないよう、組むときに決める。
        bool color_font = false;
        Color color;
        std::vector<uint16_t> glyphs;
        std::vector<float> advances;
    };
    // 塗りの矩形 (分数の線・\rule・枠・\colorbox の背景)。外側の端を装置の画素に合わせ、枠の太さは CSS px に切り捨てる。
    // 細くても 1px は残す (Chrome の border と同じ)。Fill は太さを持たない背景。
    struct Rect {
        enum class Side : uint8_t {
            Top,
            Right,
            Bottom,
            Left,
            Fill
        };
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

// 数式の SVG の形 (\sqrt・矢印・大きな括弧) を A8 のマスクに一度描いて覚え、以後は FillOpacityMask で塗る。形の箱は左上を
// 装置の画素に合わせて描くので、拡大率が同じなら画素は置く位置に依らず、同じ形の数式の間で共有する。毎フレーム形を塗ると、
// 右下が画素に合わない clip と塗りの作り直しで、形の多い数式の画面の 1 フレームが 1.3ms かかった (マスクで 0.44ms、グリフの
// run もまとめて 0.34ms。BM_MathFrameDraw/shapes)。画素の差は最大 14 階調で、ほぼ 1 階調。ID2D1GeometryRealization は細い
// 矢印の軸が濃くなる (DPI 96 で 113 階調) ので使わない。描く先のデバイスのビットマップを持つ。UI スレッドだけで使う。
class MathMaskCache {
public:
    // 1 画面ぶんは形の多い文書でも数枚から数十枚 (BM_MathFrameDraw/shapes で 5 枚)。予算はスクロールで戻ったときに作り直さない分で、
    // 旧版の数式の型 (A8 のページ) と同じ 8MB。
    static constexpr size_t kBudgetBytes = 8u * 1024 * 1024;

    MathMaskCache();
    ~MathMaskCache();
    MathMaskCache(const MathMaskCache&) = delete;
    MathMaskCache& operator=(const MathMaskCache&) = delete;

    // 覚えたマスクを捨て、以後は device の描画先に描く。null なら Fill は描かない。
    void Reset(ID2D1Device* device) noexcept;
    // 1 フレームを描き終えたら呼ぶ。合計が budget_bytes を超えていたら、このフレームで描かなかったマスクを古い順に捨てる。
    void EndFrame(size_t budget_bytes);
    // path の箱の左上を rt の今の座標の (left, top) に、装置の画素が 1 単位あたり scale_x x scale_y の拡大で描く。
    // rt は Reset で渡したデバイスの描画先。描けなければ false で、呼び出し側が形を塗る。
    bool Fill(ID2D1RenderTarget* rt, const MathDrawing::Path& path, ID2D1PathGeometry* geometry, float left, float top, float scale_x,
              float scale_y, ID2D1Brush* brush);

    size_t Count() const noexcept;
    size_t Bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// (x, y) を数式のベースラインの左端として、色の指定の無い部分を brush で描く。指定した色には brush の不透明度を掛ける。
// 絵文字 (Glyphs::color_font) は本文と同じくフォントの色で描く。
// 罫線・SVG の箱・ベースラインは、rt の今の変換と DPI から装置の画素に合わせる (変換は拡大と平行移動だけとみなす)。
// masks があれば SVG の形をマスクで塗る。図のラベルは記録した後で拡大・縮小して描くので渡さない。
void DrawMath(ID2D1RenderTarget* rt, const MathDrawing& math, float x, float y, ID2D1Brush* brush, MathMaskCache* masks = nullptr);
