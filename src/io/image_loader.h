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


class TaskScheduler;

class ImageLoader {
public:
    using Callback = void(*)(void*);

    ~ImageLoader();

    void Init(ID2D1RenderTarget* rt, IWICImagingFactory* wic = nullptr);
    void SetRenderTarget(ID2D1RenderTarget* rt) noexcept { render_target_ = rt; }

    // TaskSchedulerを設定し、非同期読み込みを有効にする。
    void InitAsync(HWND hwnd, UINT msg_id, TaskScheduler& scheduler);

    // 画像ファイルを同期的に読み込む（テスト用に残す）。
    bool LoadImage(const std::wstring& abs_path, DiagramEntry& out);

    // キャッシュのみを参照する。I/O は行わない。
    bool GetCachedImage(const std::wstring& abs_path, DiagramEntry& out) const;

    // 非同期読み込みをキューに追加する。重複パスは無視される。
    void RequestLoadAsync(const std::wstring& abs_path,
        Callback on_complete, void* user_data);

    // UI スレッドから呼び出す: 完了した WIC デコード結果を D2D ビットマップに変換し、
    // キャッシュに格納する。バッチ内の最後の結果のコールバックのみを1回発火する
    // （呼び出し側が全ノードを再スキャンする設計のため）。
    void ProcessCompletedDecodes();

    // 保留中のリクエストと完了結果をすべて破棄する。
    void CancelPending();

    void ClearCache() noexcept { cache_.Clear(); }
    size_t CacheSize() const noexcept { return cache_.Size(); }

    // テスト用: ビットマップなしのダミーエントリをキャッシュに挿入する。
    void InsertCacheEntry(const std::wstring& path, float width, float height);

    // 保留中のリクエストをキャンセルする（CancelPendingと同等）。
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
        Callback on_complete = nullptr;
        void* user_data = nullptr;
        bool success = false;
    };

    void GetDpiScale(float& scale_x, float& scale_y) const;

    static constexpr size_t MAX_CACHE_ENTRIES = 128;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    LruCache<std::wstring, CachedImage> cache_{ MAX_CACHE_ENTRIES };

    // 非同期読み込み
    HWND hwnd_ = nullptr;
    UINT msg_id_ = 0;
    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> cancel_gen_{ 0 };
    std::mutex pending_mutex_;
    std::set<std::wstring> pending_paths_;

    std::mutex result_mutex_;
    std::vector<DecodeResult> completed_;
};
