#pragma once
#include "brush_id.h"
#include "render_params.h"
#include "theme.h"
#include "command_generator.h"
#include "d2d_render_backend.h"
#include "text_layout_cache.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

struct IWICBitmap;
#include <functional>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Renderer {
public:
    Renderer() noexcept;
    ~Renderer();

    // theme は ThemeService が持つ今のテーマ。中身を書き換えたら OnThemeColorsChanged / OnThemeMetricsChanged で知らせる。
    bool Init(HWND hwnd, const Theme& theme);
    // D2DRenderBackend::Resize を参照。
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
    // 最初の描画の前に呼ぶ (App::CreateRenderDevice 参照)。
    bool CreateDevice();
    ID2D1RenderTarget* GetRenderTarget() const noexcept;
    D2D1_SIZE_F GetSize() const noexcept
    {
        return backend_.GetSize();
    }
    ID2D1Factory* GetD2DFactory() const noexcept
    {
        return backend_.GetD2DFactory();
    }
    IDWriteFactory* GetDWriteFactory() const noexcept
    {
        return backend_.GetDWriteFactory();
    }
    IWICImagingFactory* GetWICFactory() const noexcept
    {
        return backend_.GetWICFactory();
    }
    // ライト/ダーク切替。寸法とフォントは共通なのでテキストレイアウトを維持し、ブラシの色だけ差し替える。
    void OnThemeColorsChanged()
    {
        RecreateBrushes();
    }
    // ズーム。書式を作り直す。
    void OnThemeMetricsChanged()
    {
        RecreateAllTextFormats();
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

    // D2DRenderBackend の同名関数を参照。
    HANDLE FrameWaitHandle() const noexcept
    {
        return backend_.FrameWaitHandle();
    }
    void MarkFrameAcquired() noexcept
    {
        backend_.MarkFrameAcquired();
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
    void GenGestureTrail(const std::pmr::vector<GesturePoint>& points);

    D2DRenderBackend backend_;
    ID2D1DeviceContext* rt() const noexcept
    {
        return backend_.GetRenderTarget();
    }

    FixedBrushArray brushes_;

    void RecreateBrushes();
    void RecreateAllTextFormats();
    void CreateTextFormats();

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
    std::pmr::vector<std::pmr::wstring> toc_titles_;

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
