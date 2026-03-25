#pragma once
#include "types.h"
#include "layout_cache.h"
#include "theme.h"
#include "layout.h"
#include "dwrite_measurer.h"
#include "command_generator.h"
#include "command_executor.h"
#include "syntax.h"
#include "pane.h"
#include "titlebar.h"
#include "file_explorer.h"
#include "toc.h"
#include "mouse_gesture.h"
#include "d2d_render_backend.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <vector>
#include <array>
#include <memory>
#include <memory_resource>

using Microsoft::WRL::ComPtr;

enum class BrushId : uint8_t {
    Text, Heading, CodeBg, CodeText, Link, Hr,
    BlockquoteBar, BlockquoteText, Selection, TableStripe,
    SyntaxKeyword, SyntaxType, SyntaxString, SyntaxNumber,
    SyntaxComment, SyntaxPreprocessor, SyntaxFunction,
    AlertNote, AlertTip, AlertImportant, AlertWarning, AlertCaution,
    TitleBarBg, TitleBarText, TitleBarButtonHover, TitleBarButtonActive,
    PaneBg, Splitter, PaneItemHover, PaneItemActive,
    ScrollbarThumb, Overlay,
    Count
};

struct GestureRenderState {
    bool trail_active = false;
    const std::pmr::deque<GesturePoint>* trail_points = nullptr;
    bool overlay_visible = false;
    int direction = 0;   // -1=Left(戻る), 1=Right(進む)
    float overlay_alpha = 0.0f;
};

// タイトルバー描画パラメータ。
struct TitleBarRenderState {
    float height = 0.0f;
    float window_width = 0.0f;
    D2D1_RECT_F file_btn_rect{};
    bool file_btn_hovered = false;
    bool file_pane_visible = false;
    D2D1_RECT_F toc_btn_rect{};
    bool toc_btn_hovered = false;
    bool toc_pane_visible = false;
    D2D1_RECT_F minimize_btn_rect{};
    bool minimize_btn_hovered = false;
    D2D1_RECT_F maximize_btn_rect{};
    bool maximize_btn_hovered = false;
    bool is_maximized = false;
    D2D1_RECT_F close_btn_rect{};
    bool close_btn_hovered = false;
    D2D1_RECT_F title_text_rect{};
    std::wstring_view title_text;
    bool window_active = true;
};

// サイドペイン描画パラメータを一つの構造体にまとめたもの。
struct SidePaneState {
    const PaneRect& file_pane_rect;
    const PaneRect& toc_pane_rect;
    const std::pmr::vector<FileEntry>& file_entries;
    const ScrollState& file_scroll;
    int hovered_file_index;
    const std::pmr::vector<TocEntry>& toc_entries;
    const ScrollState& toc_scroll;
    int hovered_toc_index;
    bool show_file_pane;
    bool show_toc_pane;
};

class Renderer {
public:
    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);
    void Render(std::pmr::vector<Node>& nodes, LayoutCache& cache, float scroll_y,
        const TextSelection& selection,
        const PaneRect& md_pane_rect,
        const SidePaneState& side_panes,
        const TitleBarRenderState& titlebar,
        bool can_go_back = false, bool can_go_forward = false,
        int nav_hovered = 0,
        int hovered_copy_node = -1,
        const GestureRenderState& gesture = {});
    void SetDpi(float dpi);
    void DrawLoading(float angle,
        const PaneRect& md_pane_rect,
        const SidePaneState& side_panes,
        const TitleBarRenderState& titlebar,
        const GestureRenderState& gesture = {});

    ID2D1HwndRenderTarget* GetRenderTarget() const noexcept { return backend_.GetRenderTarget(); }
    constexpr LayoutEngine& GetLayout() noexcept { return layout_; }
    constexpr const Theme& GetTheme() const noexcept { return theme_; }
    void SetTheme(const Theme& theme);
    void ApplyZoom(float new_zoom);
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);

    // LayoutEngineのテーマを更新しフォーマットを再作成する。
    void UpdateLayoutTheme();

    // ビューポート幅からマージンを差し引いてLayoutEngineでノードをレイアウトする。
    void LayoutAllNodes(std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);

    constexpr Theme& GetThemeMut() noexcept { return theme_; }

    // デバイスロスト後にD2Dレンダーターゲットが再作成された際に呼び出されるコールバックを設定。
    // コールバックには新しいレンダーターゲットのポインタが渡される。
    void SetDeviceLostCallback(std::function<void(ID2D1RenderTarget*)> cb) { on_device_lost_ = std::move(cb); }

    constexpr void InvalidateFilePaneCache() noexcept { file_pane_cache_.dirty = true; }
    constexpr void InvalidateTocPaneCache() noexcept { toc_pane_cache_.dirty = true; }

    // ファイル切替時にヒットテストバッファ等を縮小する
    void ShrinkBuffers() { hit_test_buffer_.shrink_to_fit(); cmd_generator_.ShrinkBuffers(); }

