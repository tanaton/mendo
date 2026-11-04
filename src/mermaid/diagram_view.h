#pragma once
#include "mermaid_util.h"
#include "tooltip.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>

struct ID2D1DeviceContext5;

namespace diagram_view {

// 8 倍は試作で確かめた範囲。
inline constexpr float kZoomStops[]{ 0.05f, 0.1f, 0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
inline constexpr float kPadding = 16.0f;

// 全体表示の倍率。本文より拡大する窓なので、小さな図は等倍のまま置く。窓を余白より小さく潰したら最小の倍率で止める。
float FitZoom(float view_width, float view_height, float natural_width, float natural_height) noexcept;
// dir (+1 / -1) の向きの次の倍率。全体表示の倍率も止まる位置にし、そのすぐ隣の値は 1 回押しても見た目が変わらないので除く。
// 端なら nullopt。
std::optional<float> NextZoom(float zoom, float fit_zoom, int dir) noexcept;

} // namespace diagram_view

// 図の拡大表示の窓。図の描画命令を倍率を変えて描き、右上のボタン・ホイール (スクロール)・ドラッグ・キーで操作する。
// PopupWindow はキャプチャを取り、ホイールを本体へ渡し、非アクティブで閉じるので、見ながら操作するこの窓には使えない。
class DiagramView {
public:
    // owner の中央に、owner の 9 割の大きさで「読み込み中」として開く。on_close は ×・Esc で呼ぶ (呼ばれた側がこれを壊す)。
    // 開けなければ null。
    static std::unique_ptr<DiagramView> Open(HWND owner, IDWriteFactory* dw, IWICImagingFactory* wic, bool dark,
                                             std::move_only_function<void()> on_close);
    // 窓も壊す。
    ~DiagramView();
    DiagramView(const DiagramView&) = delete;
    DiagramView& operator=(const DiagramView&) = delete;

    // 本体と一緒に壊されていない。
    bool IsOpen() const noexcept
    {
        return hwnd_ != nullptr;
    }
    void BringToFront() const noexcept;
    // タイトル・ボタンのツールチップ・読み込み中の文言を、表示中の言語にする。
    void ApplyLanguage();
    void SetDarkMode(bool dark);
    // 別の図を開いた。
    void ShowLoading();
    // 図が出ていれば位置とボタンで変えた倍率を保つ (保存とテーマの切り替え)。全体表示のままなら新しい大きさに合わせ直す。
    void SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing);

private:
    static constexpr UINT_PTR kTooltipTimer = 2;
    // 並びは右上のバーの左から。
    enum class Button : uint8_t { None, ZoomOut, ZoomIn, Fit };

    DiagramView(IDWriteFactory* dw, IWICImagingFactory* wic, std::move_only_function<void()> on_close);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    // 扱ったメッセージは true を返し、result に戻り値を入れる。
    bool HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result);

    bool EnsureTarget();
    void Paint();
    void PaintBar();
    // 窓と図の大きさから全体表示の倍率と、スクロールの範囲を決め直す。
    void Layout();
    void UpdateScrollBars();
    // 図の左上を置く位置 (DIP)。図が窓より小さい向きは中央に置く。
    D2D1_POINT_2F Origin() const noexcept;
    void ScrollTo(float x, float y);
    void ScrollBy(float dx, float dy)
    {
        ScrollTo(scroll_x_ + dx, scroll_y_ + dy);
    }
    // スクロールバーと同じ送り (SB_LINEUP など) で動かす。キーもこれを使う。
    void Scroll(int bar, WORD request);
    void Step(int dir);
    void Fit();
    // 窓の中央に見えている点を動かさずに倍率を変える。
    void SetZoom(float zoom);
    Button HitButton(float x, float y) const noexcept;
    D2D1_RECT_F ButtonRect(Button button) const noexcept;
    D2D1_RECT_F BarRect() const noexcept;
    bool IsEnabled(Button button) const noexcept;
    void SetHover(Button button);
    void Invalidate() const noexcept
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    float Scale() const noexcept;
    D2D1_POINT_2F ToDip(LPARAM lparam) const noexcept;

    // 窓を作り終えるまでと、本体と一緒に壊された後は null。
    HWND hwnd_ = nullptr;
    std::move_only_function<void()> on_close_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    // 図を記録する DeviceContext と記録したもの。描画先と同じデバイスから作り、描画先を作り直したら捨てる。
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> record_dc_;
    Microsoft::WRL::ComPtr<ID2D1CommandList> commands_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_label_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_symbol_;

    std::shared_ptr<const mermaid_util::DiagramDrawing> drawing_;
    bool dark_ = false;
    Tooltip tooltip_;

    // 大きさと位置は DIP。
    float view_width_ = 0.0f;
    float view_height_ = 0.0f;
    float zoom_ = 1.0f;
    float fit_zoom_ = 1.0f;
    // 全体表示のままなら窓の大きさに追従する。ボタンで倍率を変えたら追従をやめる。
    bool fitting_ = true;
    // 図を余白込みで並べた面の、窓の左上が指す位置。
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    // スクロールバーの出し入れで WM_SIZE から Layout に戻ってくる間は、外側の Layout に任せる。
    bool laying_out_ = false;

    Button hover_ = Button::None;
    Button pressed_ = Button::None;
    bool tracking_leave_ = false;
    // ドラッグ中は、押した位置にスクロール位置を足したもの。ここからポインタの位置を引けば新しいスクロール位置になる。
    std::optional<D2D1_POINT_2F> drag_anchor_;
};
