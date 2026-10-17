#include "image_loader.h"
#include "d2d_util.h"
#include "file_io.h"
#include "file_loader.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "wic_util.h"
#include <cmath>

ImageLoader::~ImageLoader()
{
    Shutdown();
}

bool ImageLoader::Init(IWICImagingFactory* wic)
{
    if (wic) {
        wic_factory_ = wic;
        return true;
    }
    wic_factory_ = wic_util::CreateWicFactory("ImageLoader CoCreateInstance(WIC)");
    return wic_factory_ != nullptr;
}

void ImageLoader::InitAsync(HWND hwnd, UINT msg_id, TaskScheduler& scheduler)
{
    hwnd_ = hwnd;
    msg_id_ = msg_id;
    scheduler_ = &scheduler;
}

void ImageLoader::Shutdown()
{
    CancelPending();
    latch_.Wait();
}

const ImageLoader::CachedImage* ImageLoader::FindCachedImage(const std::wstring& abs_path)
{
    return cache_.Find(abs_path);
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
    const auto px = static_cast<UINT>(std::ceil(width_dip * DpiScale()));
    return (px + kQuantumPx - 1) / kQuantumPx * kQuantumPx;
}

void ImageLoader::RequestLoadAsync(const std::wstring& abs_path, Callback on_complete, float max_width_dip)
{
    if (!scheduler_) {
        return;
    }

    {
        const std::lock_guard lock(pending_mutex_);
        if (auto it = failed_paths_.find(abs_path); it != failed_paths_.end() && it->second >= kMaxImageRetries) {
            return;
        }
        if (!pending_paths_.insert(abs_path).second) {
            return;
        }
    }

    const uint32_t gen = cancel_gen_.load();
    const UINT max_width_px = DecodeWidthPx(max_width_dip);
    const bool posted = scheduler_->Post([this, path = abs_path, on_complete = std::move(on_complete), gen, max_width_px, guard = latch_.Acquire()]() mutable {
        if (cancel_gen_.load() != gen) {
            return;
        }

        DecodeResult result{ .on_complete = std::move(on_complete) };
        DecodeForDisplay(path, max_width_px, result);
        result.path = std::move(path);

        if (cancel_gen_.load() != gen) {
            return;
        }

        {
            const std::lock_guard lock(result_mutex_);
            completed_.emplace_back(std::move(result));
        }

        if (hwnd_) {
            ::PostMessageW(hwnd_, msg_id_, 0, 0);
        }
    });
    if (!posted) {
        // lambda が走らないので latch::Guard は capture 内で destruct され自動 Release。
        // ProcessCompletedDecodes 経由の pending_paths_ クリアも行われないので巻き戻す。
        const std::lock_guard lock(pending_mutex_);
        pending_paths_.erase(abs_path);
    }
}

void ImageLoader::DecodeForDisplay(const std::wstring& path, UINT max_width_px, DecodeResult& result) const
{
    if (!wic_factory_) {
        return;
    }
    // bytes はデコードしたビットマップを作り終えるまで生かす (ストリームはコピーせずに参照する)。
    const auto bytes = ReadAllBytes(std::filesystem::path(path), nullptr, path_util::kFileShareRWDelete, MAX_FILE_SIZE);
    const auto decoded = wic_util::DecodeFromMemory(wic_factory_.Get(), bytes);
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

void ImageLoader::ProcessCompletedDecodes()
{
    std::vector<DecodeResult> results;
    {
        const std::lock_guard lock(result_mutex_);
        results.swap(completed_);
    }

    if (results.empty()) {
        return;
    }

    {
        const std::lock_guard lock(pending_mutex_);
        for (const auto& r : results) {
            pending_paths_.erase(r.path);
            if (!r.bitmap) {
                ++failed_paths_[r.path];
            }
        }
    }

    Callback last_cb;
    for (auto& r : results) {
        const auto* existing = cache_.Find(r.path);
        if (r.bitmap && render_target_ && (!existing || existing->sharp_width_px < r.sharp_width_px)) {
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            const HRESULT hr = render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap);
            if (SUCCEEDED(hr) && bitmap) {
                CacheBitmap(r.path, std::move(bitmap), r.width, r.height, r.sharp_width_px);
            }
        }
        // 全 on_complete は同一の invalidate シグナルなので最後の 1 つだけ呼ぶ。
        last_cb = std::move(r.on_complete);
    }

    if (last_cb) {
        last_cb();
    }
}

void ImageLoader::CacheBitmap(
    const std::wstring& path, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap,
    float pixel_width, float pixel_height, UINT sharp_width_px)
{
    const float scale = DpiScale();
    const float width = pixel_width / scale;
    const float height = pixel_height / scale;

    cache_.Insert(path, CachedImage{ std::move(bitmap), width, height, sharp_width_px });
    cache_.TrimToBudget(MAX_CACHE_BYTES, [](const CachedImage& c) { return BitmapBytes(c.bitmap.Get()); });
}

void ImageLoader::CancelPending()
{
    cancel_gen_.fetch_add(1);
    {
        const std::lock_guard lock(pending_mutex_);
        pending_paths_.clear();
        failed_paths_.clear();
    }
    {
        const std::lock_guard lock(result_mutex_);
        completed_.clear();
    }
}
