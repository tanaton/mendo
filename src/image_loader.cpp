#include "image_loader.h"

void ImageLoader::Init(ID2D1RenderTarget* rt)
{
    render_target_ = rt;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory_));
}

bool ImageLoader::LoadImage(const std::wstring& abs_path, DiagramEntry& out)
{
    if (!wic_factory_ || !render_target_) {
        return false;
    }

    // キャッシュ確認
    auto it = cache_.find(abs_path);
    if (it != cache_.end()) {
        out.bitmap = it->second.bitmap;
        out.width = it->second.width;
        out.height = it->second.height;
        return true;
    }

    // WIC でデコード
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_factory_->CreateDecoderFromFilename(
        abs_path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = wic_factory_->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return false;
    }

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<ID2D1Bitmap> bitmap;
    hr = render_target_->CreateBitmapFromWicBitmap(converter.Get(), &bitmap);
    if (FAILED(hr)) {
        return false;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);

    // キャッシュに格納
    CachedImage cached;
    cached.bitmap = bitmap;
    cached.width = static_cast<float>(w);
    cached.height = static_cast<float>(h);
    cache_[abs_path] = cached;

    out.bitmap = bitmap;
    out.width = cached.width;
    out.height = cached.height;
    return true;
}
