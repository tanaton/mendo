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
#include <vector>
#include <array>
#include <memory>
#include <memory_resource>
#include <utility>


class Renderer {
public:
    bool Init(HWND hwnd);
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
    constexpr const Theme& GetTheme() const noexcept
    {
        return theme_;
    }
    void SetTheme(const Theme& theme);
    // base theme (zoom=1.0) から再構築して現在 zoom を適用する。
    // 累積適用による誤差蓄積を避けるため、ズーム変更経路はこの関数に統一する。
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);

    void UpdateLayoutTheme();

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
        pane_caches_[static_cast<size_t>(t)].dirty = true;
    }
    constexpr void InvalidateAllSidePaneCaches() noexcept
    {
        for (auto& c : pane_caches_) {
            c.dirty = true;
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
        return pane_caches_[static_cast<size_t>(t)];
    }

    void ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache, int first_visible, float viewport_top, float viewport_bottom);

    void DrawSidePanes(const SidePaneState& sp);
    void DrawTitleBar(const TitleBarRenderState& tb);
    void DrawMdScrollbar(const PaneRect& md_pane_rect, float scroll_y, float total_content_height, bool has_dirty_nodes);
    void DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const PaneRect& rect, const ScrollState& scroll, int hovered_index, bool close_hovered, bool refresh_hovered);
    void DrawToc(const std::pmr::vector<TocEntry>& entries, const std::pmr::vector<Node>& nodes, const PaneRect& rect, const ScrollState& scroll, int hovered_index, bool close_hovered, int active_index);
    void DrawSplitter(float x, float top, float bottom);
    // hovered: 0=なし, 1=戻る, 2=進む
    void DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, int hovered);
    void DrawGestureTrail(const std::pmr::deque<GesturePoint>& points);
    void DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect);
    void DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect);
    void DrawSearchBar(const SearchBarRenderState& sb, const PaneRect& md_pane_rect);

    D2DRenderBackend backend_;
    ID2D1DeviceContext* rt() const noexcept
    {
        return backend_.GetRenderTarget();
    }
    ID2D1Factory* d2d() const noexcept
    {
        return backend_.GetD2DFactory();
    }

    std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, std::to_underlying(BrushId::Count)> brushes_;
    FixedBrushArray fixed_brushes_cache_{};

    ID2D1SolidColorBrush* Brush(BrushId id) const noexcept
    {
        return brushes_[std::to_underlying(id)].Get();
    }

    ID2D1SolidColorBrush* GetSyntaxBrush(SyntaxTokenType type) const noexcept;
    void ApplyTableEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top, float viewport_bottom);
    void ApplyNodeEffects(Node& node, NodeLayoutEntry& entry, float entry_text_top, float viewport_top = -1.0f, float viewport_bottom = -1.0f);
    void RecreateBrushes();
    void InvalidateBrushes() noexcept;
    void ResolveThemeFonts();
    void RecreatePaneFormats();
    Microsoft::WRL::ComPtr<IDWriteTextFormat> CreatePaneFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, const wchar_t* locale);
    bool CheckEndDraw();
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

    Microsoft::WRL::ComPtr<IDWriteTextLayout> nav_back_layout_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> nav_forward_layout_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> gesture_back_layout_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> gesture_forward_layout_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> cached_toast_layout_;
    std::pmr::wstring cached_toast_text_{ GetThreadLocalPoolResource() };

    // 検索バーの入力テキストレイアウトキャッシュ。
    // キー: (query, ime_comp, caret_pos, width) 入力 height は定数なのでキーに含めない。
    // キャレット点滅や同一入力継続フレームで CreateTextLayout と
    // 表示テキスト合成の双方を回避する。
    struct SearchLayoutCache {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        std::pmr::wstring text{ GetThreadLocalPoolResource() };     // 合成後の表示テキスト (IME 未使用時は query と同一)
        std::pmr::wstring query{ GetThreadLocalPoolResource() };    // 直近フレームの sb.query
        std::pmr::wstring ime_comp{ GetThreadLocalPoolResource() }; // 直近フレームの sb.ime_composition
        int caret_pos = -1;                                         // IME 合成時の挿入位置（無いとき -1）
        float width = -1.0f;
        bool has_underline = false;
        // キャレット x 位置のフレーム間キャッシュ。点滅フレームのみ caret_visible が変わる
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
