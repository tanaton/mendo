#include "image_loader.h"
#include "ui_constants.h"
#include <shlwapi.h>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

// ファイルを共有モードでメモリに読み込みIStreamとして返す。
// CreateDecoderFromFilename はファイルを排他的に開くため、
// 外部エディタ等がファイルを更新できなくなる問題を回避する。
static ComPtr<IStream> ReadFileToStream(const std::wstring& path)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0 || size.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(hFile);
        return nullptr;
    }

    std::vector<BYTE> buf(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr);
    CloseHandle(hFile);

    if (!ok || bytesRead != buf.size()) {
        return nullptr;
    }

    ComPtr<IStream> stream;
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

void ImageLoader::Init(ID2D1RenderTarget* rt)
{
    render_target_ = rt;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory_));
    if (FAILED(hr)) {
        wic_factory_.Reset();
    }
}

void ImageLoader::InitAsync(HWND hwnd, UINT msg_id)
{
    hwnd_ = hwnd;
    msg_id_ = msg_id;
    shutdown_flag_.store(false);

    int count = mermaid_util::ComputeWorkerCount(
        std::thread::hardware_concurrency());
    worker_threads_.reserve(count);
    for (int i = 0; i < count; ++i) {
        worker_threads_.emplace_back(&ImageLoader::WorkerLoop, this);
    }
}

void ImageLoader::Shutdown()
{
    {
        std::lock_guard lock(queue_mutex_);
        shutdown_flag_.store(true);
    }
    queue_cv_.notify_all();
    for (auto& t : worker_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    worker_threads_.clear();
}

bool ImageLoader::LoadImage(const std::wstring& abs_path, DiagramEntry& out)
{
    if (!wic_factory_ || !render_target_) {
        return false;
    }

    // キャッシュ確認
    auto it = cache_.find(abs_path);
    if (it != cache_.end()) {
        out.bitmap = it->second.bitmap;
        out.width = it->second.width;
        out.height = it->second.height;
        return true;
    }

    // WIC でデコード（メモリストリーム経由でファイルロックを回避）
    auto stream = ReadFileToStream(abs_path);
    if (!stream) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_factory_->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
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

    ComPtr<ID2D1Bitmap> bitmap;
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
    cache_[abs_path] = cached;

    out.bitmap = bitmap;
    out.width = cached.width;
    out.height = cached.height;
    return true;
}

bool ImageLoader::GetCachedImage(const std::wstring& abs_path, DiagramEntry& out) const
{
    auto it = cache_.find(abs_path);
    if (it != cache_.end()) {
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
        std::lock_guard lock(queue_mutex_);
        if (!pending_paths_.insert(abs_path).second) {
            return;
        }
        request_queue_.push({ abs_path, on_complete, user_data });
    }
    queue_cv_.notify_one();
}

void ImageLoader::ProcessCompletedDecodes()
{
    std::vector<DecodeResult> results;
    {
        std::lock_guard lock(result_mutex_);
        results.swap(completed_);
    }

    if (results.empty()) {
        return;
    }

    {
        std::lock_guard lock(queue_mutex_);
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
                ComPtr<ID2D1Bitmap> bitmap;
                HRESULT hr = render_target_->CreateBitmapFromWicBitmap(
                    r.converter.Get(), &bitmap);
                if (SUCCEEDED(hr) && bitmap) {
                    CachedImage cached;
                    cached.bitmap = bitmap;
                    cached.width = r.width / scale_x;
                    cached.height = r.height / scale_y;
                    cache_[r.path] = cached;
                }
            }
        }

        last_cb = r.on_complete;
        last_data = r.user_data;
    }

    if (last_cb) {
        last_cb(last_data);
    }
}

void ImageLoader::CancelPending()
{
    {
        std::lock_guard lock(queue_mutex_);
        std::queue<PendingRequest> empty;
        request_queue_.swap(empty);
        pending_paths_.clear();
    }
    {
        std::lock_guard lock(result_mutex_);
        completed_.clear();
    }
}

void ImageLoader::WorkerLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic));

    while (true) {
        PendingRequest req;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return shutdown_flag_.load() || !request_queue_.empty();
            });
            if (shutdown_flag_.load() && request_queue_.empty()) {
                break;
            }
            req = std::move(request_queue_.front());
            request_queue_.pop();
        }

        DecodeResult result;
        result.path = req.path;
        result.on_complete = req.on_complete;
        result.user_data = req.user_data;

        if (wic) {
            auto stream = ReadFileToStream(req.path);
            ComPtr<IWICBitmapDecoder> decoder;
            HRESULT hr = stream ? wic->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder) : E_FAIL;

            if (SUCCEEDED(hr)) {
                ComPtr<IWICBitmapFrameDecode> frame;
                hr = decoder->GetFrame(0, &frame);
                if (SUCCEEDED(hr)) {
                    ComPtr<IWICFormatConverter> converter;
                    hr = wic->CreateFormatConverter(&converter);
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

        {
            std::lock_guard lock(result_mutex_);
            completed_.push_back(std::move(result));
        }

        if (hwnd_) {
            PostMessage(hwnd_, msg_id_, 0, 0);
        }
    }

    CoUninitialize();
}
