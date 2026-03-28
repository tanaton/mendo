#pragma once
#include "layout_cache.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

class ImageLoader {
public:
    void Init(ID2D1RenderTarget* rt);
    void SetRenderTarget(ID2D1RenderTarget* rt) noexcept { render_target_ = rt; }

    // 画像ファイルを読み込み、DiagramEntry にビットマップと元サイズを格納する。
    // 成功時 true、失敗時 false を返す。
    bool LoadImage(const std::wstring& abs_path, DiagramEntry& out);

    void ClearCache() noexcept { cache_.clear(); }

private:
    struct CachedImage {
        ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };

    ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    std::unordered_map<std::wstring, CachedImage> cache_;
};
