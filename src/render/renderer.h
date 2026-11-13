#pragma once
#include "render_params.h"
#include "theme.h"
#include "command_generator.h"
#include "ui_constants.h"
#include "win_handle.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

// d3d11.h / dxgi は renderer_device.cpp だけで読み、renderer.h から app.h を読む翻訳単位へ広げない。
struct ID3D11Device;
struct IDXGISwapChain2;
struct IWICImagingFactory;
struct IWICBitmap;
class MathMaskCache;

// D3D デバイスの作成 (数十 ms) を起動処理と重ねるため、wWinMain の冒頭で呼ぶ。
void PrefetchD3DDevice();
// 先行作成が終わるとシグナルになる。先行作成していないか、受け取り済みなら nullptr。
HANDLE D3DPrefetchDoneEvent() noexcept;

// 描画デバイスとフレームの段階。D3D に触らないので、デバイスのロストを起こさずに遷移を試せる。
class RenderDeviceState {
public:
    // デバイスを作る前。描かない。
    struct NoDevice {};
    struct Ready {
        // 次のフレームを描いてよいこと (Frame Latency Waitable Object のシグナル) を受け取った。表示するまで受け取った扱いのままにし、
        // 描かずに終わったフレームの後で待ち直さない。
        bool frame_acquired = false;
    };
    // EndDraw・Present・Resize がデバイスのロストを知らせたか、作り直しに失敗した。次のフレームの冒頭で作り直す。
    struct Lost {};

    bool IsReady() const noexcept
    {
        return std::holds_alternative<Ready>(state_);
    }
    bool IsLost() const noexcept
    {
        return std::holds_alternative<Lost>(state_);
    }
    bool NeedsFrame() const noexcept
    {
        const auto* ready = std::get_if<Ready>(&state_);
        return ready && !ready->frame_acquired;
    }
    void MarkFrameAcquired() noexcept
    {
        if (auto* ready = std::get_if<Ready>(&state_)) {
            ready->frame_acquired = true;
        }
    }
    void MarkCreated() noexcept
    {
        state_ = Ready{};
    }
    void MarkLost() noexcept
    {
        state_ = Lost{};
    }
    // 表示する (Present する) なら true。
    bool OnEndDraw(HRESULT hr) noexcept
    {
        if (hr == D2DERR_RECREATE_TARGET) {
            MarkLost();
            return false;
        }
        return SUCCEEDED(hr);
    }
    void OnPresent(HRESULT hr) noexcept
    {
        // TDR / ドライバ内部障害もデバイス再生成で復旧するため REMOVED/RESET と同様に扱う。
        // OCCLUDED(成功) / WAS_STILL_DRAWING / INVALID_CALL は誤爆になるため拾わない。
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
            MarkLost();
            return;
        }
        // 表示されなかったフレーム (隠れていた・失敗した) はシグナルを消費しないので、取れた扱いのまま次に回す。
        if (auto* ready = std::get_if<Ready>(&state_); ready && SUCCEEDED(hr) && hr != DXGI_STATUS_OCCLUDED) {
            ready->frame_acquired = false;
        }
    }

