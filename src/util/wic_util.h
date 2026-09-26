#pragma once
#include "log_hr.h"
#include <wincodec.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <algorithm>
#include <optional>

namespace wic_util {

// CLSID_WICImagingFactory の CoCreateInstance を共通化する。
// 失敗時は context 文字列付きで LogHrFailure を流し nullptr を返す。
inline Microsoft::WRL::ComPtr<IWICImagingFactory> CreateWicFactory(const wchar_t* context) noexcept
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    const HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        mendo::LogHrFailure(context, hr);
        return nullptr;
    }
    return factory;
}

// IWICBitmapSource を GUID_WICPixelFormat32bppPBGRA に変換する。
// HICON 由来の IWICBitmap など、デコーダを経由しないソースにも使用する。
//
// 注意: IWICFormatConverter は呼び出し元が CreateBitmapFromWicBitmap などで
// 利用するため、この関数の戻り値の lifetime に渡って保持される。
// 別スレッド/別呼び出しで Initialize を再呼び出ししてしまうと既存の戻り値が
// 別ソースを指してしまうため、毎回新規生成する（プールしない）。
// CreateFormatConverter 自体は CoCreateInstance に比べて軽量。
inline Microsoft::WRL::ComPtr<IWICFormatConverter> ConvertBitmapSource(
    IWICImagingFactory* wic, IWICBitmapSource* source,
    const WICPixelFormatGUID& pixel_format = GUID_WICPixelFormat32bppPBGRA)
{
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    HRESULT hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return nullptr;
    }

    hr = converter->Initialize(
        source, pixel_format,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return nullptr;
    }

    return converter;
}

// WIC デコード結果。ピクセルサイズと FormatConverter を保持する。
struct DecodeResult {
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    UINT pixel_width = 0;
    UINT pixel_height = 0;
};

// IStream から画像をデコードし、GUID_WICPixelFormat32bppPBGRA 形式の
// FormatConverter とピクセルサイズを返す。converter の lifetime 注意は
// ConvertBitmapSource を参照。
inline std::optional<DecodeResult> DecodeFromStream(
    IWICImagingFactory* wic, IStream* stream,
    const WICPixelFormatGUID& pixel_format = GUID_WICPixelFormat32bppPBGRA)
{
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    auto converter = ConvertBitmapSource(wic, frame.Get(), pixel_format);
    if (!converter) {
        return std::nullopt;
    }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    return DecodeResult{ std::move(converter), w, h };
}

struct PixelSize {
    UINT width = 0;
    UINT height = 0;
    constexpr bool operator==(const PixelSize&) const = default;
};

// 表示に要る解像度までアスペクト比を保って縮小したサイズ。拡大はしない。
// max_width: 表示で使われうる最大幅 (px)、max_dim: GPU ビットマップの最大辺 (px)。0 は無制限。
constexpr PixelSize ComputeDecodeSize(UINT width, UINT height, UINT max_width, UINT max_dim) noexcept
{
    if (width == 0 || height == 0) {
        return { width, height };
    }
    double scale = 1.0;
    if (max_width > 0 && width > max_width) {
        scale = static_cast<double>(max_width) / width;
    }
    if (max_dim > 0) {
        const UINT longest = width > height ? width : height;
        scale = std::min(scale, static_cast<double>(max_dim) / longest);
    }
    if (scale >= 1.0) {
        return { width, height };
    }
    const auto scaled = [scale](UINT v) {
        const auto s = static_cast<UINT>(v * scale + 0.5);
        return s > 0 ? s : 1u;
    };
    return { scaled(width), scaled(height) };
}

// PBGRA へのデコードをこのスレッドで確定させた IWICBitmap を作る。IWICFormatConverter のまま
// 渡すと実デコードは CreateBitmapFromWicBitmap (UI スレッド) の CopyPixels まで遅延される。
inline Microsoft::WRL::ComPtr<IWICBitmap> DecodeToWicBitmap(IWICImagingFactory* wic, IWICBitmapSource* source, PixelSize original, PixelSize target)
{
    Microsoft::WRL::ComPtr<IWICBitmapSource> src = source;
    if (target != original) {
        Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(source, target.width, target.height, WICBitmapInterpolationModeFant))) {
            src = scaler;
        }
    }
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmapFromSource(src.Get(), WICBitmapCacheOnLoad, &bitmap))) {
        return nullptr;
    }
    return bitmap;
}

struct CreatedBitmap {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    UINT pixel_width = 0;
    UINT pixel_height = 0;
};

// IStream から WIC デコード -> D2D ビットマップ生成までを一括で行う。
inline std::optional<CreatedBitmap> CreateD2DBitmapFromStream(IWICImagingFactory* wic, ID2D1RenderTarget* rt, IStream* stream)
{
    if (!wic || !rt || !stream) {
        return std::nullopt;
    }
    auto decoded = DecodeFromStream(wic, stream);
    if (!decoded) {
        return std::nullopt;
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    const HRESULT hr = rt->CreateBitmapFromWicBitmap(decoded->converter.Get(), &bitmap);
    if (FAILED(hr)) {
        return std::nullopt;
    }
    return CreatedBitmap{ std::move(bitmap), decoded->pixel_width, decoded->pixel_height };
}

} // namespace wic_util
