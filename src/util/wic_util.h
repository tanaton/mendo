#pragma once
#include <wincodec.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <optional>

namespace wic_util {

// WIC デコード結果。ピクセルサイズと FormatConverter を保持する。
struct DecodeResult {
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    UINT pixel_width = 0;
    UINT pixel_height = 0;
};

// IStream から画像をデコードし、GUID_WICPixelFormat32bppPBGRA 形式の
// FormatConverter とピクセルサイズを返す。
// image_loader（同期/非同期）と mermaid（PNGキャッシュ復元）の両方から使用される。
inline std::optional<DecodeResult> DecodeFromStream(
    IWICImagingFactory* wic, IStream* stream)
{
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    return DecodeResult{ std::move(converter), w, h };
}

// IWICBitmapSource を GUID_WICPixelFormat32bppPBGRA に変換する。
// HICON 由来の IWICBitmap など、デコーダを経由しないソースに使用する。
inline Microsoft::WRL::ComPtr<IWICFormatConverter> ConvertBitmapSource(
    IWICImagingFactory* wic, IWICBitmapSource* source)
{
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    HRESULT hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return nullptr;
    }

    hr = converter->Initialize(
        source, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return nullptr;
    }

    return converter;
}

} // namespace wic_util
