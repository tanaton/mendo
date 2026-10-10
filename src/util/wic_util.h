#pragma once
#include "log_hr.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <optional>
#include <utility>

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
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, pixel_format, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    return converter;
}

// WIC デコード結果。ピクセルサイズと FormatConverter を保持する。
struct DecodeResult {
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    // アルファを持たないフレーム (JPEG 等)。変換前に縮小すると JPEG はデコーダ側の縮小デコード
    // (IWICBitmapSourceTransform) が効く。アルファ付きは straight alpha のまま補間すると縁がにじむため持たない。
    Microsoft::WRL::ComPtr<IWICBitmapSource> opaque_frame;
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
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) {
        return std::nullopt;
    }

    auto converter = ConvertBitmapSource(wic, frame.Get(), pixel_format);
    if (!converter) {
        return std::nullopt;
    }

    DecodeResult result{ std::move(converter) };
    if (FAILED(frame->GetSize(&result.pixel_width, &result.pixel_height))) {
        return std::nullopt;
    }
    WICPixelFormatGUID frame_format{};
    if (SUCCEEDED(frame->GetPixelFormat(&frame_format)) &&
        (frame_format == GUID_WICPixelFormat24bppBGR || frame_format == GUID_WICPixelFormat8bppGray)) {
        result.opaque_frame = frame;
    }
    return result;
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
        scale = std::min(scale, static_cast<double>(max_dim) / std::max(width, height));
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
inline Microsoft::WRL::ComPtr<IWICBitmap> DecodeToWicBitmap(IWICImagingFactory* wic, const DecodeResult& decoded, PixelSize target)
{
    Microsoft::WRL::ComPtr<IWICBitmapSource> src = decoded.converter;
    if (target != PixelSize{ decoded.pixel_width, decoded.pixel_height }) {
        // 縮小できなければ失敗扱いにする。原寸へ倒すとサイズ上限を素通りしてメモリが跳ねる。
        IWICBitmapSource* scale_src = decoded.opaque_frame ? decoded.opaque_frame.Get() : decoded.converter.Get();
        Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(scale_src, target.width, target.height, WICBitmapInterpolationModeFant))) {
            return nullptr;
        }
        src = scaler;
        if (decoded.opaque_frame) {
            src = ConvertBitmapSource(wic, scaler.Get());
            if (!src) {
                return nullptr;
            }
        }
    }
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmapFromSource(src.Get(), WICBitmapCacheOnLoad, &bitmap))) {
        return nullptr;
    }
    return bitmap;
}

} // namespace wic_util
