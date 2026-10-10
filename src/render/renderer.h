#pragma once
#include "brush_id.h"
#include "render_params.h"
#include "theme.h"
#include "layout.h"
#include "dwrite_measurer.h"
#include "command_generator.h"
#include "command_executor.h"
#include "syntax.h"
#include "d2d_render_backend.h"
#include "memory_resource.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SidePaneDrawContext;

class Renderer {
public:
    bool Init(HWND hwnd, const Theme& theme);
    void Resize(UINT width, UINT height) noexcept;
    void Render(const RenderParams& params);
    void SetDpi(float dpi) noexcept;
    void DrawLoading(
        float angle,
        const PaneRect& md_pane_rect,
        const SidePaneState& side_panes,
        const TitleBarRenderState& titlebar,
        const GestureRenderState& gesture = {},
        const ToastRenderState& toast = {});

    ID2D1RenderTarget* GetRenderTarget() const noexcept
    {
        return backend_.GetRenderTarget();
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
    void SetLayoutScheduler(TaskScheduler* scheduler) noexcept
    {
        layout_.SetLayoutScheduler(scheduler);
        measurer_.SetScheduler(scheduler);
    }
    constexpr const Theme& GetTheme() const noexcept
    {
        return theme_;
    }
    // ライト/ダーク切替用。寸法とフォントは共通なのでテキストレイアウトを維持し、色だけ差し替える。
    void SetTheme(const Theme& theme);
    // base theme (zoom=1.0) から再構築して現在 zoom を適用する。
    // 累積適用による誤差蓄積を避けるため、ズーム変更経路はこの関数に統一する。
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);

    void SetDeviceLostCallback(std::move_only_function<void(ID2D1RenderTarget*)> cb)
    {
        on_device_lost_ = std::move(cb);
    }

    int HitTestSearchInput(std::wstring_view query, float local_x, float max_width) const;
    void SetSearchMatches(const std::pmr::vector<SearchMatch>* matches, int current_index, uint32_t generation) noexcept
    {
        cmd_generator_.SetSearchMatches(matches, current_index, generation);
    }

    constexpr void InvalidateSidePaneCache(PaneTarget t) noexcept
    {
        SidePaneCache(t).Invalidate();
    }
    constexpr void InvalidateAllSidePaneCaches() noexcept
    {
        for (auto& c : pane_caches_) {
            c.Invalidate();
        }
    }

    // ファイル切替時にヒットテストバッファ等を縮小する。
    // 初期容量は次ファイルの描画 hot path で再拡大されないよう事前確保する。
    void ShrinkBuffers()
    {
        hit_test_buffer_.shrink_to_fit();
        hit_test_buffer_.reserve(HIT_TEST_METRICS_INITIAL_CAPACITY);
    }

    // Render() の前に呼ぶこと。RenderParams を const にするための分離。
    void PrepareVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache, float scroll_y, float md_pane_height);

