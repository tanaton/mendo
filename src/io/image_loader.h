#pragma once
#include "d2d_util.h"
#include "utility.h"
#include "lru_cache.h"
#include "worker_latch.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class TaskScheduler;

class ImageLoader {
public:
    using Callback = std::move_only_function<void()>;

    ~ImageLoader();

    // 失敗時 false; 以降の RequestLoadAsync はデコードせず失敗扱いになる。
    bool Init(IWICImagingFactory* wic = nullptr);
    // **UI スレッドからのみ呼び出すこと** (worker が読むのは max_bitmap_dim_ だけ)。デバイスロスト後の
    // rt 差し替えは CancelPending() で旧 rt 向けのデコードを捨ててから呼ぶ規約 (App::Init の DeviceLost コールバック参照)。
    void SetRenderTarget(ID2D1RenderTarget* rt) noexcept
    {
        render_target_ = rt;
        max_bitmap_dim_.store(MaxBitmapDim(rt));
    }

    void InitAsync(HWND hwnd, UINT msg_id, TaskScheduler& scheduler);

    struct CachedImage {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
        // この幅 (物理 px) までは縮小によるぼけ無しで描ける。原寸でデコードしていれば上限なし。
        UINT sharp_width_px = UINT_MAX;
    };
    // ヒットしたら LRU を更新する。次の挿入・Clear まで有効。
    const CachedImage* FindCachedImage(const std::wstring& abs_path);
    // max_width_dip: 表示幅。これを量子化した物理 px まで縮小してデコードする。0 は原寸。
    void RequestLoadAsync(const std::wstring& abs_path, Callback on_complete, float max_width_dip = 0.0f);
    // 表示幅が広がり、キャッシュの解像度では足りなくなったか。
    bool NeedsSharperDecode(const CachedImage& image, float max_width_dip) const noexcept
    {
        return DecodeWidthPx(max_width_dip) > image.sharp_width_px;
    }
    // deadline を過ぎた分は残し、HasLeftoverUploads が真になる。
    void ProcessCompletedDecodes(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverUploads() const noexcept
    {
        return completed_.HasLeftover();
    }
    // 始まっていないデコードを捨てる。始まったものの結果はキャッシュに入れる (パスがキーなので文書をまたいでも使える)。
    // 始まったものは要求中のまま残し、結果が届く前に同じパスを要求し直しても二重に読まない。
    void DropQueued();
    // 始まったものも含めて結果を捨てる。
    void CancelPending();

    void ClearCache()
    {
        cache_.Clear();
        ResetFailedPaths();
    }
    void ResetFailedPaths()
    {
        const std::lock_guard lock(pending_mutex_);
        failed_paths_.clear();
    }
    size_t CacheSize() const noexcept
    {
        return cache_.Size();
    }

    void Shutdown();

#ifdef MENDO_TESTING
    // bitmap を持たない寸法だけの項目を入れる。キャッシュ動作のテスト専用。
    void InsertCacheEntryForTest(const std::wstring& path, float width, float height)
    {
        cache_.Insert(path, CachedImage{ .width = width, .height = height });
    }
#endif

private:
    struct DecodeResult {
        std::wstring path;
        // worker でデコード確定・縮小済み (失敗時は null)。width/height は縮小前の原寸 (レイアウトは原寸基準)。
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
        UINT sharp_width_px = UINT_MAX;
        Callback on_complete;
    };

    float DpiScale() const noexcept;
    // 0 は無制限。幅の細かな変化で再デコードしないよう量子化する。
    UINT DecodeWidthPx(float width_dip) const noexcept;
    // worker で呼ぶ。失敗時は result.bitmap を null のまま残す。
    void DecodeForDisplay(const std::wstring& path, UINT max_width_px, DecodeResult& result) const;
    // 物理 px を DIP に換算してキャッシュする。
    void CacheBitmap(const std::wstring& path, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap, float pixel_width, float pixel_height, UINT sharp_width_px);

    static constexpr size_t MAX_CACHE_ENTRIES = 128;
    static constexpr size_t MAX_CACHE_BYTES = 128u * 1024 * 1024;
    static constexpr uint32_t kMaxImageRetries = 3;
    // 圧縮済み画像のファイルサイズ上限。デコードはワーカー数ぶん並行するので、Markdown と同じ 2GB だと
    // 同時に数 GB を読み込ませられる。
    static constexpr LONGLONG MAX_IMAGE_FILE_SIZE = 256LL * 1024 * 1024;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    // worker が縮小サイズ計算に使う。render_target_ は worker から触れないので値で持つ。
    std::atomic<UINT> max_bitmap_dim_{ MaxBitmapDim(nullptr) };
    LruCache<std::wstring, CachedImage, MAX_CACHE_ENTRIES> cache_;

    HWND hwnd_ = nullptr;
    UINT msg_id_ = 0;
    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> cancel_gen_{ 0 };
    std::mutex pending_mutex_;
    // pending_mutex_ で守る。DropQueued と worker のデコード開始を排他にし、開始済みのものを捨てない。
    uint32_t drop_gen_ = 0;
    // 要求中のパス -> デコードを始めたか。
    std::map<std::wstring, bool> pending_paths_;
    std::map<std::wstring, uint32_t> failed_paths_;

    PostedInbox<DecodeResult> completed_;

    // Shutdown で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
