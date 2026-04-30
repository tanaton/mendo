#pragma once
#include "flat_map.h"
#include <string>
#include <functional>
#include <chrono>
#include <windows.h>

class Document;
class LayoutCache;
class ViewportManager;
class ImageLoader;
class IMermaidRenderer;
class ThemeService;

// 画像・Mermaidリソースのライフサイクル管理。
// Appから画像読み込み、Mermaidバッチ処理、ビットマップ解放の責務を分離する。
class ResourceManager {
public:
    struct Callbacks {
        std::move_only_function<void()> invalidate;
        std::move_only_function<void(UINT_PTR, UINT)> set_timer;
        std::move_only_function<void(UINT_PTR)> kill_timer;
        std::move_only_function<float()> get_content_width;
        std::move_only_function<float()> get_viewport_height;
        std::move_only_function<float()> get_indent_width;
        std::move_only_function<void()> recompute_layout;
        std::move_only_function<void()> recompute_layout_anchored;
    };

    static constexpr UINT_PTR TIMER_MERMAID_BATCH = 10;
    static constexpr UINT_PTR TIMER_BITMAP_MANAGE = 11;
    static constexpr float EVICT_BUFFER_SCREENS = 5.0f;
    static constexpr float PREFETCH_BUFFER_SCREENS = 3.0f;
    static constexpr int BATCH_TIME_BUDGET_US = 6000;

    ResourceManager() = default;
    void Init(Document& doc, LayoutCache& cache, ViewportManager& viewport,
              ImageLoader& image_loader, IMermaidRenderer& mermaid,
              ThemeService& theme_service,
              Callbacks cb);

    // --- 画像リソース ---
    // respect_viewport=true: 可視範囲のみ走査し、未キャッシュは非同期ロード起動。通常描画用。
    // respect_viewport=false: 全画像を走査、未キャッシュは無視。リロード時のスクロール計算前用。
    int ApplyCachedImages(bool respect_viewport = true);
    int ApplyCachedImagesForReload()
    {
        return ApplyCachedImages(false);
    }
    void LoadImages();
    void OnAppImageLoaded();
    void OnImageLoadComplete();

    // --- Mermaidリソース ---
    int RequestMermaidRenders();
    void OnMermaidRenderComplete();
    void CancelMermaidBatch();
    void ScheduleMermaidBatch();
    void ProcessMermaidBatch();

    // --- ビットマップ管理 ---
    void EvictOffscreenBitmaps();
    void FlushPendingResources();
    void ScheduleBitmapManage();
    void OnBitmapManageTimer();

    // --- ファイル切替時クリーンアップ ---
    void ClearResolvedPaths() noexcept
    {
        resolved_image_paths_.clear();
    }

private:
    void InvalidateMermaidForWidthChange(float content_width);

    Document* doc_ = nullptr;
    LayoutCache* cache_ = nullptr;
    ViewportManager* viewport_ = nullptr;
    ImageLoader* image_loader_ = nullptr;
    IMermaidRenderer* mermaid_ = nullptr;
    ThemeService* theme_service_ = nullptr;
    Callbacks cb_;

    float last_mermaid_content_width_ = 0.0f;
    bool mermaid_batch_loading_ = false;
    size_t mermaid_batch_next_ = 0;
    FlatMap<size_t, std::wstring> resolved_image_paths_;
    bool pending_flush_ = false;
    std::chrono::steady_clock::time_point last_flush_time_{};
};
