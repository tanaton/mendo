#pragma once
#include "brush_id.h"
#include "render_params.h"
#include "theme.h"
#include "command_generator.h"
#include "text_layout_cache.h"
#include "ui_constants.h"
#include "win_handle.h"
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// d3d11.h / dxgi は renderer_device.cpp だけで読み、d2d1_1.h / wincodec.h も含めて renderer.h から app.h を読む翻訳単位へ広げない。
struct ID2D1Factory1;
struct ID2D1Device;
struct ID2D1DeviceContext;
struct ID3D11Device;
struct IDXGISwapChain1;
struct IWICImagingFactory;
struct IWICBitmap;

// D3D デバイスの作成 (数十 ms) を起動処理と重ねるため、wWinMain の冒頭で呼ぶ。
void PrefetchD3DDevice();
// 先行作成が終わるとシグナルになる。先行作成していないか、受け取り済みなら nullptr。
HANDLE D3DPrefetchDoneEvent() noexcept;

// D2D/DirectWrite/WIC のファクトリ、D3D デバイス・スワップチェーンとロスト検知、ブラシとテキスト書式を持ち、1 フレームを描く。
// デバイスとスワップチェーンを扱うメンバ関数は renderer_device.cpp で定義する (d3d11.h / dxgi を読む唯一の TU)。
class Renderer {
public:
    Renderer() noexcept;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // theme は ThemeService が持つ今のテーマ。中身を書き換えたら OnThemeColorsChanged / OnThemeMetricsChanged で知らせる。
    // デバイスとスワップチェーンは作らない (CreateDevice で作る)。
    bool Init(HWND hwnd, const Theme& theme);
    // live: ライブリサイズ中。ResizeBuffers は GPU の同期と再確保を伴うので WM_SIZE ごとに呼ぶと重い。
    // モニタ全体の大きさのバッファを使い回し、表示範囲 (source size) だけを変える。
    void Resize(UINT width, UINT height, bool live) noexcept;

    // 1 フレームの結果。再描画をいつ予約するかは呼び出し側が決める。
    enum class FrameResult : uint8_t {
        Done,
        // デバイスを作り直した・失ったため、このフレームは表示されていない。すぐ描き直す。
        RedrawNow,
        // デバイスを作り直せなかった。すぐ描き直すと失敗を繰り返して UI スレッドを占有するので間を置く。
        RetryLater,
    };
    // params.loading_angle があれば、本文ペイン・検索バー・スクロールバーの代わりにスピナーを描く。
    FrameResult Render(const RenderParams& params);
    void SetDpi(float dpi) noexcept;
    void TrimForMinimize() noexcept;
    // 文書から作った描画用のテキスト (目次項目の UTF-16) を捨てる。文書を差し替えたら呼ぶ。
    void ForgetDocumentText() noexcept
    {
        toc_titles_ = {};
    }
    // 最初の描画の前に呼ぶ (App::CreateRenderDevice 参照)。既存のデバイスがあれば捨てて作り直す。
    bool CreateDevice();
    ID2D1RenderTarget* GetRenderTarget() const noexcept;
    // 描画面の DIP サイズ。デバイスが無い間も Resize に追従する。
    D2D1_SIZE_F GetSize() const noexcept
    {
        const float scale = DpiScaleFrom(dpi_);
        return { PixelToDip(static_cast<float>(width_px_), scale), PixelToDip(static_cast<float>(height_px_), scale) };
    }
    ID2D1Factory* GetD2DFactory() const noexcept;
    IDWriteFactory* GetDWriteFactory() const noexcept
    {
        return dwrite_factory_.Get();
    }
    IWICImagingFactory* GetWICFactory() const noexcept
    {
        return wic_factory_.Get();
    }
    // ライト/ダーク切替。寸法とフォントは共通なのでテキストレイアウトを維持し、ブラシの色だけ差し替える。
    void OnThemeColorsChanged()
    {
        RecreateBrushes();
    }
    // ズーム。大きさがズームに従う書式 (本文・ジェスチャー) だけ作り直す。タイトルバーやサイドペインの書式は
    // ズームしないので、それらで作ったレイアウトも残す (作り直すと 1 段ごとに 80 個ほどで約 2.7ms)。
    void OnThemeMetricsChanged()
    {
        CreateTextFormats(/*zoomed_only=*/true);
    }
    // UI の決まった文言の書式は選んだ言語のロケールで組むので作り直す。
    void OnLanguageChanged()
    {
        RecreateAllTextFormats();
    }

    void SetDeviceLostCallback(std::move_only_function<void(ID2D1RenderTarget*)> cb)
    {
        on_device_lost_ = std::move(cb);
    }

    // フレーム待ちの上限。GPU が止まっても UI スレッドを永久にブロックしないよう、これを過ぎたら諦めて描く。
    static constexpr DWORD kFrameWaitTimeoutMs = 1000;
    // 次のフレームを描いてよくなるのを待つためのハンドル。取れている間とロスト中は nullptr。
    HANDLE FrameWaitHandle() const noexcept
    {
        return (frame_acquired_ || device_lost_) ? nullptr : frame_latency_waitable_.get();
    }
    // FrameWaitHandle の待ちが成立した (シグナルを消費した) ことを記録する。
    void MarkFrameAcquired() noexcept
    {
        frame_acquired_ = true;
    }