private:
    std::variant<NoDevice, Ready, Lost> state_;
};

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
        // デバイスを失ったため、このフレームは表示されていない。すぐ描き直す。
        RedrawNow,
        // デバイスを作り直した。旧デバイスで作った画像のビットマップと図のコマンドリストを呼び出し側が捨ててから描き直す。
        DeviceRecreated,
        // デバイスを作り直せなかった。すぐ描き直すと失敗を繰り返して UI スレッドを占有するので間を置く。
        RetryLater,
    };
    // params.loading_angle があれば、本文ペイン・検索バー・スクロールバー・ナビボタンの代わりにスピナーを描く。
    FrameResult Render(const RenderParams& params);
    void SetDpi(float dpi) noexcept;
    // 最小化中に D2D の内部キャッシュとドライバ側の一時確保を手放す。
    void TrimForMinimize() noexcept;
    // 別のアプリへ切り替わった後に、作り置いた短い文字列のレイアウトを手放す。戻った最初のフレームで見えている分を作り直す。
    void TrimForInactive() noexcept;
    // 最初の描画の前に呼ぶ (App::CreateRenderDevice 参照)。既存のデバイスがあれば捨てて作り直す。
    bool CreateDevice();
    ID3D11Device* GetD3DDevice() const noexcept
    {
        return d3d_device_.Get();
    }
    ID2D1Device* GetD2DDevice() const noexcept
    {
        return d2d_device_.Get();
    }
    ID2D1RenderTarget* GetRenderTarget() const noexcept
    {
        return device_context_.Get();
    }
    // 描画面の DIP サイズ。デバイスが無い間も Resize に追従する。
    D2D1_SIZE_F GetSize() const noexcept
    {
        const float scale = DpiScaleFrom(dpi_);
        return { PixelToDip(static_cast<float>(width_px_), scale), PixelToDip(static_cast<float>(height_px_), scale) };
    }
    ID2D1Factory* GetD2DFactory() const noexcept
    {
        return d2d_factory_.Get();
    }
    IDWriteFactory* GetDWriteFactory() const noexcept
    {
        return dwrite_factory_.Get();
    }
    IWICImagingFactory* GetWICFactory() const noexcept
    {
        return wic_factory_.Get();
    }
    // ライト/ダーク切替。寸法とフォントは共通なのでテキストレイアウトを維持し、ブラシの色だけ差し替える。
    // デバイスを作ったときも、ここでブラシを作る。
    void OnThemeColorsChanged();
    // ズーム。大きさがズームに従う書式 (本文の記号・番号・プレースホルダ) だけ作り直す。タイトルバーやサイドペインの書式は
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

    // フレーム待ちの上限。GPU が止まっても UI スレッドを永久にブロックしないよう、これを過ぎたら諦めて描く。
    static constexpr DWORD kFrameWaitTimeoutMs = 1000;
    // 次のフレームを描いてよくなるのを待つためのハンドル。取れている間とロスト中は nullptr。
    HANDLE FrameWaitHandle() const noexcept
    {
        return device_.NeedsFrame() ? frame_latency_waitable_.get() : nullptr;
    }
    // FrameWaitHandle の待ちが成立した (シグナルを消費した) ことを記録する。
    void MarkFrameAcquired() noexcept
    {
        device_.MarkFrameAcquired();
    }

    int HitTestSearchInput(std::wstring_view query, float local_x, float max_width)
    {
        return ::HitTestSearchInput(text_layouts_, fmt_.search_input.Get(), query, local_x, max_width);
    }

    // タイトルバーとサイドペインの描画コマンドを組み立てる。デバイスに依存しないので、
    // デバイスの作成前に呼ぶと最初のフレームの文字レイアウトを先に作れる。
    void PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& side_panes);

    // ファイル一覧と目次の項目は本文と主フォントもロケールも同じなので、本文の対応を共有し、解決を二度しない。
    void SetPaneItemFonts(const FallbackFontMap* fonts) noexcept
    {
        pane_item_fonts_ = fonts;
    }

private:
    // 本文ペインに重ねるもの (ナビボタン・ジェスチャー・トースト・検索バー・スクロールバー、読み込み中はスピナー) を
    // frame_cmds_ に積む。
    void GenOverlays(const RenderParams& p);

    // 倍率が変わったら作り直す。
    void EnsurePaneListFormats(PaneTarget target, float zoom);
    void GenGestureTrail(const std::vector<DipPoint>& points);

    // ---- デバイスとスワップチェーン (renderer_device.cpp) ----
    bool CreateFactories(HWND hwnd);
    bool CreateDeviceResources();
    bool CreateSwapChainBitmap();
    // 表示範囲を描画面の左上 width x height にする。失敗したら false。
    bool SetSourceSize(UINT width, UINT height) noexcept;
    void ConfigureFrameLatency() noexcept;
    // 次のフレームを描いてよいか (Frame Latency Waitable Object のシグナル) を最大 kFrameWaitTimeoutMs 待つ。
    // GPU パイプラインの 1 フレーム遅れに同期し、CPU 側を Present の Vsync ブロックまで詰めずに済ませる。
    // 取れたら Present するまで取れた扱いのままにして、描かずに終わったフレームの後で待ち直さない。
    void AcquireFrame() noexcept;
    // EndDraw 後に呼ぶ。デバイスロスト系の失敗は device_ に知らせる。
    void Present() noexcept;

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
    RenderDeviceState device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swap_chain_;
    UniqueNullHandle frame_latency_waitable_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;

    FixedBrushArray brushes_;
    // インライン数式の SVG の形のマスク。デバイスごとに作り直す。
    std::unique_ptr<MathMaskCache> math_masks_;

    void RecreateAllTextFormats();
    void CreateTextFormats(bool zoomed_only = false);

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
        Microsoft::WRL::ComPtr<IDWriteTextFormat> link_status;
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
    const FallbackFontMap* pane_item_fonts_ = nullptr;

    // CommandGenerator と共有する。
    TextLayoutCache text_layouts_;
    // 1 フレームの描画コマンド。タイトルバーとサイドペイン・本文・本文に重ねるものの順に積み、まとめて実行する。
    // タイトルバーとサイドペインはデバイスに依存しないので、作成前に組み立てて最初のフレームの文字レイアウトを先に作れる。
    DrawCommandList frame_cmds_;
    // frame_cmds_ が指すジェスチャーの軌跡。次のフレームまで持つ。
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> gesture_trail_;

    // デバイスに依存しない PBGRA 変換済みのアイコン。デバイスロスト後もここから作り直す。
    Microsoft::WRL::ComPtr<IWICBitmap> app_icon_wic_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> app_icon_bitmap_;
    void DecodeAppIcon();
    void LoadAppIconBitmap();
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> gesture_stroke_style_;

    const Theme* theme_ = nullptr;
    CommandGenerator cmd_generator_;
};
