#pragma once
#include "app_constants.h"
#include <unordered_map>
#include <string>
#include <chrono>
#include <windows.h>

class Document;
class LayoutCache;
class ViewportManager;
class ImageLoader;
class IMermaidRenderer;
class ThemeService;
struct Theme;

// 画像・Mermaidリソースのライフサイクル管理。
// Appから画像読み込み、Mermaidバッチ処理、ビットマップ解放の責務を分離する。
//
// Cb は以下のメソッドを提供する duck type。
//   void invalidate();
//   void set_timer(app_timer::Id, UINT);
//   void kill_timer(app_timer::Id);
//   float get_content_width();
//   float get_viewport_height();
//   float get_indent_width();
//   void recompute_layout();
//   void recompute_layout_anchored();
template <class Cb>
class ResourceManagerT {
public:
    static constexpr float EVICT_BUFFER_SCREENS = 5.0f;
    static constexpr float PREFETCH_BUFFER_SCREENS = 3.0f;
    static constexpr int BATCH_TIME_BUDGET_US = 6000;

    ResourceManagerT() = default;
    void Init(Document& doc, LayoutCache& cache, ViewportManager& viewport,
              ImageLoader& image_loader, IMermaidRenderer& mermaid,
              ThemeService& theme_service,
              const Theme& theme,
              Cb cb);

    // respect_viewport=true: 可視範囲のみ走査し、未キャッシュは非同期ロード起動（通常描画用）。
    // respect_viewport=false: 全画像を走査、未キャッシュは無視（リロード時のスクロール計算前用）。
    int ApplyCachedImages(bool respect_viewport = true);
    int ApplyCachedImagesForReload()
    {
        return ApplyCachedImages(false);
    }
    void LoadImages();
    void OnAppImageLoaded();
    void OnImageLoadComplete();

    int RequestMermaidRenders();
    void OnMermaidRenderComplete();
    void CancelMermaidBatch();
    void ScheduleMermaidBatch();
    void ProcessMermaidBatch();

    void EvictOffscreenBitmaps();
    void FlushPendingResources();
    void ScheduleBitmapManage();
    void OnBitmapManageTimer();

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
    const Theme* theme_ = nullptr;
    Cb cb_{};

    float last_mermaid_content_width_ = 0.0f;
    bool mermaid_batch_loading_ = false;
    size_t mermaid_batch_next_ = 0;
    std::unordered_map<size_t, std::wstring> resolved_image_paths_;
    bool pending_flush_ = false;
    std::chrono::steady_clock::time_point last_flush_time_{};
};
