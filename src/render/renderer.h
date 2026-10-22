#pragma once
#include "brush_id.h"
#include "render_params.h"
#include "theme.h"
#include "layout.h"
#include "dwrite_measurer.h"
#include "command_generator.h"
#include "d2d_render_backend.h"
#include "text_layout_cache.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Renderer {
public:
    bool Init(HWND hwnd, Theme theme);
    // D2DRenderBackend::Resize を参照。
    void Resize(UINT width, UINT height, bool live = false) noexcept;

    // 1 フレームの結果。再描画をいつ予約するかは呼び出し側が決める。
    enum class FrameResult : uint8_t {
        Done,
        // デバイスを作り直した・失ったため、このフレームは表示されていない。すぐ描き直す。
        RedrawNow,
        // デバイスを作り直せなかった。すぐ描き直すと失敗を繰り返して UI スレッドを占有するので間を置く。
        RetryLater,
    };
    FrameResult Render(const RenderParams& params);
    void SetDpi(float dpi) noexcept;
    void TrimForMinimize() noexcept;
    // 文書から作った描画用のテキスト (目次項目の UTF-16) を捨てる。文書を差し替えたら呼ぶ。
    void ForgetDocumentText() noexcept
    {
        toc_titles_ = {};
    }
    FrameResult DrawLoading(
        float angle,
        const PaneRect& md_pane_rect,
        const SidePaneState& side_panes,
        const TitleBarRenderState& titlebar,
        const GestureRenderState& gesture = {},
        const ToastRenderState& toast = {});

    // 最初の描画の前に呼ぶ (App::CreateRenderDevice 参照)。
    bool CreateDevice();
    ID2D1RenderTarget* GetRenderTarget() const noexcept
    {
        return backend_.GetRenderTarget();
    }
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
    constexpr LayoutEngine& GetLayout() noexcept
    {
        return layout_;
    }
    // 並列計測 (ノード単位 / 巨大テーブルのセル単位) に使う scheduler。Shutdown 前に nullptr へ戻す契約。
    // 設定したらワーカーとフォントを先に起こしておく (DWriteTextMeasurer::WarmUp)。
    void SetLayoutScheduler(TaskScheduler* scheduler) noexcept
    {
        layout_.SetLayoutScheduler(scheduler);
        measurer_.SetScheduler(scheduler);
        if (scheduler) {
            measurer_.WarmUp(*scheduler);
        }
    }
    constexpr const Theme& GetTheme() const noexcept
    {
        return theme_;
    }
    // ライト/ダーク切替用。寸法とフォントは共通なのでテキストレイアウトを維持し、色だけ差し替える。
    void SetTheme(Theme theme);
    // base theme (zoom=1.0) から再構築して現在 zoom を適用する。
    // 累積適用による誤差蓄積を避けるため、ズーム変更経路はこの関数に統一する。
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);

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

    int HitTestSearchInput(std::wstring_view query, float local_x, float max_width) const;
    void SetSearchMatches(const std::pmr::vector<SearchMatch>* matches, int current_index, uint32_t generation) noexcept
    {
        cmd_generator_.SetSearchMatches(matches, current_index, generation);
    }

    // タイトルバーとサイドペインの描画コマンドを組み立てる。デバイスに依存しないので、
    // デバイスの作成前に呼ぶと最初のフレームの文字レイアウトを先に作れる。
    void PrepareChrome(const TitleBarRenderState& titlebar, const SidePaneState& side_panes);

