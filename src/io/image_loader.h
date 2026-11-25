#pragma once
#include "utility.h"
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
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

class TaskScheduler;

// 拡大表示のために読んだ画像 (ImageLoader::DecodeFullSize)。
struct FullSizeImage {
    // 32bpp PBGRA。読めなければ null。
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    // 元の画像の画素数。bitmap は上限を超える画像だけ縮めてある。
    UINT pixel_width = 0;
    UINT pixel_height = 0;
};

// ImageLoader::ProcessCompletedDecodes で GPU に載せたビットマップ。TakeDelivered で受け取った側が描く側へ渡す。
class DeliveredImages {
public:
    ID2D1Bitmap* Find(const std::wstring& abs_path) const noexcept;

private:
    friend class ImageLoader;
    // キーはパスのハッシュ。
    FlatHashMap<Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;
};

class ImageLoader {
public:
    ImageLoader() noexcept;
    ~ImageLoader();

    // デコード結果が溜まり始めたら worker で wake を呼ぶ (空なら呼ばない)。UI スレッドに ProcessCompletedDecodes を呼ばせるだけにする。
    // hwnd は描画先より先に来た要求を、窓の DPI でデコードするのに使う。
    void Init(IWICImagingFactory* wic, HWND hwnd, TaskScheduler& scheduler, WorkerWake wake);
    // **UI スレッドからのみ呼び出すこと** (worker が読むのは max_bitmap_dim_ だけ)。
    // デバイスロストでの差し替えでは、旧 rt のビットマップを捨てる。デコードは WIC ビットマップを作るので rt に依らず
    // (縮める上限 max_bitmap_dim_ は始めるときに読む)、待ちのものも受け箱の結果も新しい rt に載せる。
    void SetRenderTarget(ID2D1RenderTarget* rt);
    TaskScheduler* scheduler() const noexcept
    {
        return scheduler_;
    }

    // 読み込んだ画像の寸法。ビットマップはここに残さず、描く側 (InlineDrawings) が保持範囲の画像の分だけ持ち、外れた画像は
    // 戻ったときに読み直す。ビットマップを残すキャッシュは、メモリが大きく増えるので無くした。
    struct ImageInfo {
        float pixel_width = 0.0f;
        float pixel_height = 0.0f;
        UINT sharp_width_px = UINT_MAX;
        bool stale = false;
    };
    // 次に寸法の表を変える (ProcessCompletedDecodes) まで有効。
    const ImageInfo* FindImageInfo(const std::wstring& abs_path) const noexcept;
    size_t ImageInfoCount() const noexcept
    {
        return infos_.size();
    }
    // 寸法の表の件数の上限。1 枠 32 バイト (4096 件で 260KB ほど) なので大きく取り、超えたら空にして数え直す。
    static constexpr size_t kMaxImageInfos = 4096;
    // 受け取ったビットマップを渡し、ImageLoader には残さない。
    DeliveredImages TakeDelivered() noexcept
    {
        return std::exchange(delivered_, {});
    }
    // 今の DPI での表示寸法 (画像の 1 px を物理 1 px で描く)。
    D2D1_SIZE_F DipSize(const ImageInfo& image) const noexcept;
    void RequestLoadAsync(const std::wstring& abs_path, float max_width_dip);
    void RequestSizeAsync(const std::wstring& abs_path);
    bool HasGivenUp(const std::wstring& abs_path) const noexcept
    {
        const auto it = failed_paths_.find(abs_path);
        return it != failed_paths_.end() && it->second >= kMaxImageRetries;
    }
    // 表示幅が広がり、最後に読んだ解像度では足りなくなったか。
    bool NeedsSharperDecode(const ImageInfo& image, float max_width_dip) const noexcept
    {
        return DecodeWidthPx(max_width_dip) > image.sharp_width_px;
    }
    bool ProcessCompletedDecodes(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kImportSliceBudget);
    // 前に呼んでから RequestLoadAsync で頼み直されなかったビットマップの要求 (範囲を出た画像) を取り消す。取り込みのたびに、
    // まだ要るものを頼み直させてから呼ぶ。速いスクロールで溜まった要求を読み続けると、止まった位置の画像が後回しになるため。
    void CancelStaleBitmapRequests() noexcept;
    bool HasLeftoverUploads() const noexcept
    {
        return render_target_ && completed_.HasLeftover();
    }
    void DropQueued();
    void CancelPending();

    void ResetFailedPaths()
    {
        failed_paths_.clear();
    }
    void Invalidate(const std::wstring& abs_path);

    void Shutdown();

    // 拡大表示の画像を原寸で読む。一辺が max_dim (GPU の最大) を、画素数が max_pixels を超える画像だけ縮める。worker で呼ぶ。
    static FullSizeImage DecodeFullSize(IWICImagingFactory* wic, const std::wstring& path, UINT max_dim, uint64_t max_pixels);

    // 寸法の表と受け取ったビットマップを空にする。テスト専用。
    void ClearImagesForTest() noexcept
    {
        infos_.Clear();
        delivered_ = {};
    }
    // 読み込んだ結果として寸法 (と bitmap があれば受け取ったビットマップ) を入れる。テスト専用。
    void InsertImageForTest(const std::wstring& path, float pixel_width, float pixel_height, ID2D1Bitmap* bitmap = nullptr)
    {
        StoreResult(path, { .pixel_width = pixel_width, .pixel_height = pixel_height }, bitmap);
    }

private:
    // 始まる前に捨てたか取り消したので、読まずに返した。
    struct Skipped {};
    // 寸法だけを読んだ。読めなければ pixel_width が 0。
    struct SizeRead {
        ImageInfo info;
    };
    // デコードし、縮小まで済ませた。読めなければ bitmap が null。
    struct Decoded {
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        ImageInfo info;
    };
    struct DecodeResult {
        std::wstring path;
        std::variant<Skipped, SizeRead, Decoded> outcome;
    };

    void StoreResult(const std::wstring& path, const ImageInfo& info, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap);
    float DpiScale() const noexcept;
    // 0 は無制限。幅の細かな変化で再デコードしないよう量子化する。
    UINT DecodeWidthPx(float width_dip) const noexcept;
    void QueueDecode(const std::wstring& abs_path, UINT max_width_px, bool size_only);
    SizeRead ReadSize(const std::wstring& path) const;
    Decoded DecodeForDisplay(const std::wstring& path, UINT max_width_px) const;

    static constexpr uint32_t kMaxImageRetries = 3;
    // 圧縮済み画像のファイルサイズ上限。デコードはワーカー数ぶん並行するので、Markdown と同じ 2GB では大きすぎる。
    static constexpr LONGLONG MAX_IMAGE_FILE_SIZE = 256LL * 1024 * 1024;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    std::atomic<UINT> max_bitmap_dim_;
    FlatHashMap<ImageInfo> infos_;
    DeliveredImages delivered_;

    HWND hwnd_ = nullptr;
    WorkerWake wake_;
    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> drop_gen_{ 0 };
    struct ReadingSize {};
    struct ReadingBitmap {
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
        bool renewed = true;
        bool size_wanted = false;
    };
    struct PendingDecode {
        std::variant<ReadingSize, ReadingBitmap> reading;
        bool requested_again = false;
        bool invalidated = false;
    };
    std::unordered_map<std::wstring, PendingDecode, StringTransparentHash, std::equal_to<>> pending_paths_;
    std::unordered_map<std::wstring, uint32_t, StringTransparentHash, std::equal_to<>> failed_paths_;

    PostedInbox<DecodeResult> completed_;
    WorkerLatch latch_;
};
