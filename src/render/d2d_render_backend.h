#pragma once
#include "ui_constants.h"
#include "win_handle.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

// D3D デバイスの作成 (数十 ms) を起動処理と重ねるため、wWinMain の冒頭で呼ぶ。
void PrefetchD3DDevice();

// D2D ファクトリ・レンダーターゲット・DPI を保持するレンダーバックエンド。
// ブラシとテキストフォーマットは Renderer 側で管理する。
class D2DRenderBackend final {
public:
    // デバイスとスワップチェーンは作らない (CreateDevice で作る)。
    bool Init(HWND hwnd);
    // live: ライブリサイズ中。ResizeBuffers は GPU の同期と再確保を伴うので WM_SIZE ごとに呼ぶと重い。
    // モニタ全体の大きさのバッファを使い回し、表示範囲 (source size) だけを変える。
    void Resize(UINT width, UINT height, bool live = false) noexcept;
    void SetDpi(float dpi) noexcept;
    float GetDpi() const noexcept
    {
        return dpi_;
    }
    // 描画面の DIP サイズ。デバイスが無い間も Resize に追従する。
    D2D1_SIZE_F GetSize() const noexcept
    {
        const float scale = DpiScaleFrom(dpi_);
        return { PixelToDip(static_cast<float>(width_px_), scale), PixelToDip(static_cast<float>(height_px_), scale) };
    }
    // 既存のデバイスがあれば捨てて作り直す。
    bool CreateDevice();

    // 次のフレームを描いてよいか (Frame Latency Waitable Object のシグナル) を最大 timeout_ms 待つ。
    // GPU パイプラインの 1 フレーム遅れに同期し、CPU 側を Present の Vsync ブロックまで詰めずに済ませる。
    // 取れたら Present するまで取れた扱いのままにして、描かずに終わったフレームの後で待ち直さない。
    bool AcquireFrame(DWORD timeout_ms) noexcept;
    // 描画を見送ったフレームの再開を待つためのハンドル。取れている間とロスト中は nullptr。
    HANDLE FrameWaitHandle() const noexcept
    {
        return (frame_acquired_ || device_lost_) ? nullptr : frame_latency_waitable_.get();
    }
    // FrameWaitHandle の待ちが成立した (シグナルを消費した) ことを記録する。
    void MarkFrameAcquired() noexcept
    {
        frame_acquired_ = true;
    }

    // EndDraw 後に呼び出す。デバイスロスト系の失敗は IsDeviceLost() で通知する。
    void Present() noexcept;

    // 最小化中に D2D の内部キャッシュとドライバ側の一時確保を手放す。
    void Trim() noexcept;

    // Resize / Present が DXGI_ERROR_DEVICE_REMOVED/RESET を検知した、もしくは
    // CreateSwapChainBitmap が失敗した時に true を返す。Renderer が次フレーム頭で
    // RecreateDevice を呼んでフラグをクリアする想定。
    bool IsDeviceLost() const noexcept
    {
        return device_lost_;
    }

    ID2D1Factory* GetD2DFactory() const noexcept
    {
        return d2d_factory_.Get();
    }
    ID2D1DeviceContext* GetRenderTarget() const noexcept
    {
        return device_context_.Get();
    }
    IDWriteFactory* GetDWriteFactory() const noexcept
    {
        return dwrite_factory_.Get();
    }
    IWICImagingFactory* GetWICFactory() const noexcept
    {
        return wic_factory_.Get();
    }
    HWND GetHwnd() const noexcept
    {
        return hwnd_;
    }

private:
    bool CreateDeviceResources();
    bool CreateSwapChainBitmap();
    // 表示範囲を描画面の左上 width x height にする。対応していなければ false。
    bool SetSourceSize(UINT width, UINT height) noexcept;
    void ConfigureFrameLatency() noexcept;

    HWND hwnd_ = nullptr;
    float dpi_ = DEFAULT_DPI;
    UINT width_px_ = 0;
    UINT height_px_ = 0;
    // スワップチェーンのバッファの大きさ。ライブリサイズ中は描画面 (width_px_ x height_px_) より大きい。
    UINT buffer_width_ = 0;
    UINT buffer_height_ = 0;
    bool device_lost_ = false;
    bool frame_acquired_ = false;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    UniqueEventHandle frame_latency_waitable_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
};
