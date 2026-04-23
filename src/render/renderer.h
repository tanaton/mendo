#pragma once
#include "render_params.h"
#include "theme.h"
#include "layout.h"
#include "dwrite_measurer.h"
#include "command_generator.h"
#include "command_executor.h"
#include "syntax.h"
#include "d2d_render_backend.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <vector>
#include <array>
#include <memory>
#include <memory_resource>


enum class BrushId : uint8_t {
    Text, Heading, CodeBg, CodeText, Link, Hr,
    BlockquoteBar, BlockquoteText, Selection, TableStripe,
    SyntaxKeyword, SyntaxType, SyntaxString, SyntaxNumber,
    SyntaxComment, SyntaxPreprocessor, SyntaxFunction,
    AlertNote, AlertTip, AlertImportant, AlertWarning, AlertCaution,
    TitleBarBg, TitleBarText, TitleBarButtonHover, TitleBarButtonActive,
    TitleBarCloseRed, TitleBarCloseWhite,
    PaneBg, Splitter, PaneItemHover, PaneItemActive,
    ScrollbarThumb, Overlay,
    SearchBarBg, SearchBarBorder, SearchInputBg, SearchInputText,
    SearchHighlight, SearchHighlightCurrent, SearchNoMatchBg,
    Count
};

class Renderer {
public:
    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height) noexcept;
    void Render(const RenderParams& params);
    void SetDpi(float dpi) noexcept;
    void DrawLoading(float angle,
        const PaneRect& md_pane_rect,
        const SidePaneState& side_panes,
        const TitleBarRenderState& titlebar,
        const GestureRenderState& gesture = {},
        const ToastRenderState& toast = {}
    );

    ID2D1RenderTarget* GetRenderTarget() const noexcept { return backend_.GetRenderTarget(); }
    ID2D1Factory* GetD2DFactory() const noexcept { return backend_.GetD2DFactory(); }
    IDWriteFactory* GetDWriteFactory() const noexcept { return backend_.GetDWriteFactory(); }
    IWICImagingFactory* GetWICFactory() const noexcept { return backend_.GetWICFactory(); }
    constexpr LayoutEngine& GetLayout() noexcept { return layout_; }
    constexpr const Theme& GetTheme() const noexcept { return theme_; }
    void SetTheme(const Theme& theme);
    // base theme (zoom=1.0) から再構築して現在 zoom を適用する。
    // 累積適用による誤差蓄積を避けるため、ズーム変更経路はこの関数に統一する。
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);

    // LayoutEngineのテーマを更新しフォーマットを再作成する。
    void UpdateLayoutTheme();

    // デバイスロスト後にD2Dレンダーターゲットが再作成された際に呼び出されるコールバックを設定。
    // コールバックには新しいレンダーターゲットのポインタが渡される。
    void SetDeviceLostCallback(std::move_only_function<void(ID2D1RenderTarget*)> cb) { on_device_lost_ = std::move(cb); }

    int HitTestSearchInput(std::wstring_view query, float local_x, float max_width) const;
    void SetSearchMatches(const std::pmr::vector<SearchMatch>* matches, int current_index) noexcept
    {
        cmd_generator_.SetSearchMatches(matches, current_index);
    }

    constexpr void InvalidateFilePaneCache() noexcept { file_pane_cache_.dirty = true; }
    constexpr void InvalidateTocPaneCache() noexcept { toc_pane_cache_.dirty = true; }

    // ファイル切替時にヒットテストバッファ等を縮小する。
    // 初期容量は次ファイルの描画 hot path で再拡大されないよう事前確保する。
    void ShrinkBuffers() {
        hit_test_buffer_.shrink_to_fit();
        hit_test_buffer_.reserve(HIT_TEST_METRICS_INITIAL_CAPACITY);
        cmd_generator_.ShrinkBuffers();
    }

    // 描画前パス: 可視ノードに描画エフェクト（シンタックスハイライト、リンク色）を適用。
    // Render() の前に呼ぶことで、RenderParams を const にできる。
    void PrepareVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
        float scroll_y, float md_pane_height);

