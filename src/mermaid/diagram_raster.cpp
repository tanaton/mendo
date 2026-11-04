#include "diagram_raster.h"
#include "wic_util.h"
#include <d2d1_3.h>
#include <dwrite.h>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace std::literals;

namespace {

constexpr std::wstring_view kDefaultFamily = L"Segoe UI"sv;

// 本文 (Theme) と同じ書体に読み替える。
constexpr std::pair<std::wstring_view, std::wstring_view> kGenericFamilies[]{
    { L"sans-serif"sv, kDefaultFamily },
    { L"system-ui"sv, kDefaultFamily },
    { L"serif"sv, L"Times New Roman"sv },
    { L"monospace"sv, L"Consolas"sv },
};

std::wstring_view TrimFamily(std::wstring_view s) noexcept
{
    constexpr std::wstring_view kSpace = L" \t\r\n"sv;
    const auto first = s.find_first_not_of(kSpace);
    if (first == std::wstring_view::npos) {
        return {};
    }
    s = s.substr(first, s.find_last_not_of(kSpace) - first + 1);
    if (s.size() >= 2 && (s.front() == L'"' || s.front() == L'\'') && s.back() == s.front()) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

// 図 1 枚の中で同じ書式を何度も作らない。図の文字の書式は数種類なので線形に探す。
class TextFormats {
public:
    TextFormats(IDWriteFactory* dw, IDWriteFontCollection* fonts, const wchar_t* locale) noexcept
        : dw_(dw), fonts_(fonts), locale_(locale)
    {
    }

    IDWriteTextFormat* Get(const mermaid_util::DrawingText& t)
    {
        for (const auto& e : entries_) {
            if (e.css_family == t.family && e.size == t.font_size && e.weight == t.weight && e.italic == t.italic) {
                return e.format.Get();
            }
        }
        Entry e{ .css_family = t.family, .size = t.font_size, .weight = t.weight, .italic = t.italic };
        const std::wstring family = ResolveFontFamily(fonts_, t.family);
        if (FAILED(dw_->CreateTextFormat(family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(t.weight),
                                         t.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         t.font_size, locale_, &e.format))) {
            return nullptr;
        }
        e.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return entries_.emplace_back(std::move(e)).format.Get();
    }

private:
    struct Entry {
        std::wstring css_family;
        float size = 0.0f;
        uint16_t weight = 0;
        bool italic = false;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    };
    IDWriteFactory* dw_;
    IDWriteFontCollection* fonts_;
    const wchar_t* locale_;
    std::vector<Entry> entries_;
};

void DrawRun(ID2D1DeviceContext* dc, IDWriteFactory* dw, TextFormats& formats, ID2D1SolidColorBrush* brush, const mermaid_util::DrawingText& t)
{
    IDWriteTextFormat* format = formats.Get(t);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (!format || t.text.empty() ||
        FAILED(dw->CreateTextLayout(t.text.data(), static_cast<UINT32>(t.text.size()), format, 1.0e6f, 1.0e6f, &layout))) {
        return;
    }
    if (t.underline) {
        layout->SetUnderline(TRUE, { 0, static_cast<UINT32>(t.text.size()) });
    }
    // ページは 1 行ずつ送るので、ベースラインは最初の行のものを使う。
    std::array<DWRITE_LINE_METRICS, 1> line{};
    UINT32 lines = 0;
    layout->GetLineMetrics(line.data(), static_cast<UINT32>(line.size()), &lines);
    if (lines == 0) {
        return;
    }
    const auto& m = t.transform;
    dc->SetTransform(D2D1::Matrix3x2F(m[0], m[1], m[2], m[3], m[4], m[5]));
    brush->SetColor(D2D1::ColorF(t.rgb, t.alpha));
    dc->DrawTextLayout(D2D1::Point2F(t.x, t.y - line[0].baseline), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

} // namespace

std::wstring ResolveFontFamily(IDWriteFontCollection* fonts, std::wstring_view css_family)
{
    while (!css_family.empty()) {
        const auto comma = css_family.find(L',');
        const std::wstring_view name = TrimFamily(css_family.substr(0, comma));
        css_family = comma == std::wstring_view::npos ? std::wstring_view{} : css_family.substr(comma + 1);
        if (name.empty()) {
            continue;
        }
        for (const auto& [generic, family] : kGenericFamilies) {
            if (name == generic) {
                return std::wstring{ family };
            }
        }
        const std::wstring candidate{ name };
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (fonts && SUCCEEDED(fonts->FindFamilyName(candidate.c_str(), &index, &exists)) && exists) {
            return candidate;
        }
    }
    return std::wstring{ kDefaultFamily };
}

Microsoft::WRL::ComPtr<IWICBitmap> RasterizeDiagram(ID2D1Factory1* d2d, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                    const mermaid_util::DiagramDrawing& drawing, UINT width, UINT height,
                                                    const wchar_t* locale)
{
    if (!d2d || !dw || !wic || width == 0 || height == 0) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
        return nullptr;
    }
    const float dpi = 96.0f * drawing.size.dpr;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> dc;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt)) || FAILED(rt.As(&dc))) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dw->GetSystemFontCollection(&fonts)) || FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0), &brush))) {
        return nullptr;
    }

    TextFormats formats(dw, fonts.Get(), locale);
    bool ok = true;
    dc->BeginDraw();
    // 表示のときに縮めて描くので、ClearType の色にじみを持ち込まない。
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->Clear(D2D1::ColorF(drawing.background));
    for (const auto& layer : drawing.layers) {
        if (!layer.shapes_svg.empty()) {
            const std::span bytes{ reinterpret_cast<const uint8_t*>(layer.shapes_svg.data()), layer.shapes_svg.size() };
            const auto stream = wic_util::CreateStreamOnMemory(wic, bytes);
            Microsoft::WRL::ComPtr<ID2D1SvgDocument> doc;
            if (!stream || FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(drawing.size.width, drawing.size.height), &doc))) {
                ok = false;
                break;
            }
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->DrawSvgDocument(doc.Get());
        }
        for (const auto& t : layer.texts) {
            DrawRun(dc.Get(), dw, formats, brush.Get(), t);
        }
    }
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    if (FAILED(dc->EndDraw()) || !ok) {
        return nullptr;
    }
    return bitmap;
}
