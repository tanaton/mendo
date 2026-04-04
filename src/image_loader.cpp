#include "image_loader.h"
#include "file_loader.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include <shlwapi.h>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

// ファイルを共有モードでメモリに読み込みIStreamとして返す。
// CreateDecoderFromFilename はファイルを排他的に開くため、
// 外部エディタ等がファイルを更新できなくなる問題を回避する。
static Microsoft::WRL::ComPtr<IStream> ReadFileToStream(const std::wstring& path)
{
    const HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0 || size.QuadPart > MAX_FILE_SIZE) {
        CloseHandle(hFile);
        return nullptr;
    }

    std::vector<BYTE> buf(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr);
    CloseHandle(hFile);

    if (!ok || bytesRead != buf.size()) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(buf.data(), static_cast<UINT>(buf.size())));
    return stream;
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

void ImageLoader::Init(ID2D1RenderTarget* rt, IWICImagingFactory* wic)
{
    render_target_ = rt;
    if (wic) {
        wic_factory_ = wic;
    }
    else {
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic_factory_)
        );
        if (FAILED(hr)) {
            wic_factory_.Reset();
        }
    }
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
}

bool ImageLoader::LoadImage(const std::wstring& abs_path, DiagramEntry& out)
{
    if (!wic_factory_ || !render_target_) {
        return false;
    }

    // キャッシュ確認
    const auto it = cache_.find(abs_path);
    if (it != cache_.end()) {
        it->second.last_access = ++access_counter_;
        out.bitmap = it->second.bitmap;
        out.width = it->second.width;
        out.height = it->second.height;
        return true;
    }

    // WIC でデコード（メモリストリーム経由でファイルロックを回避）
    const auto stream = ReadFileToStream(abs_path);
    if (!stream) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_factory_->CreateDecoderFromStream(
        stream.Get(),
        nullptr,
        WICDecodeMetadataCacheOnLoad,
        &decoder
    );
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wic_factory_->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return false;
    }

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    hr = render_target_->CreateBitmapFromWicBitmap(converter.Get(), &bitmap);
    if (FAILED(hr)) {
        return false;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);

    // ピクセルサイズを DIP に変換
    float scale_x, scale_y;
    GetDpiScale(scale_x, scale_y);

    CachedImage cached;
    cached.bitmap = bitmap;
    cached.width = static_cast<float>(w) / scale_x;
    cached.height = static_cast<float>(h) / scale_y;
    cached.last_access = ++access_counter_;
    cache_[abs_path] = cached;
    EvictLruIfNeeded();

    out.bitmap = bitmap;
    out.width = cached.width;
    out.height = cached.height;
    return true;
}

bool ImageLoader::GetCachedImage(const std::wstring& abs_path, DiagramEntry& out) const
{
    const auto it = cache_.find(abs_path);
    if (it != cache_.end()) {
        it->second.last_access = ++access_counter_;
        out.bitmap = it->second.bitmap;
        out.width = it->second.width;
        out.height = it->second.height;
        return true;
    }
    return false;
}

void ImageLoader::RequestLoadAsync(const std::wstring& abs_path,
    Callback on_complete, void* user_data)
{
    {
        const std::lock_guard lock(pending_mutex_);
        if (!pending_paths_.insert(abs_path).second) {
            return;
        }
    }

    if (!scheduler_) {
        return;
    }

    const uint32_t gen = cancel_gen_.load();
    scheduler_->Post([this, path = abs_path, on_complete, user_data, gen] {
        if (cancel_gen_.load() != gen) {
            return;
        }

        DecodeResult result;
        result.path = path;
        result.on_complete = on_complete;
        result.user_data = user_data;

        if (wic_factory_) {
            const auto stream = ReadFileToStream(path);
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            HRESULT hr = stream ? wic_factory_->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder) : E_FAIL;

            if (SUCCEEDED(hr)) {
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                hr = decoder->GetFrame(0, &frame);
                if (SUCCEEDED(hr)) {
                    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                    hr = wic_factory_->CreateFormatConverter(&converter);
                    if (SUCCEEDED(hr)) {
                        hr = converter->Initialize(
                            frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapDitherTypeNone, nullptr, 0.0f,
                            WICBitmapPaletteTypeCustom);
                        if (SUCCEEDED(hr)) {
                            UINT w = 0, h = 0;
                            frame->GetSize(&w, &h);
                            result.converter = converter;
                            result.width = static_cast<float>(w);
                            result.height = static_cast<float>(h);
                            result.success = true;
                        }
                    }
                }
            }
        }

        if (cancel_gen_.load() != gen) {
            return;
        }

        {
            const std::lock_guard lock(result_mutex_);
            completed_.emplace_back(std::move(result));
        }

        if (hwnd_) {
            PostMessage(hwnd_, msg_id_, 0, 0);
        }
    });
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

    Callback last_cb = nullptr;
    void* last_data = nullptr;

    float scale_x, scale_y;
    GetDpiScale(scale_x, scale_y);

    for (auto& r : results) {
        if (r.success && r.converter && render_target_) {
            if (!cache_.contains(r.path)) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                const HRESULT hr = render_target_->CreateBitmapFromWicBitmap(r.converter.Get(), &bitmap);
                if (SUCCEEDED(hr) && bitmap) {
                    CachedImage cached;
                    cached.bitmap = bitmap;
                    cached.width = r.width / scale_x;
                    cached.height = r.height / scale_y;
                    cached.last_access = ++access_counter_;
                    cache_[r.path] = cached;
                }
            }
        }

        last_cb = r.on_complete;
        last_data = r.user_data;
    }

    EvictLruIfNeeded();

    if (last_cb) {
        last_cb(last_data);
    }
}

void ImageLoader::EvictLruIfNeeded()
{
    while (cache_.size() > kMaxCacheEntries) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.last_access < oldest->second.last_access) {
                oldest = it;
            }
        }
        cache_.erase(oldest);
    }
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
