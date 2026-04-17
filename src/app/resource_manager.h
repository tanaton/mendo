#pragma once
#include "flat_map.h"
#include <string>
#include <functional>
#include <windows.h>

class Document;
class LayoutCache;
class ViewportManager;
class ImageLoader;
class MermaidRenderer;
class ThemeService;
class Renderer;

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
        // レイアウト再計算
        std::move_only_function<void()> recompute_layout;           // RecomputeAfterDiagram
        std::move_only_function<void()> recompute_layout_anchored;  // anchor付き: 保存→再計算→復元→Invalidate
    };

    static constexpr UINT_PTR TIMER_MERMAID_BATCH = 10;
    static constexpr UINT_PTR TIMER_BITMAP_MANAGE = 11;
    // ビューポート外リソース解放のバッファ倍率
    static constexpr float EVICT_BUFFER_SCREENS = 5.0f;
    static constexpr float PREFETCH_BUFFER_SCREENS = 3.0f;
    // バッチ処理の時間予算（マイクロ秒）
    static constexpr int BATCH_TIME_BUDGET_US = 6000;

    ResourceManager() = default;
    void Init(Document& doc, LayoutCache& cache, ViewportManager& viewport,
              ImageLoader& image_loader, MermaidRenderer& mermaid,
              ThemeService& theme_service, Renderer& renderer,
              Callbacks cb);

    // --- 画像リソース ---
    int ApplyCachedImages();
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
    void ClearResolvedPaths() noexcept { resolved_image_paths_.clear(); }

private:
    void InvalidateMermaidForWidthChange(float content_width);

    Document* doc_ = nullptr;
    LayoutCache* cache_ = nullptr;
    ViewportManager* viewport_ = nullptr;
    ImageLoader* image_loader_ = nullptr;
    MermaidRenderer* mermaid_ = nullptr;
    ThemeService* theme_service_ = nullptr;
    Renderer* renderer_ = nullptr;
    Callbacks cb_;

    float last_mermaid_content_width_ = 0.0f;
    // バッチ/フラッシュ中の OnMermaidRenderComplete 再入ガード。
    // 同期的にレンダー完了が連鎖した場合に recompute_layout_anchored が毎回走るのを抑止する。
    bool mermaid_batch_loading_ = false;
    size_t mermaid_batch_next_ = 0;
    FlatMap<size_t, std::wstring> resolved_image_paths_;
    bool pending_flush_ = false;
};
