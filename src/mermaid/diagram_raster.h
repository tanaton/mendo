#pragma once
#include "math_raster.h"
#include "mermaid_util.h"
#include "utility.h"
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct ID2D1CommandList;
struct ID2D1DeviceContext5;
struct ID2D1RenderTarget;
struct ID2D1Factory;

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
    std::unordered_map<std::wstring, FontEntry, StringTransparentHash> entries_;
    std::unordered_map<std::wstring, float, StringTransparentHash> widths_;
};

// pele の math オプションに返した数式のラベル。pele は {svg} を <svg class="pele-math"> の中に入れるので、
// 中身を <g data-mendo-math="番号"/> にしておき、DrawingFromPeleSvg が番号でこれに置き換える。
struct PeleMathLabel {
    std::string tex;
    float font_size = 0.0f;
    std::shared_ptr<const MathDrawing> drawing;
};

// pele の SVG を、縮めない大きさの描画命令にする。幅の上限に合わせるのは描くとき (DiagramRect・FittedDiagramSize)。
// 書き出せない要素 (<foreignObject> や image など) を含む SVG と壊れた SVG は nullopt。
std::optional<mermaid_util::DiagramDrawing> DrawingFromPeleSvg(std::string_view svg, bool dark, DiagramTextMeasurer& measurer,
                                                               std::span<const PeleMathLabel> maths = {});

// 描画命令をそのまま 1 枚の SVG (UTF-8) にする。クリップボード用で、Office や Inkscape が読めるよう CSS を使わず属性だけで書く。
// 図形は層の図形をそのまま、文字は 1 行ずつの <text> にする。数式のラベルは $$TeX$$ の文字のまま書く。背景は塗らない。
std::string DrawingToSvg(const mermaid_util::DiagramDrawing& drawing);

// target と同じデバイスで、図をコマンドリストに記録する DeviceContext を作る。描いている target とは別に要る。
Microsoft::WRL::ComPtr<ID2D1DeviceContext5> CreateRecordContext(ID2D1RenderTarget* target);

// 図の文字の並びを組んだもの。層の順、層の中は文字の並びの順。空の並びと組めなかった並びは null。
struct DiagramTextLayouts {
    struct Run {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        float baseline = 0.0f;
    };
    std::vector<Run> runs;
};
// 図の文字の並びを組み、整形まで済ませる。記録 (UI スレッド) の 8 割は文字を組む手間なので、worker で先に組んで
// RecordDiagram に渡す。
DiagramTextLayouts LayoutDiagramTexts(IDWriteFactory* dw, const mermaid_util::DiagramDrawing& drawing, const wchar_t* locale);
// 図をコマンドリストに記録する (毎フレームそれを描く)。記録できなければ null。texts は drawing から LayoutDiagramTexts で
// 組んだもので、無ければここで組む。
Microsoft::WRL::ComPtr<ID2D1CommandList> RecordDiagram(ID2D1DeviceContext5* record_dc, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                       const mermaid_util::DiagramDrawing& drawing, const wchar_t* locale,
                                                       const DiagramTextLayouts* texts = nullptr);

// 図の描画命令を CPU で描き、画素を確定させたビットマップにする (保存・コピーの画像)。図を size (CSS px) に伸縮し、
// dpr 倍した 32bpp PBGRA に描く。locale は文字の書式のロケール。描けなければ null。
Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                    const mermaid_util::DiagramDrawing& drawing, mermaid_util::DiagramSize size, float dpr,
                                                    const wchar_t* locale);
