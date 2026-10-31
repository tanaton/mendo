#include "image_loader.h"
#include "d2d_util.h"
#include "file_io.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "wic_util.h"

ImageLoader::ImageLoader() noexcept
    : max_bitmap_dim_(MaxBitmapDim(nullptr))
{
}

ImageLoader::~ImageLoader()
{
    Shutdown();
}

bool ImageLoader::Init(IWICImagingFactory* wic, HWND hwnd, UINT msg_id, TaskScheduler& scheduler)
{
    wic_factory_ = wic;
    hwnd_ = hwnd;
    msg_id_ = msg_id;
    scheduler_ = &scheduler;
    return wic_factory_ != nullptr;
}

void ImageLoader::SetRenderTarget(ID2D1RenderTarget* rt)
{
    if (render_target_) {
        CancelPending();
        ClearCache();
    }
    render_target_ = rt;
    max_bitmap_dim_.store(MaxBitmapDim(rt));
}

void ImageLoader::Shutdown()
{
    CancelPending();
    latch_.Wait();
    completed_.TakeAll();
    pending_paths_.clear();
}

const ImageLoader::CachedImage* ImageLoader::FindCachedImage(const std::wstring& abs_path)
{
    return cache_.Find(abs_path);
}

D2D1_SIZE_F ImageLoader::DipSize(const CachedImage& image) const noexcept
{
    const float scale = DpiScale();
    return { PixelToDip(image.pixel_width, scale), PixelToDip(image.pixel_height, scale) };
}

float ImageLoader::DpiScale() const noexcept
{
    float dpi_x = DEFAULT_DPI;
    float dpi_y = DEFAULT_DPI;
    // 描画デバイスの作成前に来た要求も、表示する解像度でデコードするためウィンドウから取る。
    if (render_target_) {
        render_target_->GetDpi(&dpi_x, &dpi_y);
    }
    else if (const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 0) {
        dpi_x = static_cast<float>(dpi);
    }
    return DpiScaleFrom(dpi_x);
}

UINT ImageLoader::DecodeWidthPx(float width_dip) const noexcept
{
    if (!(width_dip > 0.0f)) {
        return 0;
    }
    constexpr UINT kQuantumPx = 256;
    const auto px = static_cast<UINT>(DipToPixelCeil(width_dip, DpiScale()));
    return (px + kQuantumPx - 1) / kQuantumPx * kQuantumPx;
}

void ImageLoader::RequestLoadAsync(const std::wstring& abs_path, float max_width_dip)
{
    if (!scheduler_) {
        return;
    }

    if (auto it = failed_paths_.find(abs_path); it != failed_paths_.end() && it->second >= kMaxImageRetries) {
        return;
    }
    if (const auto [it, inserted] = pending_paths_.try_emplace(abs_path, false); !inserted) {
        it->second = true;
        return;
    }

    const UINT max_width_px = DecodeWidthPx(max_width_dip);
    scheduler_->Post([this, path = abs_path, drop_gen = drop_gen_.load(), max_width_px, guard = latch_.Acquire()]() mutable {
        DecodeResult result;
        result.dropped = drop_gen_.load() != drop_gen;
        if (!result.dropped) {
            DecodeForDisplay(path, max_width_px, result);
        }
        result.path = std::move(path);

        if (completed_.Push(std::move(result)) && hwnd_) {
            ::PostMessageW(hwnd_, msg_id_, 0, 0);
        }
    });
}

void ImageLoader::DecodeForDisplay(const std::wstring& path, UINT max_width_px, DecodeResult& result) const
{
    if (!wic_factory_) {
        return;
    }
    // bytes はデコードしたビットマップを作り終えるまで生かす (ストリームはコピーせずに参照する)。
    const auto bytes = ReadAllBytes(path.c_str(), nullptr, path_util::kFileShareRWDelete, MAX_IMAGE_FILE_SIZE);
    TaskScheduler::EnsureWorkerCom();
    const auto decoded = wic_util::DecodeImageFileFromMemory(wic_factory_.Get(), bytes);
    if (!decoded) {
        return;
    }
    const auto target = wic_util::ComputeDecodeSize(decoded->pixel_width, decoded->pixel_height, max_width_px, max_bitmap_dim_.load());
    result.bitmap = wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, target);
    result.width = static_cast<float>(decoded->pixel_width);
    result.height = static_cast<float>(decoded->pixel_height);
    // GPU の最大辺で縮んだ場合は幅を広げて読み直しても変わらないので上限なし扱いにする。
    const bool width_limited = target.width < decoded->pixel_width && target.width == max_width_px;
    result.sharp_width_px = width_limited ? target.width : UINT_MAX;
}

bool ImageLoader::ProcessCompletedDecodes(std::chrono::steady_clock::time_point deadline)
{
    const auto results = completed_.Drain(deadline, [&](DecodeResult& r) {
        if (r.dropped) {
            return;
        }
        // GPU に載せられなかった画像もデコード失敗と同じく数える。数えないと完了通知のたびに読み直し続ける。
        bool failed = !r.bitmap;
        const auto* existing = cache_.Find(r.path);
        if (r.bitmap && render_target_ && (!existing || existing->sharp_width_px < r.sharp_width_px)) {
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            const HRESULT hr = render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap);
            if (SUCCEEDED(hr) && bitmap) {
                CacheBitmap(r.path, std::move(bitmap), r.width, r.height, r.sharp_width_px);
            }
            else {
                failed = true;
            }
        }
        if (failed) {
            r.bitmap.Reset();
        }
    });
    bool request_again = false;
    for (const auto& r : results) {
        const auto pending = pending_paths_.extract(r.path);
        if (r.dropped) {
            request_again |= pending && pending.mapped();
            continue;
        }
        request_again = true;
        if (!r.bitmap) {
            ++failed_paths_[r.path];
        }
    }
    return request_again;
}

void ImageLoader::CacheBitmap(
    const std::wstring& path, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap,
    float pixel_width, float pixel_height, UINT sharp_width_px)
{
    cache_.Insert(path, CachedImage{ std::move(bitmap), pixel_width, pixel_height, sharp_width_px });
    cache_.TrimToBudget(MAX_CACHE_BYTES, [](const CachedImage& c) static noexcept { return BitmapBytes(c.bitmap.Get()); });
}

void ImageLoader::DropQueued()
{
    drop_gen_.fetch_add(1);
}

void ImageLoader::CancelPending()
{
    DropQueued();
    ResetFailedPaths();
    for (const auto& r : completed_.TakeAll()) {
        pending_paths_.erase(r.path);
    }
}
