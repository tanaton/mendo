#include "image_loader.h"
#include "file_loader.h"
#include "task_scheduler.h"
#include "ui_constants.h"
#include "win_handle.h"
#include <optional>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

// ファイルを共有モードでメモリに読み込みIStreamとして返す。
// CreateDecoderFromFilename はファイルを排他的に開くため、
// 外部エディタ等がファイルを更新できなくなる問題を回避する。
static Microsoft::WRL::ComPtr<IStream> ReadFileToStream(const std::wstring& path)
{
    UniqueHandle hFile{ CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
    if (!hFile) {
        return nullptr;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile.get(), &size) || size.QuadPart == 0 || size.QuadPart > MAX_FILE_SIZE) {
        return nullptr;
    }

    const auto alloc_size = static_cast<SIZE_T>(size.QuadPart);
    UniqueGlobalMem hMem{ GlobalAlloc(GMEM_MOVEABLE, alloc_size) };
    if (!hMem) {
        return nullptr;
    }

    void* ptr = GlobalLock(hMem.get());
    if (!ptr) {
        return nullptr;
    }
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(hFile.get(), ptr, static_cast<DWORD>(alloc_size), &bytesRead, nullptr);
    GlobalUnlock(hMem.get());

    if (!ok || bytesRead != alloc_size) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hMem.get(), TRUE, &stream))) {
        return nullptr;
    }
    hMem.release(); // CreateStreamOnHGlobal(TRUE) が所有権を取得
    return stream;
}

// WIC デコードパイプラインの共通処理。
// IStream から画像をデコードし、FormatConverter とピクセルサイズを返す。
// LoadImage（同期）と RequestLoadAsync（非同期）の両方から使用される。
struct WicDecodeResult {
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    UINT pixel_width = 0;
    UINT pixel_height = 0;
};

static std::optional<WicDecodeResult> DecodeFromStream(
    IWICImagingFactory* wic, IStream* stream)
{
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    return WicDecodeResult{ converter, w, h };
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

    const auto decoded = DecodeFromStream(wic_factory_.Get(), stream.Get());
    if (!decoded) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    const HRESULT hr = render_target_->CreateBitmapFromWicBitmap(decoded->converter.Get(), &bitmap);
    if (FAILED(hr)) {
        return false;
    }

    // ピクセルサイズを DIP に変換
    float scale_x, scale_y;
    GetDpiScale(scale_x, scale_y);

    CachedImage cached;
    cached.bitmap = bitmap;
    cached.width = static_cast<float>(decoded->pixel_width) / scale_x;
    cached.height = static_cast<float>(decoded->pixel_height) / scale_y;

    out.bitmap = bitmap;
    out.width = cached.width;
    out.height = cached.height;
    cache_.Insert(abs_path, std::move(cached));
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
            if (stream) {
                if (const auto decoded = DecodeFromStream(wic_factory_.Get(), stream.Get())) {
                    result.converter = decoded->converter;
                    result.width = static_cast<float>(decoded->pixel_width);
                    result.height = static_cast<float>(decoded->pixel_height);
                    result.success = true;
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
            if (!cache_.Contains(r.path)) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                const HRESULT hr = render_target_->CreateBitmapFromWicBitmap(r.converter.Get(), &bitmap);
                if (SUCCEEDED(hr) && bitmap) {
                    CachedImage cached;
                    cached.bitmap = bitmap;
                    cached.width = r.width / scale_x;
                    cached.height = r.height / scale_y;
                    cache_.Insert(r.path, std::move(cached));
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

void ImageLoader::InsertCacheEntry(const std::wstring& path, float width, float height)
{
    CachedImage cached;
    cached.width = width;
    cached.height = height;
    cache_.Insert(path, std::move(cached));
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
