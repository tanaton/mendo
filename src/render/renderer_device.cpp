#include "renderer.h"
#include "diagram_raster.h"
#include "ui_constants.h"
#include "wic_util.h"
#include "win32_util.h"
#include <d2d1_1.h>
#include <d3d11.h>
// IDXGISwapChain2 / SetMaximumFrameLatency / GetFrameLatencyWaitableObject 用。dxgi1_2.h も読む。
#include <dxgi1_3.h>
#include <wincodec.h>
#include <algorithm>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D11Device> CreateD3DDevice() noexcept
{
    MENDO_PROFILE("CreateD3DDevice");
    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    ComPtr<ID3D11Device> device;
    const auto create_device = [&](D3D_DRIVER_TYPE driver_type) {
        return D3D11CreateDevice(
            nullptr, driver_type, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels, ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION, &device, nullptr, nullptr);
    };
    if (FAILED(create_device(D3D_DRIVER_TYPE_HARDWARE))) {
        // GPU を使えない環境 (RDP / VM / ドライバ障害など) でも描画できるよう、ソフトウェアの WARP に落とす。
        if (const HRESULT hr = create_device(D3D_DRIVER_TYPE_WARP); FAILED(hr)) {
            LogHrFailure("D3D11CreateDevice (WARP)", hr);
            return nullptr;
        }
    }
    return device;
}

// 初回の CreateDeviceResources が join して受け取る。デバイスロスト後の再作成は同期で作る。
ComPtr<ID3D11Device> g_prefetched_device;
std::jthread g_prefetch_thread;

} // namespace

void PrefetchD3DDevice()
{
    g_prefetch_thread = std::jthread([] static noexcept { g_prefetched_device = CreateD3DDevice(); });
}

HANDLE D3DPrefetchDoneEvent() noexcept
{
    // スレッドのハンドルは終了でシグナルになる。
    return g_prefetch_thread.joinable() ? g_prefetch_thread.native_handle() : nullptr;
}

void Renderer::ConfigureFrameLatency() noexcept
{
    frame_latency_waitable_.reset();
    // 既定の MaximumFrameLatency=3 はホイール連打でカクつきの原因になる。1 まで詰める。
    // 失敗時は既定の 3 のままになる。waitable は取得しても 3 フレーム遅れに同期するだけで
    // 意図しない wait を招くため、SetMaximumFrameLatency が成功した時のみ取得する。
    if (FAILED(swap_chain_->SetMaximumFrameLatency(1))) {
        return;
    }
    frame_latency_waitable_.reset(swap_chain_->GetFrameLatencyWaitableObject());
}

// d3d11 / dxgi の型の ComPtr を作って壊すので、それらを読むこの TU で定義する。
Renderer::Renderer() noexcept = default;
Renderer::~Renderer() = default;

bool Renderer::CreateFactories(HWND hwnd)
{
    hwnd_ = hwnd;

    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    if (dpi_ == 0.0f) {
        dpi_ = DEFAULT_DPI;
    }
    if (RECT rc{}; GetClientRect(hwnd, &rc)) {
        width_px_ = static_cast<UINT>(rc.right - rc.left);
        height_px_ = static_cast<UINT>(rc.bottom - rc.top);
    }

    const D2D1_FACTORY_OPTIONS opts{};
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &opts,
        reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        LogHrFailure("D2D1CreateFactory", hr);
        return false;
    }

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        LogHrFailure("DWriteCreateFactory", hr);
        return false;
    }

    // Renderer・ImageLoader で共有。アイコン／画像／ダイアグラムは WIC が無いと
    // 機能しないため fail-fast。
    wic_factory_ = wic_util::CreateWicFactory("WIC ImagingFactory creation");
    return wic_factory_ != nullptr;
}