private:
    // 描画前パス: レイアウトに描画エフェクト（シンタックスハイライト、リンク色）を適用。
    void ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
        int first_visible, float viewport_bottom);

    void DrawTitleBar(const TitleBarRenderState& tb);
    void DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const PaneRect& rect,
        const ScrollState& scroll, int hovered_index);
    void DrawToc(const std::pmr::vector<TocEntry>& entries, const PaneRect& rect,
        const ScrollState& scroll, int hovered_index);
    void DrawSplitter(float x, float top, float height);
    void DrawNavOverlay(const PaneRect& md_pane_rect,
        bool can_back, bool can_forward,
        int hovered);  // 0=なし, 1=戻る, 2=進む
    void DrawGestureTrail(const std::pmr::deque<GesturePoint>& points);
    void DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect);

    D2DRenderBackend backend_;
    // 簡易アクセサ（600行の描画コード内で冗長なbackend_.Get...を避けるため）
    ID2D1HwndRenderTarget* rt() const noexcept { return backend_.GetRenderTarget(); }
    ID2D1Factory* d2d() const noexcept { return backend_.GetD2DFactory(); }

    std::array<ComPtr<ID2D1SolidColorBrush>, static_cast<size_t>(BrushId::Count)> brushes_;
    ID2D1SolidColorBrush* Brush(BrushId id) const noexcept {
        return brushes_[static_cast<size_t>(id)].Get();
    }

    ID2D1SolidColorBrush* GetSyntaxBrush(SyntaxTokenType type) const;
    void ApplyNodeEffects(const Node& node, NodeLayoutEntry& entry);
    void RecreateBrushes();
    void RecreatePaneFormats();
    ComPtr<IDWriteTextFormat> CreatePaneFormat(
        const wchar_t* family, DWRITE_FONT_WEIGHT weight,
        float size, const wchar_t* locale);
    bool CheckEndDraw();
    bool RecreateRenderTarget();

    // ApplyNodeEffectsでインラインコード背景を計算するためのヒットテストバッファ。
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    ComPtr<IDWriteTextFormat> icon_font_format_;
    ComPtr<IDWriteTextFormat> fmt_copy_btn_icon_;
    ComPtr<IDWriteTextFormat> fmt_list_number_;
    ComPtr<IDWriteTextFormat> fmt_titlebar_text_;
    ComPtr<IDWriteTextFormat> fmt_titlebar_icon_;
    ComPtr<IDWriteTextFormat> fmt_pane_icon_;
    ComPtr<IDWriteTextFormat> fmt_pane_item_;
    ComPtr<IDWriteTextFormat> fmt_pane_header_;
    ComPtr<IDWriteTextFormat> fmt_nav_button_;
    ComPtr<IDWriteTextLayout> nav_back_layout_;   // ◀ のキャッシュ済みレイアウト
    ComPtr<IDWriteTextLayout> nav_forward_layout_; // ▶ のキャッシュ済みレイアウト
    ComPtr<ID2D1StrokeStyle> gesture_stroke_style_;
    ComPtr<IDWriteTextFormat> fmt_gesture_overlay_;

public:
    // ペインビットマップキャッシュ — サイドペインはオフスクリーンビットマップに描画され、
    // 内容が変更された場合のみ再描画される。
    struct PaneCache {
        ComPtr<ID2D1BitmapRenderTarget> bitmap_rt;
        ComPtr<ID2D1Bitmap> cached_bitmap; // GetBitmap() の毎フレーム呼び出しを回避
        bool dirty = true;
        float cached_width = 0;
        float cached_height = 0;

        constexpr void Invalidate() noexcept { dirty = true; }
        void Reset() noexcept { bitmap_rt.Reset(); cached_bitmap.Reset(); dirty = true; cached_width = 0; cached_height = 0; }
    };
private:
    PaneCache file_pane_cache_;
    PaneCache toc_pane_cache_;

    Theme theme_;
    DWriteTextMeasurer measurer_;
    LayoutEngine layout_;
    CommandGenerator cmd_generator_;
    CommandExecutor cmd_executor_;
    std::function<void(ID2D1RenderTarget*)> on_device_lost_;
};