private:
    void ApplyVisibleEffects(std::pmr::vector<Node>& nodes, LayoutCache& cache,
        int first_visible, float viewport_top, float viewport_bottom);

    void DrawSidePanes(const SidePaneState& sp);
    void DrawTitleBar(const TitleBarRenderState& tb);
    void DrawMdScrollbar(const PaneRect& md_pane_rect, float scroll_y, float total_content_height, bool has_dirty_nodes);
    void DrawFileExplorer(const std::pmr::vector<FileEntry>& entries, const PaneRect& rect, const ScrollState& scroll, int hovered_index, bool close_hovered, bool refresh_hovered);
    void DrawToc(const std::pmr::vector<TocEntry>& entries, const std::pmr::vector<Node>& nodes, const PaneRect& rect, const ScrollState& scroll, int hovered_index, bool close_hovered, int active_index);
    void DrawSplitter(float x, float top, float bottom);
    void DrawNavOverlay(const PaneRect& md_pane_rect, bool can_back, bool can_forward, int hovered);  // 0=なし, 1=戻る, 2=進む
    void DrawGestureTrail(const std::pmr::deque<GesturePoint>& points);
    void DrawGestureOverlay(int direction, float alpha, const PaneRect& md_pane_rect);
    void DrawToastOverlay(const ToastRenderState& toast, const PaneRect& md_pane_rect);
    void DrawSearchBar(const SearchBarRenderState& sb, const PaneRect& md_pane_rect);

    D2DRenderBackend backend_;
    // 簡易アクセサ（600行の描画コード内で冗長なbackend_.Get...を避けるため）
    ID2D1DeviceContext* rt() const noexcept { return backend_.GetRenderTarget(); }
    ID2D1Factory* d2d() const noexcept { return backend_.GetD2DFactory(); }

    std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, static_cast<size_t>(BrushId::Count)> brushes_;
    ID2D1SolidColorBrush* Brush(BrushId id) const noexcept
    {
        return brushes_[static_cast<size_t>(id)].Get();
    }

    ID2D1SolidColorBrush* GetSyntaxBrush(SyntaxTokenType type) const noexcept;
    void ApplyTableEffects(Node& node, NodeLayoutEntry& entry, float viewport_top, float viewport_bottom);
    void ApplyNodeEffects(Node& node, NodeLayoutEntry& entry, float viewport_top = -1.0f, float viewport_bottom = -1.0f);
    void RecreateBrushes();
    void InvalidateBrushes() noexcept;
    void RecreatePaneFormats();
    Microsoft::WRL::ComPtr<IDWriteTextFormat> CreatePaneFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, const wchar_t* locale);
    bool CheckEndDraw();
    bool RecreateRenderTarget();

    // ApplyNodeEffectsでインラインコード背景を計算するためのヒットテストバッファ。
    std::pmr::vector<DWRITE_HIT_TEST_METRICS> hit_test_buffer_;

    // UI テキストフォーマットをグループ化した構造体
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

    Microsoft::WRL::ComPtr<IDWriteTextLayout> nav_back_layout_;   // ◀ のキャッシュ済みレイアウト
    Microsoft::WRL::ComPtr<IDWriteTextLayout> nav_forward_layout_; // ▶ のキャッシュ済みレイアウト
    Microsoft::WRL::ComPtr<IDWriteTextLayout> gesture_back_layout_;    // "← 戻る" のキャッシュ済みレイアウト
    Microsoft::WRL::ComPtr<IDWriteTextLayout> gesture_forward_layout_;  // "→ 進む" のキャッシュ済みレイアウト
    Microsoft::WRL::ComPtr<IDWriteTextLayout> cached_toast_layout_;
    std::pmr::wstring cached_toast_text_;

    // 検索バーの入力テキストレイアウトキャッシュ。
    // キー: (query, ime_composition, caret_pos, input 幅)
    // 値:   cached_search_layout_ と合成済み表示テキスト cached_search_text_。
    // キャレット点滅や同一入力継続フレームで CreateTextLayout と
    // 表示テキスト合成の双方を回避する。入力 height は定数なのでキーに含めない。
    mutable Microsoft::WRL::ComPtr<IDWriteTextLayout> cached_search_layout_;
    mutable std::pmr::wstring cached_search_text_;       // 合成後の表示テキスト (IME 未使用時は query と同一)
    mutable std::pmr::wstring cached_search_query_;      // 直近フレームの sb.query
    mutable std::pmr::wstring cached_search_ime_comp_;   // 直近フレームの sb.ime_composition
    mutable int cached_search_caret_pos_ = -1;           // IME 合成時の挿入位置（無いとき -1）
    mutable float cached_search_width_ = -1.0f;
    mutable bool cached_search_has_underline_ = false;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> app_icon_bitmap_;
    void LoadAppIconBitmap();
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> gesture_stroke_style_;

    PaneCache file_pane_cache_;
    PaneCache toc_pane_cache_;

    // ApplyVisibleEffects スキップ判定用キャッシュ
    uint32_t last_effects_gen_ = UINT32_MAX;
    int last_effects_first_ = -1;
    float last_effects_bottom_ = -1.0f;

    Theme theme_;
    DWriteTextMeasurer measurer_;
    LayoutEngine layout_;
    CommandGenerator cmd_generator_;
    CommandExecutor cmd_executor_;
    std::move_only_function<void(ID2D1RenderTarget*)> on_device_lost_;
};