bool Renderer::CreateDeviceResources()
{
    if (g_prefetch_thread.joinable()) {
        g_prefetch_thread.join();
        d3d_device_ = std::move(g_prefetched_device);
    }
    else {
        d3d_device_ = CreateD3DDevice();
    }
    if (!d3d_device_) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = d3d_device_.As(&dxgi_device);
    if (FAILED(hr)) {
        LogHrFailure("ID3D11Device::As<IDXGIDevice>", hr);
        return false;
    }

    hr = d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_);
    if (FAILED(hr)) {
        LogHrFailure("ID2D1Factory1::CreateDevice", hr);
        return false;
    }

    hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &device_context_);
    if (FAILED(hr)) {
        LogHrFailure("ID2D1Device::CreateDeviceContext", hr);
        return false;
    }

    device_context_->SetDpi(dpi_, dpi_);

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        LogHrFailure("IDXGIDevice::GetAdapter", hr);
        return false;
    }

    ComPtr<IDXGIFactory2> dxgi_factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) {
        LogHrFailure("IDXGIAdapter::GetParent<IDXGIFactory2>", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width_px_;
    scd.Height = height_px_;
    buffer_width_ = width_px_;
    buffer_height_ = height_px_;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    // Waitable で GPU が次フレームのバッファを準備できるまで CPU を待たせる。
    // これがないと Present の Vsync ブロックで CPU が完全停止する。
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swap_chain;
    hr = dxgi_factory->CreateSwapChainForHwnd(d3d_device_.Get(), hwnd_, &scd, nullptr, nullptr, &swap_chain);
    if (FAILED(hr)) {
        LogHrFailure("CreateSwapChainForHwnd", hr);
        return false;
    }
    // Windows 8.1 以降は必ず取れる (GetDpiForWindow を使うので Windows 10 1607 より前では起動しない)。
    hr = swap_chain.As(&swap_chain_);
    if (FAILED(hr)) {
        LogHrFailure("IDXGISwapChain1::As<IDXGISwapChain2>", hr);
        return false;
    }

    ConfigureFrameLatency();
    return CreateSwapChainBitmap();
}

bool Renderer::CreateSwapChainBitmap()
{
    ComPtr<IDXGISurface> surface;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        LogHrFailure("IDXGISwapChain1::GetBuffer", hr);
        return false;
    }

    // 不透明なウィンドウなので IGNORE にする。PREMULTIPLIED だと D2D が透過のある描画先とみなし、
    // 文字のアンチエイリアスを ClearType からグレースケールに落とす。
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        dpi_, dpi_);

    ComPtr<ID2D1Bitmap1> target_bitmap;
    hr = device_context_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_bitmap);
    if (FAILED(hr)) {
        LogHrFailure("CreateBitmapFromDxgiSurface", hr);
        return false;
    }

    device_context_->SetTarget(target_bitmap.Get());
    return true;
}

bool Renderer::SetSourceSize(UINT width, UINT height) noexcept
{
    return SUCCEEDED(swap_chain_->SetSourceSize(width, height));
}

void Renderer::Resize(UINT width, UINT height, bool live) noexcept
{
    if (width == 0 || height == 0) {
        return;
    }
    width_px_ = width;
    height_px_ = height;
    if (!swap_chain_ || !device_context_) {
        return;
    }
    const bool fits = live ? (width <= buffer_width_ && height <= buffer_height_)
                           : (width == buffer_width_ && height == buffer_height_);
    if (fits && SetSourceSize(width, height)) {
        return;
    }

    UINT buffer_width = width;
    UINT buffer_height = height;
    if (live) {
        // 1 回のドラッグで何度も作り直さないよう、モニタ全体まで広げられる大きさで取る。
        MONITORINFO mi{ .cbSize = sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            buffer_width = std::max(buffer_width, static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left));
            buffer_height = std::max(buffer_height, static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top));
        }
    }

    // ResizeBuffers の前にターゲット参照を切る必要がある
    device_context_->SetTarget(nullptr);

    // SwapChain 生成時と同じ Flags を渡す必要がある（Waitable は ResizeBuffers でも維持）。
    const auto resize_buffers = [&](UINT w, UINT h) {
        const HRESULT hr = swap_chain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(hr)) {
            LogHrFailure("IDXGISwapChain1::ResizeBuffers", hr);
            return false;
        }
        buffer_width_ = w;
        buffer_height_ = h;
        return true;
    };
    bool ok = resize_buffers(buffer_width, buffer_height);
    // 表示範囲を絞れないと大きいバッファが窓に引き伸ばされるので、ちょうどの大きさで取り直す。
    if (ok && (buffer_width != width || buffer_height != height) && !SetSourceSize(width, height)) {
        ok = resize_buffers(width, height);
    }
    // SetTarget(nullptr) のまま戻ると次回 BeginDraw で D2DERR_WRONG_STATE。
    if (!ok || !CreateSwapChainBitmap()) {
        device_.MarkLost();
    }
}

