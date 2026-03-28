#include "image_loader.h"

ImageLoader::~ImageLoader()
{
    Shutdown();
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
    worker_thread_ = std::thread(&ImageLoader::WorkerLoop, this);
}

void ImageLoader::Shutdown()
{
    {
        std::lock_guard lock(queue_mutex_);
        shutdown_flag_.store(true);
    }
    queue_cv_.notify_one();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
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

    // WIC でデコード
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_factory_->CreateDecoderFromFilename(
        abs_path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
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

    // キャッシュに格納
    CachedImage cached;
    cached.bitmap = bitmap;
    cached.width = static_cast<float>(w);
    cached.height = static_cast<float>(h);
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

    for (auto& r : results) {
        if (r.success && r.converter && render_target_) {
            if (!cache_.contains(r.path)) {
                ComPtr<ID2D1Bitmap> bitmap;
                HRESULT hr = render_target_->CreateBitmapFromWicBitmap(
                    r.converter.Get(), &bitmap);
                if (SUCCEEDED(hr) && bitmap) {
                    CachedImage cached;
                    cached.bitmap = bitmap;
                    cached.width = r.width;
                    cached.height = r.height;
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
            ComPtr<IWICBitmapDecoder> decoder;
            HRESULT hr = wic->CreateDecoderFromFilename(
                req.path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder);

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
