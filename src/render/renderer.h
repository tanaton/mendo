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

struct GestureRenderState {
    const std::pmr::deque<GesturePoint>* trail_points = nullptr;
    int direction = 0;   // -1=Left(戻る), 1=Right(進む)
    float overlay_alpha = 0.0f;
    bool trail_active = false;
    bool overlay_visible = false;
};

struct ToastRenderState {
    bool visible = false;
    float alpha = 0.0f;
    std::wstring_view message;
};

// タイトルバー描画パラメータ。
struct TitleBarRenderState {
    // --- 8バイトアライメント ---
    std::wstring_view title_text;
    // --- 4バイトアライメント (D2D1_RECT_F = 4×float) ---
    D2D1_RECT_F open_file_btn_rect{};
    D2D1_RECT_F help_btn_rect{};
    D2D1_RECT_F theme_btn_rect{};
    D2D1_RECT_F search_btn_rect{};
    D2D1_RECT_F file_btn_rect{};
    D2D1_RECT_F toc_btn_rect{};
    D2D1_RECT_F minimize_btn_rect{};
    D2D1_RECT_F maximize_btn_rect{};
    D2D1_RECT_F close_btn_rect{};
    D2D1_RECT_F icon_rect{};
    D2D1_RECT_F title_text_rect{};
    float height = 0.0f;
    float window_width = 0.0f;
    // --- 1バイトアライメント ---
    bool open_file_btn_hovered = false;
    bool help_btn_hovered = false;
    bool theme_btn_hovered = false;
    bool is_dark_mode = false;
    bool search_btn_hovered = false;
    bool search_active = false;
    bool file_btn_hovered = false;
    bool file_pane_visible = false;
    bool toc_btn_hovered = false;
    bool toc_pane_visible = false;
    bool minimize_btn_hovered = false;
    bool maximize_btn_hovered = false;
    bool is_maximized = false;
    bool close_btn_hovered = false;
    bool window_active = true;
};

// ���イドペイン描画パラメータを一つの構造体にまとめたもの。
struct SidePaneState {
    // --- 8バイトアライメント (参照 = ポインタ) ---
    const PaneRect& file_pane_rect;
    const PaneRect& toc_pane_rect;
    const std::pmr::vector<FileEntry>& file_entries;
    const ScrollState& file_scroll;
    const std::pmr::vector<TocEntry>& toc_entries;
    const std::pmr::vector<Node>& nodes; // TocEntryからのテキスト参照用
    const ScrollState& toc_scroll;
    // --- 4バイトアライメント ---
    int hovered_file_index;
    int hovered_toc_index;
    int active_toc_index;
    // --- 1バイトアライメント ---
    bool show_file_pane;
    bool show_toc_pane;
    bool file_close_hovered;
    bool file_refresh_hovered;
    bool toc_close_hovered;
};

// ペインビットマップキャッシュ — サイドペインはオフスクリーンビットマップに描画され、
// 内容が変更された場合のみ再描画される。
struct PaneCache {
    Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> bitmap_rt;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> cached_bitmap; // GetBitmap() の毎フレーム呼び出しを回避
    bool dirty = true;
    float cached_width = 0;
    float cached_height = 0;

    constexpr void Invalidate() noexcept { dirty = true; }
    void Reset() noexcept { bitmap_rt.Reset(); cached_bitmap.Reset(); dirty = true; cached_width = 0; cached_height = 0; }
};

// 検索バー描画パラメータ
struct SearchBarRenderState {
    // --- 8バイトアライメント ---
    std::wstring_view query;
    std::wstring_view ime_composition; // IME変換中のコンポジション文字列
    // --- 4バイトアライメント ---
    int current_match = -1;    // 0-based、-1 = マッチなし
    int total_matches = 0;
    int caret_pos = -1;         // キャレット位置（-1 = テキスト末尾）
    int selection_start = -1;   // 選択開始位置（caret_posと異なる場合、選択範囲あり）
    // --- 1バイトアライメント ---
    bool visible = false;
    bool has_focus = false;
    bool caret_visible = false; // キャレット（点滅制御）
    // チェックボックス状態
    bool case_sensitive = false;
    bool highlight_enabled = true;
    // 検索バー内のボタンホバー状態
    bool up_btn_hovered = false;
    bool down_btn_hovered = false;
    bool close_btn_hovered = false;
    bool case_btn_hovered = false;
    bool highlight_btn_hovered = false;
};

// Renderer::Render に渡す全パラメータをまとめた構造体。
struct RenderParams {
    // --- 8バイ���アライメント (参照 = ポインタ) ---
    std::pmr::vector<Node>& nodes;
    LayoutCache& cache;
    const TextSelection& selection;
    const PaneRect& md_pane_rect;
    const SidePaneState& side_panes;
    const TitleBarRenderState& titlebar;
    const GestureRenderState& gesture;
    const ToastRenderState& toast;
    const SearchBarRenderState& search_bar;
    // --- 4バイトアライメント ---
    float scroll_y = 0.0f;
    float total_content_height = 0.0f;
    int nav_hovered = 0;
    int hovered_copy_node = -1;
    int hovered_save_node = -1;
    // --- 1��イトアライメント ---
    bool can_go_back = false;
    bool can_go_forward = false;
    bool has_dirty_nodes = false;
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
    void ApplyZoom(float new_zoom);
    void ApplyZoomFromBase(const Theme& base_theme, float new_zoom);

    // LayoutEngineのテーマを更新しフォーマットを再作成する。
    void UpdateLayoutTheme();

    // デバイスロスト後にD2Dレンダーターゲットが再作成された際に呼び出されるコールバックを設定。
    // コールバックには新しいレンダーターゲットのポインタが渡される。
    void SetDeviceLostCallback(std::function<void(ID2D1RenderTarget*)> cb) { on_device_lost_ = std::move(cb); }

    int HitTestSearchInput(std::wstring_view query, float local_x, float max_width) const;
    void SetSearchMatches(const std::pmr::vector<SearchMatch>* matches, int current_index) noexcept
    {
        cmd_generator_.SetSearchMatches(matches, current_index);
    }

    constexpr void InvalidateFilePaneCache() noexcept { file_pane_cache_.dirty = true; }
    constexpr void InvalidateTocPaneCache() noexcept { toc_pane_cache_.dirty = true; }

    // ファイル切替時にヒットテストバッファ等を縮小する
    void ShrinkBuffers() { hit_test_buffer_.shrink_to_fit(); cmd_generator_.ShrinkBuffers(); }

private:
    // 描画前パス: レイアウトに描画エフェクト（シンタックスハイライト、リンク色）を適用。
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
    std::function<void(ID2D1RenderTarget*)> on_device_lost_;
};
