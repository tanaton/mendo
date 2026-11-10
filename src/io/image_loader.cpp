#include "image_loader.h"
#include "d2d_util.h"
#include "file_io.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "wic_util.h"
#include <ranges>

namespace {

uint64_t PathKey(const std::wstring& path) noexcept
{
    return RapidHash(path);
}

// 寸法を読むときに先に読むファイルの先頭の長さ。JPEG・PNG・GIF・ICO の寸法はヘッダにあり、手元の写真と
// スクリーンショットは 16KB 以内で読めた。大きなメタデータ (ICC プロファイル・テキスト) が前にあるものと BMP は足りない。
constexpr size_t kImageHeadBytes = 64 * 1024;

// 寸法だけを、ファイルの先頭から読む。足りなければ全体を読む。4MB の写真 40 枚の表を開くと、寸法のために読む量が
// 161MB → 2.5MB、ワーカーの時間が 78 → 19ms (2026-11-08、ファイルは OS のキャッシュにある状態)。
std::optional<wic_util::PixelSize> ReadImageSize(IWICImagingFactory* wic, const std::wstring& path, LONGLONG max_size)
{
    const auto file = OpenFileForReadShared(path.c_str(), path_util::kFileShareRWDelete, max_size);
    if (!file || file->size == 0) {
        return std::nullopt;
    }
    ByteBuffer head(std::min(file->size, kImageHeadBytes));
    if (!ReadExact(file->handle.get(), head.data(), head.size())) {
        return std::nullopt;
    }
    if (const auto size = wic_util::ReadImageFileSizeFromMemory(wic, std::as_const(head)); size || head.size() == file->size) {
        return size;
    }
    const auto bytes = ReadAllBytes(path.c_str(), nullptr, path_util::kFileShareRWDelete, max_size);
    return wic_util::ReadImageFileSizeFromMemory(wic, bytes);
}

} // namespace

ID2D1Bitmap* DeliveredImages::Find(const std::wstring& abs_path) const noexcept
{
    // ほとんどの走査では空なので、パスのハッシュを取らずに返す。
    if (bitmaps_.size() == 0) {
        return nullptr;
    }
    const auto* bitmap = bitmaps_.Find(PathKey(abs_path));
    return bitmap ? bitmap->Get() : nullptr;
}

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
        delivered_ = {};
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

const ImageLoader::ImageInfo* ImageLoader::FindImageInfo(const std::wstring& abs_path) const noexcept
{
    return infos_.Find(PathKey(abs_path));
}

