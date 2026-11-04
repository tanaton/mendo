#pragma once
#include "mermaid_util.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <string_view>

struct ID2D1DeviceContext5;
struct ID2D1Factory;
struct IDWriteFactory;
struct IDWriteFontCollection;

// CSS の font-family の候補から、fonts にある最初の書体の名前を返す。総称 (sans-serif など) は本文と同じ書体に読み替え、
// どれも無ければ Segoe UI。Segoe UI に無い文字は、書式のロケールで DirectWrite のフォールバックが決める。
std::wstring ResolveFontFamily(IDWriteFontCollection* fonts, std::wstring_view css_family);

// dc の今の変換に重ねて、図の左上を原点とする CSS px で、背景から描く。BeginDraw と EndDraw の間で呼ぶ。
// wic は図形の SVG を読むストリームに使う。描けなければ false (描きかけのまま)。
bool DrawDiagram(ID2D1DeviceContext5* dc, IDWriteFactory* dw, IWICImagingFactory* wic, const mermaid_util::DiagramDrawing& drawing,
                 const wchar_t* locale);

// 図の描画命令を CPU で描き、画素を確定させたビットマップにする (保存・コピーの PNG)。CSS px を dpr 倍した 32bpp PBGRA に描く。
// locale は文字の書式のロケール。描けなければ null。
Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                    const mermaid_util::DiagramDrawing& drawing, float dpr, const wchar_t* locale);
