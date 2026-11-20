#pragma once
#include "zoom_viewer.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 拡大表示の窓に頼まれた操作の記録。窓が壊れた後も見られるよう、窓の外に置く。
struct ZoomWindowLog {
    int opened = 0;
    int loading = 0;
    int front = 0;
    bool alive = false;
    bool dark = false;
    bool fail_open = false;
    // 本体の描画デバイスを差し替えられた回数。
    int device_changes = 0;
    // 出している画像を外すよう頼まれた回数 (ReleaseImage)。
    int released = 0;
    std::wstring title;
    std::vector<std::shared_ptr<const mermaid_util::DiagramDrawing>> drawings;
    struct Image {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        UINT pixel_width = 0;
        UINT pixel_height = 0;
    };
    std::vector<Image> images;
};

// 窓を作らない拡大表示 (本物は開くと前面を取る)。
class FakeZoomWindow final : public IZoomWindow {
public:
    FakeZoomWindow(ZoomWindowLog& log, bool dark, std::wstring_view title) : log_(log)
    {
        ++log_.opened;
        log_.alive = true;
        log_.dark = dark;
        log_.title = title;
    }
    ~FakeZoomWindow() override
    {
        log_.alive = false;
    }
    bool IsOpen() const noexcept override
    {
        return true;
    }
    bool HasContent() const noexcept override
    {
        return has_content_;
    }
    void BringToFront() const noexcept override
    {
        ++log_.front;
    }
    void SetTitle(std::wstring_view title) override
    {
        log_.title = title;
    }
    void ApplyLanguage() override
    {}
    void SetDarkMode(bool dark) override
    {
        log_.dark = dark;
    }
    void ShowLoading() override
    {
        ++log_.loading;
        has_content_ = false;
    }
    void SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing, Microsoft::WRL::ComPtr<ID2D1CommandList>) override
    {
        has_content_ = true;
        image_ = false;
        log_.drawings.push_back(std::move(drawing));
    }
    void SetImage(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap, UINT pixel_width, UINT pixel_height) override
    {
        has_content_ = true;
        image_ = true;
        log_.images.push_back({ std::move(bitmap), pixel_width, pixel_height });
    }
    // 本物と同じく、画像を外したら絵を出していないことにする (図は記録し直せるので残る)。
    void ReleaseImage() override
    {
        ++log_.released;
        has_content_ &= !image_;
    }
    void SetDevice(ID3D11Device*, ID2D1Device*) override
    {
        ++log_.device_changes;
        has_content_ &= !image_;
    }

private:
    ZoomWindowLog& log_;
    bool has_content_ = false;
    bool image_ = false;
};

// ZoomViewer::WindowFactory。工場は関数ポインタで状態を持てないので、static に置いた記録 Log に書く。
template <ZoomWindowLog& Log>
std::unique_ptr<IZoomWindow> OpenFakeZoomWindow(HWND, ID3D11Device*, ID2D1Device*, IDWriteFactory*, IWICImagingFactory*, bool dark,
                                                std::wstring_view title, std::move_only_function<void()>)
{
    return Log.fail_open ? nullptr : std::make_unique<FakeZoomWindow>(Log, dark, title);
}