void ImageLoader::StoreResult(const std::wstring& path, const ImageInfo& info, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap)
{
    const uint64_t key = PathKey(path);
    if (infos_.size() >= kMaxImageInfos && !infos_.Find(key)) {
        infos_.Clear();
    }
    *infos_.TryEmplace(key).first = info;
    if (bitmap) {
        *delivered_.bitmaps_.TryEmplace(key).first = std::move(bitmap);
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
    QueueDecode(abs_path, DecodeWidthPx(max_width_dip), /*size_only=*/false);
}

void ImageLoader::RequestSizeAsync(const std::wstring& abs_path)
{
    QueueDecode(abs_path, 0, /*size_only=*/true);
}

void ImageLoader::QueueDecode(const std::wstring& abs_path, UINT max_width_px, bool size_only)
{
    if (!scheduler_) {
        return;
    }

    if (HasGivenUp(abs_path)) {
        return;
    }
    const auto [it, inserted] = pending_paths_.try_emplace(abs_path);
    auto& pending = it->second;
    if (!inserted) {
        pending.requested_again = true;
        if (auto* bitmap = std::get_if<ReadingBitmap>(&pending.reading)) {
            if (size_only) {
                bitmap->size_wanted = true;
            }
            else {
                bitmap->renewed = true;
                bitmap->cancelled->store(false, std::memory_order_relaxed);
            }
        }
        return;
    }
    std::shared_ptr<std::atomic<bool>> cancelled;
    if (!size_only) {
        cancelled = pending.reading.emplace<ReadingBitmap>().cancelled;
    }

    scheduler_->Post([this, path = abs_path, drop_gen = drop_gen_.load(), cancelled = std::move(cancelled), max_width_px, size_only,
                      guard = latch_.Acquire()]() mutable {
        DecodeResult result;
        if (drop_gen_.load() != drop_gen || (cancelled && cancelled->load(std::memory_order_relaxed))) {
            result.outcome = Skipped{};
        }
        else {
            TaskScheduler::EnsureWorkerCom();
            if (size_only) {
                result.outcome = ReadSize(path);
            }
            else {
                result.outcome = DecodeForDisplay(path, max_width_px);
            }
        }
        result.path = std::move(path);

        if (completed_.Push(std::move(result)) && hwnd_) {
            ::PostMessageW(hwnd_, msg_id_, 0, 0);
        }
    });
}

ImageLoader::SizeRead ImageLoader::ReadSize(const std::wstring& path) const
{
    SizeRead out;
    if (const auto size = ReadImageSize(wic_factory_.Get(), path, MAX_IMAGE_FILE_SIZE)) {
        out.info = { static_cast<float>(size->width), static_cast<float>(size->height), 0 };
    }
    return out;
}

ImageLoader::Decoded ImageLoader::DecodeForDisplay(const std::wstring& path, UINT max_width_px) const
{
    // bytes はデコードしたビットマップを作り終えるまで生かす (ストリームはコピーせずに参照する)。
    const auto bytes = ReadAllBytes(path.c_str(), nullptr, path_util::kFileShareRWDelete, MAX_IMAGE_FILE_SIZE);
    const auto decoded = wic_util::DecodeImageFileFromMemory(wic_factory_.Get(), bytes);
    if (!decoded) {
        return {};
    }
    const auto target = wic_util::ComputeDecodeSize(decoded->pixel_width, decoded->pixel_height, max_width_px, max_bitmap_dim_.load());
    // GPU の最大辺で縮んだ場合は幅を広げて読み直しても変わらないので上限なし扱いにする。
    const bool width_limited = target.width < decoded->pixel_width && target.width == max_width_px;
    return {
        .bitmap = wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, target),
        .info = { static_cast<float>(decoded->pixel_width), static_cast<float>(decoded->pixel_height), width_limited ? target.width : UINT_MAX },
    };
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
        if (std::holds_alternative<Skipped>(r.outcome)) {
            request_again |= pending && pending.mapped().requested_again;
            return;
        }
        request_again = true;
        auto* const decoded = std::get_if<Decoded>(&r.outcome);
        const ImageInfo& info = decoded ? decoded->info : std::get<SizeRead>(r.outcome).info;
        const auto* bitmap_request = pending ? std::get_if<ReadingBitmap>(&pending.mapped().reading) : nullptr;
        const bool cancelled = bitmap_request && bitmap_request->cancelled->load(std::memory_order_relaxed);
        const bool stale = pending && pending.mapped().invalidated;
        if ((!decoded || cancelled) && info.pixel_width > 0) {
            // 寸法だけの要求と、デコードの後で取り消した要求。ビットマップは載せず、前に読んだ解像度 (sharp_width_px) も消さない。
            // 更新されたファイルの寸法は入れ替える (前のビットマップは古いので、解像度も読んでいない扱いにする)。
            if (const auto* known = FindImageInfo(r.path); !known || known->stale) {
                StoreResult(r.path, { info.pixel_width, info.pixel_height, 0, stale }, nullptr);
            }
            return;
        }
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        if (decoded && decoded->bitmap && SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(decoded->bitmap.Get(), &bitmap)) && bitmap) {
            ImageInfo stored = info;
            stored.stale = stale;
            StoreResult(r.path, stored, std::move(bitmap));
            return;
        }
        // GPU に載せられなかった画像もデコード失敗と同じく数える。数えないと完了通知のたびに読み直し続ける。
        // 待つ間に更新されたファイルの失敗は、更新の前の中身のものかもしれないので数えない。
        if (!stale) {
            ++failed_paths_[r.path];
        }
    });
    return request_again;
}

void ImageLoader::Invalidate(const std::wstring& abs_path)
{
    failed_paths_.erase(abs_path);
    if (auto* const info = infos_.Find(PathKey(abs_path))) {
        info->stale = true;
    }
    if (const auto it = pending_paths_.find(abs_path); it != pending_paths_.end()) {
        it->second.invalidated = true;
    }
}

void ImageLoader::CancelStaleBitmapRequests() noexcept
{
    for (auto& pending : pending_paths_ | std::views::values) {
        auto* const bitmap = std::get_if<ReadingBitmap>(&pending.reading);
        if (!bitmap) {
            continue;
        }
        const bool size_wanted = std::exchange(bitmap->size_wanted, false);
        if (!std::exchange(bitmap->renewed, false)) {
            bitmap->cancelled->store(true, std::memory_order_relaxed);
            pending.requested_again = size_wanted;
        }
    }
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
