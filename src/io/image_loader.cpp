#include "image_loader.h"
#include "file_io.h"
#include "file_loader.h"
#include "log_hr.h"
#include "stream_util.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "wic_util.h"
#include "win_handle.h"
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

static Microsoft::WRL::ComPtr<IStream> ReadFileToStream(const std::wstring& path)
{
    auto r = OpenFileForReadShared(
        std::filesystem::path(path),
        path_util::kFileShareRWDelete, MAX_FILE_SIZE);
    if (r.error != OpenFileError::None || r.size == 0) {
        return nullptr;
    }

    return stream_util::CreateMemoryStreamFromFile(r.handle.get(), r.size);
}


ImageLoader::~ImageLoader()
{
    Shutdown();
}

void ImageLoader::GetDpiScale(float& scale_x, float& scale_y) const
{
    float dpi_x = DEFAULT_DPI, dpi_y = DEFAULT_DPI;
    if (render_target_) {
        render_target_->GetDpi(&dpi_x, &dpi_y);
    }
    scale_x = (dpi_x > 0.0f) ? (dpi_x / DEFAULT_DPI) : 1.0f;
    scale_y = (dpi_y > 0.0f) ? (dpi_y / DEFAULT_DPI) : 1.0f;
}

bool ImageLoader::Init(ID2D1RenderTarget* rt, IWICImagingFactory* wic)
{
    render_target_ = rt;
    if (wic) {
        wic_factory_ = wic;
        return true;
    }
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory_));
    if (FAILED(hr)) {
        mendo::LogHrFailure(L"ImageLoader CoCreateInstance(WIC)", hr);
        wic_factory_.Reset();
        return false;
    }
    return true;
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

bool ImageLoader::LoadImage(const std::wstring& abs_path, DiagramEntry& out)
{
    if (!wic_factory_ || !render_target_) {
        return false;
    }

    if (auto* cached_ptr = cache_.Find(abs_path)) {
        out.bitmap = cached_ptr->bitmap;
        out.width = cached_ptr->width;
        out.height = cached_ptr->height;
        return true;
    }

    // WIC でデコード（メモリストリーム経由でファイルロックを回避）
    const auto stream = ReadFileToStream(abs_path);
    if (!stream) {
        return false;
    }

    auto created = wic_util::CreateD2DBitmapFromStream(wic_factory_.Get(), render_target_, stream.Get());
    if (!created) {
        return false;
    }

    const auto [width, height] = CreateAndCacheImage(abs_path, created->bitmap, created->pixel_width, created->pixel_height);
    out.bitmap = std::move(created->bitmap);
    out.width = width;
    out.height = height;
    return true;
}

bool ImageLoader::GetCachedImage(const std::wstring& abs_path, DiagramEntry& out) const
{
    if (const auto* cached_ptr = cache_.Find(abs_path)) {
        out.bitmap = cached_ptr->bitmap;
        out.width = cached_ptr->width;
        out.height = cached_ptr->height;
        return true;
    }
    return false;
}

void ImageLoader::RequestLoadAsync(const std::wstring& abs_path, Callback on_complete)
{
    if (!scheduler_) {
        return;
    }

    {
        const std::lock_guard lock(pending_mutex_);
        if (!pending_paths_.insert(abs_path).second) {
            return;
        }
    }

    const uint32_t gen = cancel_gen_.load();
    const bool posted = scheduler_->Post([this, path = abs_path, on_complete = std::move(on_complete), gen, guard = latch_.Acquire()]() mutable {
        if (cancel_gen_.load() != gen) {
            return;
        }

        DecodeResult result;
        result.on_complete = std::move(on_complete);

        if (wic_factory_) {
            const auto stream = ReadFileToStream(path);
            if (stream) {
                if (const auto decoded = wic_util::DecodeFromStream(wic_factory_.Get(), stream.Get())) {
                    result.converter = decoded->converter;
                    result.width = static_cast<float>(decoded->pixel_width);
                    result.height = static_cast<float>(decoded->pixel_height);
                    result.success = true;
                }
            }
        }
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
        for (auto& r : results) {
            pending_paths_.erase(r.path);
        }
    }

    Callback last_cb;

    for (auto& r : results) {
        if (r.success && r.converter && render_target_) {
            if (!cache_.Contains(r.path)) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                const HRESULT hr = render_target_->CreateBitmapFromWicBitmap(r.converter.Get(), &bitmap);
                if (SUCCEEDED(hr) && bitmap) {
                    CreateAndCacheImage(r.path, std::move(bitmap), static_cast<UINT>(r.width), static_cast<UINT>(r.height));
                }
            }
        }

        last_cb = std::move(r.on_complete);
    }

    if (last_cb) {
        last_cb();
    }
}

void ImageLoader::InsertCacheEntry(const std::wstring& path, float width, float height)
{
    CachedImage cached;
    cached.width = width;
    cached.height = height;
    cache_.Insert(path, std::move(cached));
}

std::pair<float, float> ImageLoader::CreateAndCacheImage(
    const std::wstring& path, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap,
    UINT pixel_width, UINT pixel_height)
{
    float scale_x, scale_y;
    GetDpiScale(scale_x, scale_y);

    CachedImage cached;
    cached.bitmap = std::move(bitmap);
    cached.width = static_cast<float>(pixel_width) / scale_x;
    cached.height = static_cast<float>(pixel_height) / scale_y;

    const float w = cached.width;
    const float h = cached.height;
    cache_.Insert(path, std::move(cached));
    return { w, h };
}

void ImageLoader::CancelPending()
{
    cancel_gen_.fetch_add(1);
    {
        const std::lock_guard lock(pending_mutex_);
        pending_paths_.clear();
    }
    {
        const std::lock_guard lock(result_mutex_);
        completed_.clear();
    }
}