void Renderer::SetDpi(float dpi) noexcept
{
    dpi_ = dpi;
    if (device_context_) {
        device_context_->SetDpi(dpi, dpi);
    }
}

void Renderer::TrimForInactive() noexcept
{
    // 目次やファイル一覧をスクロールすると上限 512 個まで溜まる。上限を下げると目次を戻るたびに作り直す (目次の全行が
    // 入れ替わるフレームで 0.38ms) ので、上限は保ち、使わない間だけ手放すほうをユーザーが選んだ (2026-11-13)。
    // 見出し 2000 個の目次をホイールで 300 回送ってから切り替えると、commit 40.0〜40.4 → 39.2〜39.7MB。
    // 1 秒後の trim の直前に捨てると、解放の後で触ったページが private WS に 0.5MB 残るので、切り替わった時点で捨てる。
    // 描画コマンドが指すレイアウトも消えるので、描くコマンドも捨てる (次のフレームは作り直してから描く)。
    frame_cmds_.clear();
    text_layouts_.Clear();
}

void Renderer::TrimForMinimize() noexcept
{
    if (!d2d_device_) {
        return;
    }
    d2d_device_->ClearResources();
    math_masks_->Reset(d2d_device_.Get());
    // IDXGIDevice3::Trim の前に ClearState で参照を外しておく必要がある。
    ComPtr<ID3D11DeviceContext> d3d_ctx;
    d3d_device_->GetImmediateContext(&d3d_ctx);
    d3d_ctx->ClearState();
    ComPtr<IDXGIDevice3> dxgi_device;
    if (SUCCEEDED(d3d_device_.As(&dxgi_device))) {
        dxgi_device->Trim();
    }
}

void Renderer::AcquireFrame() noexcept
{
    if (!device_.NeedsFrame() || !frame_latency_waitable_) {
        return;
    }
    // alertable=FALSE: UI スレッドは APC を使わないので、意図しない早期復帰を防ぐ。
    if (WaitForSingleObjectEx(frame_latency_waitable_.get(), kFrameWaitTimeoutMs, FALSE) == WAIT_OBJECT_0) {
        device_.MarkFrameAcquired();
    }
}

void Renderer::Present() noexcept
{
    device_.OnPresent(swap_chain_->Present(1, 0));
}

bool Renderer::CreateDevice()
{
    // 作り直しに失敗したら次の描画で再試行させるため、成功するまでロスト扱いにする。
    device_.MarkLost();
    frame_latency_waitable_.reset();
    // flip モデルのスワップチェーンは、前のものが解放されるまで同じ HWND に作れない。ブラシや図の描画命令が
    // 古いデバイスを生かしていても手放せるよう描画先を外し、D3D の遅延解放をフラッシュで済ませる。
    if (device_context_) {
        device_context_->SetTarget(nullptr);
    }
    math_masks_->Reset(nullptr);
    device_context_.Reset();
    d2d_device_.Reset();
    swap_chain_.Reset();
    if (d3d_device_) {
        ComPtr<ID3D11DeviceContext> d3d_context;
        d3d_device_->GetImmediateContext(&d3d_context);
        d3d_context->ClearState();
        d3d_context->Flush();
    }
    d3d_device_.Reset();

    if (!CreateDeviceResources()) {
        // 途中で失敗すると描画先の無い context が残る。GetRenderTarget の非 null は使えることを意味する。
        device_context_.Reset();
        return false;
    }
    device_.MarkCreated();
    math_masks_->Reset(d2d_device_.Get());
    OnThemeColorsChanged();
    LoadAppIconBitmap();
    return true;
}
