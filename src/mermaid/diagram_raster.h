#pragma once
#include "mermaid_util.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <string_view>

struct ID2D1Factory1;
struct IDWriteFactory;
struct IDWriteFontCollection;

// CSS の font-family の候補から、fonts にある最初の書体の名前を返す。総称 (sans-serif など) は本文と同じ書体に読み替え、
// どれも無ければ Segoe UI。Segoe UI に無い文字は、書式のロケールで DirectWrite のフォールバックが決める。
std::wstring ResolveFontFamily(IDWriteFontCollection* fonts, std::wstring_view css_family);

// 図の描画命令を CPU で描き、このスレッドで画素を確定させたビットマップにする。どのスレッドからも呼べる
// (d2d は D2D1_FACTORY_TYPE_MULTI_THREADED、dw は共有ファクトリ)。width x height (物理 px) の 32bpp PBGRA に、
// CSS px を drawing.size.dpr 倍して描く。locale は文字の書式のロケール。描けなければ null。
Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(ID2D1Factory1* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                    const mermaid_util::DiagramDrawing& drawing, UINT width, UINT height,
                                                    const wchar_t* locale);
