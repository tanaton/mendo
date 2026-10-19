#pragma once
#include "win32_util.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace wic_util {

// CLSID_WICImagingFactory の CoCreateInstance を共通化する。
// 失敗時は context 文字列付きで LogHrFailure を流し nullptr を返す。
inline Microsoft::WRL::ComPtr<IWICImagingFactory> CreateWicFactory(const char* context) noexcept
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogHrFailure(context, hr);
        return nullptr;
    }
    return factory;
}

// bytes をコピーせずに参照するストリーム。bytes はストリームから作ったデコード結果を使い終えるまで生かすこと。
// 空や DWORD を超える大きさなら nullptr。
inline Microsoft::WRL::ComPtr<IWICStream> CreateStreamOnMemory(IWICImagingFactory* wic, std::span<const uint8_t> bytes)
{
    if (!wic || bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max()) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())))) {
        return nullptr;
    }
    return stream;
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

// 文書から指定された画像のコンテナ形式。WIC の内容判定に任せると HEIF・RAW や他社製コーデックなど
// インストール済みのどのデコーダもプロセス内で動かせてしまうので、ブラウザが表示する形式だけを
// 先頭バイトで選ぶ。該当しなければ nullptr。
inline const GUID* SniffImageContainer(std::span<const uint8_t> bytes) noexcept
{
    const auto starts_with = [bytes](std::initializer_list<uint8_t> sig, size_t at = 0) {
        return bytes.size() >= at + sig.size() && std::ranges::equal(bytes.subspan(at, sig.size()), sig);
    };
    if (starts_with({ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A })) {
        return &GUID_ContainerFormatPng;
    }
    if (starts_with({ 0xFF, 0xD8, 0xFF })) {
        return &GUID_ContainerFormatJpeg;
    }
    if (starts_with({ 'G', 'I', 'F', '8' })) {
        return &GUID_ContainerFormatGif;
    }
    if (starts_with({ 'B', 'M' })) {
        return &GUID_ContainerFormatBmp;
    }
    if (starts_with({ 'R', 'I', 'F', 'F' }) && starts_with({ 'W', 'E', 'B', 'P' }, 8)) {
        return &GUID_ContainerFormatWebp;
    }
    if (starts_with({ 0x00, 0x00, 0x01, 0x00 })) {
        return &GUID_ContainerFormatIco;
    }
    return nullptr;
}

// bytes をコピーせずに参照して container 形式の画像をデコードし、pixel_format の FormatConverter と
// ピクセルサイズを返す。bytes は戻り値を使い終えるまで生かすこと (converter の lifetime 注意は
// ConvertBitmapSource を参照)。形式の判定を WIC に任せるとインストール済みのどのデコーダも
// 動きうるので、container の形式に限る。
inline std::optional<DecodeResult> DecodeFromMemory(
    IWICImagingFactory* wic, std::span<const uint8_t> bytes, const GUID& container,
    const WICPixelFormatGUID& pixel_format = GUID_WICPixelFormat32bppPBGRA)
{
    const auto stream = CreateStreamOnMemory(wic, bytes);
    if (!stream) {
        return std::nullopt;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = wic->CreateDecoder(container, nullptr, &decoder);
    if (SUCCEEDED(hr)) {
        hr = decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand);
    }
    if (FAILED(hr) || FAILED(decoder->GetFrame(0, &frame))) {
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

// 文書から指定された画像ファイル。SniffImageContainer で選んだ形式のデコーダだけを使う。
inline std::optional<DecodeResult> DecodeImageFileFromMemory(IWICImagingFactory* wic, std::span<const uint8_t> bytes)
{
    const GUID* container = SniffImageContainer(bytes);
    if (!container) {
        return std::nullopt;
    }
    return DecodeFromMemory(wic, bytes, *container);
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
