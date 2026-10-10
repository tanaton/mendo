#include "image_loader.h"
#include "d2d_util.h"
#include "file_io.h"
#include "file_loader.h"
#include "stream_util.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "wic_util.h"

namespace {

Microsoft::WRL::ComPtr<IStream> ReadFileToStream(const std::wstring& path)
{
    auto r = OpenFileForReadShared(std::filesystem::path(path), path_util::kFileShareRWDelete, MAX_FILE_SIZE);
    if (r.error != OpenFileError::None || r.size == 0) {
        return nullptr;
    }
    return stream_util::CreateMemoryStreamFromFile(r.handle.get(), r.size);
}

// 画像は原寸 (1 px = 1 物理 px) より大きく描かず、ペイン幅はモニタ幅を超えないため、
// 最も広いモニタの幅が表示に要る最大解像度になる。
UINT MaxMonitorWidthPx() noexcept
{
    UINT max_width = 0;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
        if (GetMonitorInfoW(mon, &mi)) {
            auto& w = *reinterpret_cast<UINT*>(lp);
            w = std::max(w, static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&max_width));
    return max_width;
}

} // namespace

ImageLoader::~ImageLoader()
{
    Shutdown();
}

bool ImageLoader::Init(ID2D1RenderTarget* rt, IWICImagingFactory* wic)
{
    SetRenderTarget(rt);
    if (wic) {
        wic_factory_ = wic;
        return true;
    }
    wic_factory_ = wic_util::CreateWicFactory(L"ImageLoader CoCreateInstance(WIC)");
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

bool ImageLoader::GetCachedImage(const std::wstring& abs_path, DiagramEntry& out)
{
    const auto* cached = cache_.Find(abs_path);
    if (!cached) {
        return false;
    }
    out.bitmap = cached->bitmap;
    out.width = cached->width;
    out.height = cached->height;
    return true;
}

void ImageLoader::RequestLoadAsync(const std::wstring& abs_path, Callback on_complete)
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
    const bool posted = scheduler_->Post([this, path = abs_path, on_complete = std::move(on_complete), gen, guard = latch_.Acquire()]() mutable {
        if (cancel_gen_.load() != gen) {
            return;
        }

        DecodeResult result{ .on_complete = std::move(on_complete) };
        DecodeForDisplay(path, result);
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

void ImageLoader::DecodeForDisplay(const std::wstring& path, DecodeResult& result) const
{
    if (!wic_factory_) {
        return;
    }
    const auto stream = ReadFileToStream(path);
    if (!stream) {
        return;
    }
    const auto decoded = wic_util::DecodeFromStream(wic_factory_.Get(), stream.Get());
    if (!decoded) {
        return;
    }
    const auto target = wic_util::ComputeDecodeSize(decoded->pixel_width, decoded->pixel_height, MaxMonitorWidthPx(), max_bitmap_dim_.load());
    result.bitmap = wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, target);
    result.width = static_cast<float>(decoded->pixel_width);
    result.height = static_cast<float>(decoded->pixel_height);
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
        if (r.bitmap && render_target_ && !cache_.Contains(r.path)) {
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            const HRESULT hr = render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap);
            if (SUCCEEDED(hr) && bitmap) {
                CacheBitmap(r.path, std::move(bitmap), r.width, r.height);
            }
        }
        // 全 on_complete は同一の invalidate シグナルなので最後の 1 つだけ呼ぶ。
        last_cb = std::move(r.on_complete);
    }

    if (last_cb) {
        last_cb();
    }
}

void ImageLoader::InsertCacheEntry(const std::wstring& path, float width, float height)
{
    cache_.Insert(path, CachedImage{ .width = width, .height = height });
}

void ImageLoader::CacheBitmap(
    const std::wstring& path, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap,
    float pixel_width, float pixel_height)
{
    float dpi_x = DEFAULT_DPI;
    float dpi_y = DEFAULT_DPI;
    if (render_target_) {
        render_target_->GetDpi(&dpi_x, &dpi_y);
    }
    const float width = pixel_width / DpiScaleFrom(dpi_x);
    const float height = pixel_height / DpiScaleFrom(dpi_y);

    cache_.Insert(path, CachedImage{ std::move(bitmap), width, height });
    cache_.TrimToBudget(MAX_CACHE_BYTES, [](const CachedImage& c) { return mendo::BitmapBytes(c.bitmap.Get()); });
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
