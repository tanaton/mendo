#pragma once
#include "utility.h"
#include "transposition_cache.h"
#include "worker_latch.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

class TaskScheduler;

class ImageLoader {
public:
    ImageLoader() noexcept;
    ~ImageLoader();

    // デコード結果が届いたら hwnd へ msg_id を投げる (hwnd が null なら投げない)。
    // wic が null なら false を返し、以降の RequestLoadAsync はデコードせず失敗扱いになる。
    bool Init(IWICImagingFactory* wic, HWND hwnd, UINT msg_id, TaskScheduler& scheduler);
    // **UI スレッドからのみ呼び出すこと** (worker が読むのは max_bitmap_dim_ だけ)。
    // デバイスロストでの差し替えでは、旧 rt のビットマップと始まっていないデコードを捨てる。
    // 始まったデコードは WIC ビットマップで rt に依らないので、新しい rt で使う。
    void SetRenderTarget(ID2D1RenderTarget* rt);

    struct CachedImage {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        // 縮小前の原寸 (物理 px)。表示の DIP は DPI で変わるので DipSize で求める。
        float pixel_width = 0.0f;
        float pixel_height = 0.0f;
        // この幅 (物理 px) までは縮小によるぼけ無しで描ける。原寸でデコードしていれば上限なし。
        UINT sharp_width_px = UINT_MAX;
    };
    // ヒットしたら順位を上げる。次の挿入・Clear まで有効。
    const CachedImage* FindCachedImage(const std::wstring& abs_path);
    // 今の DPI での表示寸法 (画像の 1 px を物理 1 px で描く)。
    D2D1_SIZE_F DipSize(const CachedImage& image) const noexcept;
    // max_width_dip: 表示幅。これを量子化した物理 px まで縮小してデコードする。0 は原寸。
    void RequestLoadAsync(const std::wstring& abs_path, float max_width_dip);
    // 表示幅が広がり、キャッシュの解像度では足りなくなったか。
    bool NeedsSharperDecode(const CachedImage& image, float max_width_dip) const noexcept
    {
        return DecodeWidthPx(max_width_dip) > image.sharp_width_px;
    }
    // deadline を過ぎた分は残し、HasLeftoverUploads が真になる。
    // 戻り値: 見えている画像を要求し直すべきか。結果を 1 件以上取り込んだ (失敗も含む) か、捨てた要求が待ちの間に要求し直されていた。
    bool ProcessCompletedDecodes(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    // 新しい結果は投函で知らせるので、続きが要るのは時間切れで残したときだけ。UI スレッド専用。
    bool HasLeftoverUploads() const noexcept
    {
        return completed_.HasLeftover();
    }
    // 始まっていないデコードを worker に空振りさせる。始まったものの結果はキャッシュに入れる (パスがキーなので文書をまたいでも使える)。
    // どちらも結果が届くまで要求中のまま残し、同じパスを二重に読まない。
    void DropQueued();
    // DropQueued に加え、届いている結果と失敗の記録も捨てる。
    void CancelPending();

    void ClearCache()
    {
        cache_.Clear();
        ResetFailedPaths();
    }
    void ResetFailedPaths()
    {
        failed_paths_.clear();
    }
    size_t CacheSize() const noexcept
    {
        return cache_.Size();
    }

    void Shutdown();

    // 寸法 (と任意の bitmap) だけの項目を入れる。キャッシュ動作のテスト専用。
    void InsertCacheEntryForTest(const std::wstring& path, float pixel_width, float pixel_height, ID2D1Bitmap* bitmap = nullptr)
    {
        cache_.Insert(path, CachedImage{ .bitmap = bitmap, .pixel_width = pixel_width, .pixel_height = pixel_height });
    }

private:
    struct DecodeResult {
        std::wstring path;
        // worker でデコード確定・縮小済み (失敗時は null)。width/height は縮小前の原寸 (レイアウトは原寸基準)。
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
        UINT sharp_width_px = UINT_MAX;
        // 始まる前に捨てられた。読まずに返す。
        bool dropped = false;
    };

    float DpiScale() const noexcept;
    // 0 は無制限。幅の細かな変化で再デコードしないよう量子化する。
    UINT DecodeWidthPx(float width_dip) const noexcept;
    // worker で呼ぶ。失敗時は result.bitmap を null のまま残す。
    void DecodeForDisplay(const std::wstring& path, UINT max_width_px, DecodeResult& result) const;
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
    std::atomic<UINT> max_bitmap_dim_;
    TranspositionCache<std::wstring, CachedImage, MAX_CACHE_ENTRIES> cache_;

    HWND hwnd_ = nullptr;
    UINT msg_id_ = 0;
    TaskScheduler* scheduler_ = nullptr;
    // DropQueued で進める。投入時の値から変わっていたら worker は読まずに dropped で返す (Mermaid の disk_drop_gen_ と同じ)。
    std::atomic<uint32_t> drop_gen_{ 0 };
    // 結果を待っているパス -> 待つ間に要求し直されたか。要求は必ず結果を 1 回返すので、届いたら外す。
    // 文書を切り替えて捨てた要求の結果で旧文書の画像を読み直さないよう、要求し直されたものだけ読み直させる。
    std::unordered_map<std::wstring, bool> pending_paths_;
    std::map<std::wstring, uint32_t> failed_paths_;

    PostedInbox<DecodeResult> completed_;

    // Shutdown で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};