    int HitTestSearchInput(std::wstring_view query, float local_x, float max_width) const
    {
        return search_input_.HitTest(query, local_x, max_width);
    }

    // タイトルバーとサイドペインの描画コマンドを組み立てる。デバイスに依存しないので、
    // デバイスの作成前に呼ぶと最初のフレームの文字レイアウトを先に作れる。
    void PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& side_panes);

private:
    // フレーム共通の前処理 (デバイスロスト復旧・BeginDraw・タイトルバー・サイドペイン)。
    // 描画できない場合はそのフレームの結果を返し、その場合 BeginDraw は呼ばれていない。
    std::optional<FrameResult> BeginFrame(const TitleBarRenderState& titlebar, const SidePaneState& side_panes);
    // 本文ペインに重ねるもの (ナビボタン・ジェスチャー・トースト・検索バー・スクロールバー、読み込み中はスピナー) を
    // overlay_cmds_ に積む。
    void GenOverlays(const RenderParams& p);

    // 倍率が変わったら作り直す。
    void EnsurePaneListFormats(PaneTarget target, float zoom);
    void GenGestureTrail(const std::vector<GesturePoint>& points);

    // ---- デバイスとスワップチェーン (renderer_device.cpp) ----
    bool CreateFactories(HWND hwnd);
    bool CreateDeviceResources();
    bool CreateSwapChainBitmap();
    // 表示範囲を描画面の左上 width x height にする。対応していなければ false。
    bool SetSourceSize(UINT width, UINT height) noexcept;
    void ConfigureFrameLatency() noexcept;
    // 次のフレームを描いてよいか (Frame Latency Waitable Object のシグナル) を最大 kFrameWaitTimeoutMs 待つ。
    // GPU パイプラインの 1 フレーム遅れに同期し、CPU 側を Present の Vsync ブロックまで詰めずに済ませる。
    // 取れたら Present するまで取れた扱いのままにして、描かずに終わったフレームの後で待ち直さない。
    void AcquireFrame() noexcept;
    // EndDraw 後に呼ぶ。デバイスロスト系の失敗は device_lost_ で知らせる。
    void Present() noexcept;
    // 最小化中に D2D の内部キャッシュとドライバ側の一時確保を手放す。
    void TrimDevice() noexcept;

    ID2D1DeviceContext* rt() const noexcept
    {
        return device_context_.Get();
    }

    HWND hwnd_ = nullptr;
    float dpi_ = DEFAULT_DPI;
    UINT width_px_ = 0;
    UINT height_px_ = 0;
    // スワップチェーンのバッファの大きさ。ライブリサイズ中は描画面 (width_px_ x height_px_) より大きい。
    UINT buffer_width_ = 0;
    UINT buffer_height_ = 0;
    // Resize / Present が DXGI_ERROR_DEVICE_REMOVED/RESET 等を検知したか、CreateSwapChainBitmap が失敗した。
    // 次フレームの BeginFrame で RecreateDevice を呼んで戻す。
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

    FixedBrushArray brushes_;

    void RecreateBrushes();
    void RecreateAllTextFormats();
    void CreateTextFormats(bool zoomed_only = false);

    FrameResult EndFrame();
    bool RecreateDevice();
    FrameResult RecreateDeviceForFrame()
    {
        return RecreateDevice() ? FrameResult::RedrawNow : FrameResult::RetryLater;
    }

    struct TextFormats {
        // CommandGenerator が参照する。
        MdPaneFormats md;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> titlebar_text;
        // タイトルバーと検索バーのアイコン。
        Microsoft::WRL::ComPtr<IDWriteTextFormat> ui_icon;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> pane_icon;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> pane_header;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> nav_button;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> gesture_overlay;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> toast_text;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> search_input;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> search_count;
    };
    TextFormats fmt_;

    // サイドペインの一覧の書式。ペインごとの倍率で作る。
    struct PaneListFormats {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> item;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> icon;
        // 作ったときの倍率。0 は未作成。
        float zoom = 0.0f;
    };
    PaneListFormats pane_list_fmt_[2];

    // CommandGenerator と共有する。
    TextLayoutCache text_layouts_;
    // タイトルバーとサイドペインの描画コマンド。本文 (cmd_generator_) より先に実行する。
    // デバイスに依存しないので、作成前に組み立てて最初のフレームの文字レイアウトを先に作れる。
    DrawCommandList chrome_cmds_;
    // 本文の上に重ねるもの。本文の後に実行する。
    DrawCommandList overlay_cmds_;
    // overlay_cmds_ が指すジェスチャーの軌跡。次のフレームまで持つ。
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> gesture_trail_;
    // 目次項目の UTF-16。描いた項目だけ作り、ForgetDocumentText と最小化で捨てる。
    std::vector<std::wstring> toc_titles_;

    SearchInputLayout search_input_;
    // デバイスに依存しない PBGRA 変換済みのアイコン。デバイスロスト後もここから作り直す。
    Microsoft::WRL::ComPtr<IWICBitmap> app_icon_wic_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> app_icon_bitmap_;
    void DecodeAppIcon();
    void LoadAppIconBitmap();
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> gesture_stroke_style_;

    const Theme* theme_ = nullptr;
    CommandGenerator cmd_generator_;
    std::move_only_function<void(ID2D1RenderTarget*)> on_device_lost_;
};
