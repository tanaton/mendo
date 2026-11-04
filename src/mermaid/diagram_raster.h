#pragma once
#include "mermaid_util.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct ID2D1DeviceContext5;
struct ID2D1Factory;
struct IDWriteFactory;
struct IDWriteFontCollection;
struct IDWriteTextFormat;

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

// 図の文字の書式・幅・行の高さ。pele の配置 (measurer)、描画命令の文字の位置、描画 (DrawDiagram) が同じ書式を使うので、
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
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts_;
    std::wstring locale_;
    std::wstring key_;
    std::unordered_map<std::wstring, FontEntry> entries_;
    std::unordered_map<std::wstring, float> widths_;
};

// pele の SVG を描画命令にする。max_width は pele に渡した幅の上限で、SVG はページと同じく max-width:100% で縮める。
// size.dpr は 0 のままなので、呼び出し側が入れる。
// 書き出せない要素 (数式のラベルの <foreignObject> など) を含む SVG と壊れた SVG は nullopt。
std::optional<mermaid_util::DiagramDrawing> DrawingFromPeleSvg(std::string_view svg, bool dark, float max_width, DiagramTextMeasurer& measurer);

// dc の今の変換に重ねて、図の左上を原点とする CSS px で、背景から描く。BeginDraw と EndDraw の間で呼ぶ。
// wic は図形の SVG を読むストリームに使う。描けなければ false (描きかけのまま)。
bool DrawDiagram(ID2D1DeviceContext5* dc, IDWriteFactory* dw, IWICImagingFactory* wic, const mermaid_util::DiagramDrawing& drawing,
                 const wchar_t* locale);

// 図の描画命令を CPU で描き、画素を確定させたビットマップにする (保存・コピーの PNG)。CSS px を dpr 倍した 32bpp PBGRA に描く。
// locale は文字の書式のロケール。描けなければ null。
Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                    const mermaid_util::DiagramDrawing& drawing, float dpr, const wchar_t* locale);