private:
    // フレーム共通の前処理 (デバイスロスト復旧・BeginDraw・タイトルバー・サイドペイン)。
    // 描画できない場合はそのフレームの結果を返し、その場合 BeginDraw は呼ばれていない。
    std::optional<FrameResult> BeginFrame(const TitleBarRenderState& titlebar, const SidePaneState& side_panes);
    void DrawLoadingSpinner(float angle, const PaneRect& md_pane_rect);


    void GenTitleBar(const TitleBarRenderState& tb);
    void GenFileExplorer(const std::pmr::vector<FileEntry>& entries, const SidePaneInstance& pane);
    void GenToc(const SidePaneState& sp, const SidePaneInstance& pane);
    template <typename GenItemFn>
    void GenSidePane(const SidePaneInstance& pane, bool is_file_pane, size_t item_count, std::wstring_view header_text, GenItemFn gen_item);
    void GenSidePaneHeader(const SidePaneInstance& pane, bool is_file_pane, std::wstring_view header_text);
    // 置き方は TextLayoutCache::Origin。
    void EmitChromeText(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float opacity = 1.0f, D2D1_SIZE_F box = {});
    // NO_WRAP の左寄せテキスト。はみ出しは呼び出し側のクリップで切る。
    void EmitLeftText(IDWriteTextFormat* fmt, std::wstring_view text, float x, float y, float height);
    void DrawMdScrollbar(const PaneRect& md_pane_rect, float scroll_y, float total_content_height, bool has_dirty_nodes);
    void DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, NavDirection hovered);
    void DrawGestureTrail(const std::pmr::vector<GesturePoint>& points);
    void DrawTransientOverlays(const GestureRenderState& gesture, const ToastRenderState& toast, const PaneRect& md_pane_rect);
    void DrawGestureOverlay(NavDirection direction, const PaneRect& md_pane_rect);
    void DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect);
    void DrawSearchBar(const SearchBarRenderState& sb, const PaneRect& md_pane_rect);
    // 入力欄のテキスト・選択範囲を描画し、キャレットの x 座標を返す。
    float DrawSearchInputText(const SearchBarRenderState& sb, const SearchBarLayout& sbl);
    // 入力が前フレームと同じなら search_input_ のレイアウトを返し、違えば作り直す。
    IDWriteTextLayout* AcquireSearchInputLayout(const SearchBarRenderState& sb, int comp_start, float width, float height);
    void DrawSearchBarButtons(const SearchBarRenderState& sb, const SearchBarLayout& sbl);
    // ジェスチャー/トーストの背景パネル。テーマに応じた単色ブラシを alpha で塗る。
    void FillOverlayPanel(const D2D1_RECT_F& rect, float corner, float alpha);
    // DrawText は呼び出しごとに内部でテキストレイアウトを作るため、中央揃え書式のレイアウトを幅/高さ 0 で
    // 作って矩形中心に描く。矩形サイズに依存しないので、文字列と書式が同じ間は text_layouts_ で使い回せる。
    // box 指定時 (固定サイズのオーバーレイ) はその大きさで作って矩形左上に描く。
    // fmt または brush が無い場合は何もしない。
    void DrawCenteredText(std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box = {});

    D2DRenderBackend backend_;
    ID2D1DeviceContext* rt() const noexcept
    {
        return backend_.GetRenderTarget();
    }

    FixedBrushArray brushes_;

    ID2D1SolidColorBrush* Brush(BrushId id) const noexcept
    {
        return brushes_[std::to_underlying(id)].Get();
    }

    void RecreateBrushes();
    void InvalidateBrushes() noexcept;
    // 候補のうち実在するフォントに決める。テーマ間で共通なので Init で 1 回だけ解決し、
    // 以後は AdoptThemeKeepingFonts で引き継ぐ。
    void ResolveThemeFonts();
    void AdoptThemeKeepingFonts(Theme theme);
    void RecreatePaneFormats();

    FrameResult EndFrame();
    bool RecreateDevice();
    FrameResult RecreateDeviceForFrame()
    {
        return RecreateDevice() ? FrameResult::RedrawNow : FrameResult::RetryLater;
    }

    struct TextFormats {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> icon_font;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> copy_btn_icon;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> list_number;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> placeholder_text;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> titlebar_text;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> titlebar_icon;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> pane_icon;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> pane_item;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> pane_header;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> nav_button;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> gesture_overlay;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> toast_text;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> search_input;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> search_count;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> search_icon;
    };
    TextFormats fmt_;

    // CommandGenerator と共有する。
    TextLayoutCache text_layouts_;
    // タイトルバーとサイドペインの描画コマンド。本文 (cmd_generator_) より先に実行する。
    // 最初のフレームで文字を出す部分だけをコマンドにし、デバイス作成前に組み立てられるようにしている。
    // 起動直後には出ないオーバーレイ (トースト・検索バー等) は即時描画のまま。
    DrawCommandList chrome_cmds_;
    // 目次項目の UTF-16。描いた項目だけ作り、ForgetDocumentText と最小化で捨てる。
    std::pmr::vector<std::pmr::wstring> toc_titles_;

    // 検索バーの入力欄のレイアウト。height は定数なのでキーに含めない。
    struct SearchInputLayout {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        std::pmr::wstring text; // IME の合成中テキストを挿入した表示テキスト
        float width = -1.0f;
        DWRITE_TEXT_RANGE underline{}; // IME の合成範囲
    };
    SearchInputLayout search_input_;
    std::pmr::wstring search_text_scratch_;
    // デバイスに依存しない PBGRA 変換済みのアイコン。デバイスロスト後もここから作り直す。
    Microsoft::WRL::ComPtr<IWICBitmap> app_icon_wic_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> app_icon_bitmap_;
    void DecodeAppIcon();
    void LoadAppIconBitmap();
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> gesture_stroke_style_;

    Theme theme_;
    DWriteTextMeasurer measurer_;
    LayoutEngine layout_;
    CommandGenerator cmd_generator_;
    std::move_only_function<void(ID2D1RenderTarget*)> on_device_lost_;
};