private:
    constexpr PaneCache& SidePaneCache(PaneTarget t) noexcept
    {
        return pane_caches_[std::to_underlying(t)];
    }
    void ResetSidePaneCaches() noexcept;

    // テーマのフォント/寸法変更をレイアウト・テキストフォーマット・コマンド生成へ反映する。
    void ApplyThemeMetrics();

    // フレーム共通の前処理 (デバイスロスト復旧・BeginDraw・タイトルバー・サイドペイン)。
    // 描画できない場合 false を返し、その場合 BeginDraw は呼ばれていない。
    bool BeginFrame(const TitleBarRenderState& titlebar, const SidePaneState& side_panes);
    void DrawLoadingSpinner(float angle, const PaneRect& md_pane_rect);

    void ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache, int first_visible, float viewport_top, float viewport_bottom);

    void DrawSidePanes(const SidePaneState& sp);
    void DrawTitleBar(const TitleBarRenderState& tb);
    void DrawMdScrollbar(const PaneRect& md_pane_rect, float scroll_y, float total_content_height, bool has_dirty_nodes);
    void DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const SidePaneInstance& pane);
    void DrawToc(const std::pmr::vector<TocEntry>& entries, const std::pmr::vector<Node>& nodes, const SidePaneInstance& pane, int active_index);
    void DrawSplitter(float x, float top, float bottom);
    void DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, NavButtonHover hovered);
    void DrawGestureTrail(const std::pmr::deque<GesturePoint>& points);
    void DrawGestureOverlay(int direction, const PaneRect& md_pane_rect);
    void DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect);
    void DrawSearchBar(const SearchBarRenderState& sb, const PaneRect& md_pane_rect);
    // 入力欄のテキスト・選択範囲を描画し、キャレットの x 座標を返す。
    float DrawSearchInputText(const SearchBarRenderState& sb, const SearchBarLayout& sbl);
    // search_cache_ を再利用、またはミス時に作り直した入力欄レイアウトを返す (所有は search_cache_)。
    IDWriteTextLayout* AcquireSearchInputLayout(const SearchBarRenderState& sb, int comp_start, float width, float height, bool& cache_hit);
    void DrawSearchBarButtons(const SearchBarRenderState& sb, const SearchBarLayout& sbl);
    // ジェスチャー/トーストの背景パネル。テーマに応じた単色ブラシを alpha で塗る。
    void FillOverlayPanel(const D2D1_RECT_F& rect, float corner, float alpha);
    // DrawText は呼び出しごとに内部でテキストレイアウトを作るため、中央揃え書式のレイアウトを幅/高さ 0 で
    // 作って矩形中心に描く。矩形サイズに依存しないので、文字列と書式が同じ間は使い回せる。
    // box 指定時 (固定サイズのオーバーレイ) はその大きさで作って矩形左上に描く。
    struct CenteredTextLayout {
        IDWriteTextFormat* format = nullptr;
        std::wstring text;
        D2D1_SIZE_F box{};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    };
    // fmt または brush が無い場合は何もしない。
    void DrawCenteredText(CenteredTextLayout& slot, std::wstring_view text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box = {});
    // 固定文字列用。(書式, 文字列) ごとに icon_layouts_ へ作り置く。
    void DrawIcon(std::wstring_view icon, IDWriteTextFormat* fmt, const D2D1_RECT_F& rect, BrushId brush_id, float alpha, D2D1_SIZE_F box = {});
    SidePaneDrawContext MakeSidePaneContext(PaneTarget target, const SidePaneInstance& pane, size_t item_count, std::wstring_view header_text);

    D2DRenderBackend backend_;
    ID2D1DeviceContext* rt() const noexcept
    {
        return backend_.GetRenderTarget();
    }
    ID2D1Factory* d2d() const noexcept
    {
        return backend_.GetD2DFactory();
    }

    FixedBrushArray brushes_;

    ID2D1SolidColorBrush* Brush(BrushId id) const noexcept
    {
        return brushes_[std::to_underlying(id)].Get();
    }

    ID2D1SolidColorBrush* GetSyntaxBrush(SyntaxTokenType type) const noexcept;
    void ApplyTableEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom);
    // hit_test_buffer_ の先頭 count 件をセルのインラインコード背景として cell_index 昇順を保って追加する。
    void AppendCellInlineCodeBgs(TableLayoutData& tl, uint32_t cell_index, UINT32 count);
    void ApplyNodeEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom);
    void ApplySyntaxEffects(IDWriteTextLayout* layout, const Node& node);
    void RecreateBrushes();
    void InvalidateBrushes() noexcept;
    void ResolveThemeFonts();
    void RecreatePaneFormats();
    Microsoft::WRL::ComPtr<IDWriteTextFormat> CreatePaneFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, const wchar_t* locale);
    void CheckEndDraw();
    bool RecreateRenderTarget();
    // 再描画要求を出した場合 true。
    bool HandleDeviceLost();

    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_{ GetThreadLocalPoolResource() };

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

    std::vector<CenteredTextLayout> icon_layouts_;
    CenteredTextLayout title_layout_;
    CenteredTextLayout search_count_layout_;
    CenteredTextLayout toast_layout_;
    // 目次項目の描画ごとの UTF-16 変換先。項目ごとの確保を避けて再利用する。
    std::pmr::wstring toc_text_scratch_{ GetThreadLocalPoolResource() };

    // 検索バーの入力テキストレイアウトキャッシュ。
    // キー: (query, ime_comp, caret_pos, width) 入力 height は定数なのでキーに含めない。
    // 同一入力が続くフレームで CreateTextLayout と
    // 表示テキスト合成の双方を回避する。
    struct SearchLayoutCache {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        std::pmr::wstring text{ GetThreadLocalPoolResource() };     // 合成後の表示テキスト (IME 未使用時は query と同一)
        std::pmr::wstring query{ GetThreadLocalPoolResource() };    // 直近フレームの sb.query
        std::pmr::wstring ime_comp{ GetThreadLocalPoolResource() }; // 直近フレームの sb.ime_composition
        int caret_pos = -1;                                         // IME 合成時の挿入位置（無いとき -1）
        float width = -1.0f;
        bool has_underline = false;
        // キャレット x 位置のフレーム間キャッシュ。入力が変わらない
        // ケースで HitTestTextPosition の COM 越境呼び出しを省く。
        // 有効性は (layout, effective_pos) 一致で判定する。
        int effective_pos = -2; // -2 = 未確定
        float caret_x = 0.0f;

        void Reset()
        {
            layout.Reset();
            text.clear();
            query.clear();
            ime_comp.clear();
            caret_pos = -1;
            width = -1.0f;
            has_underline = false;
            effective_pos = -2;
            caret_x = 0.0f;
        }
    };
    mutable SearchLayoutCache search_cache_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> app_icon_bitmap_;
    void LoadAppIconBitmap();
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> gesture_stroke_style_;

    PaneCache pane_caches_[2];

    // 直近フレームで ApplyVisibleEffects を実行したキー。(世代, 可視域) が一致すれば再適用を省く。
    struct EffectsKey {
        uint32_t gen = std::numeric_limits<uint32_t>::max();
        int first_visible = -1;
        int viewport_bottom_q = -1;
        bool operator==(const EffectsKey&) const = default;
    };
    EffectsKey last_effects_;

    Theme theme_;
    DWriteTextMeasurer measurer_;
    LayoutEngine layout_;
    CommandGenerator cmd_generator_;
    CommandExecutor cmd_executor_;
    std::move_only_function<void(ID2D1RenderTarget*)> on_device_lost_;
};
