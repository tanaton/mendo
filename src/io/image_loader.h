#pragma once
#include "layout_cache.h"
#include "lru_cache.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <string>
#include <set>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>


class TaskScheduler;

class ImageLoader {
public:
    using Callback = std::move_only_function<void()>;

    ~ImageLoader();

    void Init(ID2D1RenderTarget* rt, IWICImagingFactory* wic = nullptr);
    void SetRenderTarget(ID2D1RenderTarget* rt) noexcept { render_target_ = rt; }

    void InitAsync(HWND hwnd, UINT msg_id, TaskScheduler& scheduler);
    bool LoadImage(const std::wstring& abs_path, DiagramEntry& out);
    bool GetCachedImage(const std::wstring& abs_path, DiagramEntry& out) const;
    void RequestLoadAsync(const std::wstring& abs_path, Callback on_complete);
    void ProcessCompletedDecodes();
    void CancelPending();

    void ClearCache() noexcept { cache_.Clear(); }
    size_t CacheSize() const noexcept { return cache_.Size(); }

    void InsertCacheEntry(const std::wstring& path, float width, float height);
    void Shutdown();

private:
    struct CachedImage {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };

    struct DecodeResult {
        std::wstring path;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        float width = 0.0f;
        float height = 0.0f;
        Callback on_complete;
        bool success = false;
    };

    void GetDpiScale(float& scale_x, float& scale_y) const;
    std::pair<float, float> CreateAndCacheImage(const std::wstring& path,
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap, UINT pixel_width, UINT pixel_height);

    static constexpr size_t MAX_CACHE_ENTRIES = 128;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    LruCache<std::wstring, CachedImage> cache_{ MAX_CACHE_ENTRIES };

    HWND hwnd_ = nullptr;
    UINT msg_id_ = 0;
    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> cancel_gen_{ 0 };
    std::mutex pending_mutex_;
    std::set<std::wstring> pending_paths_;

    std::mutex result_mutex_;
    std::vector<DecodeResult> completed_;
};
