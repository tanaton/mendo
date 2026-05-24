#pragma once
#include "layout_cache.h"
#include "lru_cache.h"
#include "worker_latch.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <string>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>


class TaskScheduler;

class ImageLoader {
public:
    using Callback = std::move_only_function<void()>;

    ~ImageLoader();

    // 失敗時 false; 以降の LoadImage / RequestLoadAsync は no-op。
    bool Init(ID2D1RenderTarget* rt, IWICImagingFactory* wic = nullptr);
    // **UI スレッドからのみ呼び出すこと**。worker は render_target_ を非アトミック参照する
    // (ProcessCompletedDecodes 経由)。デバイスロスト後の rt 差し替えは CancelPending() で
    // worker を止めてから呼ぶ規約 (App::Init の DeviceLost コールバック参照)。
    void SetRenderTarget(ID2D1RenderTarget* rt) noexcept
    {
        render_target_ = rt;
    }

    void InitAsync(HWND hwnd, UINT msg_id, TaskScheduler& scheduler);
    bool LoadImage(const std::wstring& abs_path, DiagramEntry& out);
    bool GetCachedImage(const std::wstring& abs_path, DiagramEntry& out) const;
    void RequestLoadAsync(const std::wstring& abs_path, Callback on_complete);
    void ProcessCompletedDecodes();
    void CancelPending();

    void ClearCache() noexcept
    {
        cache_.Clear();
    }
    size_t CacheSize() const noexcept
    {
        return cache_.Size();
    }

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
    std::pair<float, float> CreateAndCacheImage(const std::wstring& path, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap, UINT pixel_width, UINT pixel_height);

    static constexpr size_t MAX_CACHE_ENTRIES = 128;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    LruCache<std::wstring, CachedImage, MAX_CACHE_ENTRIES> cache_;

    HWND hwnd_ = nullptr;
    UINT msg_id_ = 0;
    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> cancel_gen_{ 0 };
    std::mutex pending_mutex_;
    std::unordered_set<std::wstring> pending_paths_;

    std::mutex result_mutex_;
    std::vector<DecodeResult> completed_;

    // Shutdown で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
