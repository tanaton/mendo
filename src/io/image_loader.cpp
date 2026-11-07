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

void ImageLoader::Init(IWICImagingFactory* wic, HWND hwnd, UINT msg_id, TaskScheduler& scheduler)
{
    wic_factory_ = wic;
    hwnd_ = hwnd;
    msg_id_ = msg_id;
    scheduler_ = &scheduler;
}

void ImageLoader::SetRenderTarget(ID2D1RenderTarget* rt)
{
    // 寸法はデバイスに依らないので残す。
    if (render_target_) {
        delivered_.Clear();
        ResetFailedPaths();
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

uint64_t ImageLoader::PathKey(const std::wstring& path) noexcept
{
    return Fnv1a64({ reinterpret_cast<const char*>(path.data()), path.size() * sizeof(wchar_t) });
}

const ImageLoader::ImageInfo* ImageLoader::FindImageInfo(const std::wstring& abs_path) const noexcept
{
    return infos_.Find(PathKey(abs_path));
}

ID2D1Bitmap* ImageLoader::FindDelivered(const std::wstring& abs_path) const noexcept
{
    const auto* bitmap = delivered_.Find(PathKey(abs_path));
    return bitmap ? bitmap->Get() : nullptr;
}

void ImageLoader::StoreResult(const std::wstring& path, const ImageInfo& info, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap)
{
    const uint64_t key = PathKey(path);
    if (infos_.size() >= kMaxImageInfos && !infos_.Find(key)) {
        infos_.Clear();
    }
    *infos_.TryEmplace(key).first = info;
    if (bitmap) {
        *delivered_.TryEmplace(key).first = std::move(bitmap);
    }
}

D2D1_SIZE_F ImageLoader::DipSize(const ImageInfo& image) const noexcept
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

    if (HasGivenUp(abs_path)) {
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
    // 描画先が無い間 (起動でデバイスを待つ間・デバイスの作り直しの再試行中) は結果を受け箱に残す。受け取ると GPU に載せられず、
    // 要求中の印も外れるので、見えている画像を同じ間に何度もデコードし直す。描画先が戻ったら呼び出し側が受け取る。
    if (!render_target_) {
        return false;
    }
    bool request_again = false;
    completed_.Drain(deadline, [&](DecodeResult& r) {
        const auto pending = pending_paths_.extract(r.path);
        if (r.dropped) {
            request_again |= pending && pending.mapped();
            return;
        }
        request_again = true;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        if (r.bitmap && SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap)) && bitmap) {
            StoreResult(r.path, { r.width, r.height, r.sharp_width_px }, std::move(bitmap));
        }
        else {
            // GPU に載せられなかった画像もデコード失敗と同じく数える。数えないと完了通知のたびに読み直し続ける。
            ++failed_paths_[r.path];
        }
    });
    return request_again;
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
