#include "resource_manager.h"
#include "document.h"
#include "layout_cache.h"
#include "viewport_manager.h"
#include "image_loader.h"
#include "mermaid.h"
#include "mermaid_util.h"
#include "theme_service.h"
#include "renderer.h"
#include "layout_service.h"
#include "profiler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

void ResourceManager::Init(Document& doc, LayoutCache& cache, ViewportManager& viewport,
    ImageLoader& image_loader, MermaidRenderer& mermaid,
    ThemeService& theme_service, Renderer& renderer,
    Callbacks cb)
{
    doc_ = &doc;
    cache_ = &cache;
    viewport_ = &viewport;
    image_loader_ = &image_loader;
    mermaid_ = &mermaid;
    theme_service_ = &theme_service;
    renderer_ = &renderer;
    cb_ = std::move(cb);
}

// ============================================================
// 画像リソース
// ============================================================

int ResourceManager::ApplyCachedImages()
{
    const std::wstring doc_dir{ doc_->GetDirectory() };
    if (doc_dir.empty()) {
        return 0;
    }

    const float content_width = cb_.get_content_width();
    if (content_width <= 0.0f) {
        return 0;
    }

    const float viewport_top = viewport_->GetScrollY();
    const float viewport_height = cb_.get_viewport_height();
    const float buffer = viewport_height * PREFETCH_BUFFER_SCREENS;
    const float range_top = viewport_top - buffer;
    const float range_bottom = viewport_top + viewport_height + buffer;

    int applied = 0;
    auto& nodes = doc_->GetNodesMut();
    for (size_t i : doc_->GetImageNodeIndices()) {
        auto& node = nodes[i];
        auto& diagram = cache_->GetDiagram(i);
        if (diagram.bitmap) {
            continue;
        }

        if (!node.has_image() || node.image_data->src.find(L"://") != std::pmr::wstring::npos) {
            continue;
        }

        if (viewport_height > 0.0f && IsOffscreen((*cache_)[i].y_position, (*cache_)[i].height, range_top, range_bottom)) {
            continue;
        }

        // 解決済みパスのキャッシュを確認し、ディスクI/Oを回避
        auto [path_it, inserted] = resolved_image_paths_.try_emplace(i);
        auto& abs_str = std::get<1>(*path_it);
        if (inserted) {
            std::filesystem::path img_path(node.image_data->src);
            if (img_path.is_relative()) {
                img_path = std::filesystem::path(doc_dir) / img_path;
            }

            std::error_code ec;
            const auto abs_path = std::filesystem::canonical(img_path, ec);
            if (ec) {
                resolved_image_paths_.erase(i);
                continue;
            }
            abs_str = abs_path.wstring();
        }

        if (image_loader_->GetCachedImage(abs_str, diagram)) {
            node.image_data->width = diagram.width;
            node.image_data->height = diagram.height;

            const float indent = node.indent_level * renderer_->GetTheme().indent_width;
            const float node_width = content_width - indent;
            float h = diagram.height;
            if (diagram.width > node_width && diagram.width > 0) {
                h *= node_width / diagram.width;
            }
            (*cache_)[i].height = h;
            (*cache_)[i].layout_dirty = false;
            ++applied;
        }
        else {
            image_loader_->RequestLoadAsync(abs_str, [this] { OnImageLoadComplete(); });
        }
    }
    return applied;
}

void ResourceManager::LoadImages()
{
    if (ApplyCachedImages() > 0) {
        cb_.recompute_layout();
        cb_.invalidate();
    }
}

void ResourceManager::OnAppImageLoaded()
{
    image_loader_->ProcessCompletedDecodes();
}

void ResourceManager::OnImageLoadComplete()
{
    pending_flush_ = true;
    if (ApplyCachedImages() > 0) {
        cb_.recompute_layout_anchored();
    }
}

// ============================================================
// Mermaidリソース
// ============================================================

void ResourceManager::InvalidateMermaidForWidthChange()
{
    const float content_width = cb_.get_content_width();
    if (content_width <= 0.0f) {
        return;
    }

    if (last_mermaid_content_width_ > 0.0f &&
        mermaid_util::QuantizeWidth(content_width) != mermaid_util::QuantizeWidth(last_mermaid_content_width_)) {
        const float min_width = std::min(content_width, last_mermaid_content_width_);
        bool any_invalidated = false;
        for (size_t i : doc_->GetMermaidNodeIndices()) {
            auto& diagram = cache_->GetDiagram(i);
            if (diagram.bitmap && diagram.width > 0 &&
                diagram.width + 1.0f < min_width) {
                continue;
            }
            diagram.bitmap.Reset();
            diagram.width = 0;
            diagram.height = 0;
            any_invalidated = true;
        }
        if (any_invalidated) {
            mermaid_->ClearCache();
        }
        mermaid_->ClearPendingQueue();
    }
    last_mermaid_content_width_ = content_width;
}

int ResourceManager::RequestMermaidRenders()
{
    InvalidateMermaidForWidthChange();

    const float content_width = cb_.get_content_width();
    if (content_width <= 0.0f) {
        return 0;
    }

    const float viewport_top = viewport_->GetScrollY();
    const float viewport_height = cb_.get_viewport_height();
    const float buffer = viewport_height * PREFETCH_BUFFER_SCREENS;
    const float range_top = viewport_top - buffer;
    const float range_bottom = viewport_top + viewport_height + buffer;

    int applied = 0;
    for (size_t i : doc_->GetMermaidNodeIndices()) {
        auto& node = doc_->GetNodesMut()[i];
        auto& diagram = cache_->GetDiagram(i);
        if (diagram.bitmap) {
            continue;
        }

        if (viewport_height > 0.0f && IsOffscreen((*cache_)[i].y_position, (*cache_)[i].height, range_top, range_bottom)) {
            continue;
        }

        mermaid_->RequestRender(node, (*cache_)[i], diagram,
            content_width, theme_service_->IsDarkMode(),
            [this] { OnMermaidRenderComplete(); });
        if (diagram.bitmap) {
            ++applied;
        }
    }
    return applied;
}

void ResourceManager::OnMermaidRenderComplete()
{
    if (mermaid_batch_loading_) {
        return;
    }
    pending_flush_ = true;
    cb_.recompute_layout_anchored();
}

void ResourceManager::CancelMermaidBatch()
{
    mermaid_->CancelPending();
    cb_.kill_timer(TIMER_MERMAID_BATCH);
}

void ResourceManager::ScheduleMermaidBatch()
{
    mermaid_batch_next_ = 0;
    cb_.set_timer(TIMER_MERMAID_BATCH, 16);
}

void ResourceManager::ProcessMermaidBatch()
{
    MENDO_PROFILE("ProcessMermaidBatch");

    const float content_width = cb_.get_content_width();
    if (content_width <= 0.0f) {
        cb_.kill_timer(TIMER_MERMAID_BATCH);
        return;
    }

    const float viewport_top = viewport_->GetScrollY();
    const float viewport_height = cb_.get_viewport_height();
    const float buffer = viewport_height * EVICT_BUFFER_SCREENS;
    const float range_top = viewport_top - buffer;
    const float range_bottom = viewport_top + viewport_height + buffer;

    const bool dark_mode = theme_service_->IsDarkMode();
    const auto& indices = doc_->GetMermaidNodeIndices();
    bool any_loaded = false;

    const auto start = std::chrono::steady_clock::now();

    mermaid_batch_loading_ = true;
    while (mermaid_batch_next_ < indices.size()) {
        const size_t i = indices[mermaid_batch_next_];

        if (viewport_height > 0.0f && IsOffscreen((*cache_)[i].y_position, (*cache_)[i].height, range_top, range_bottom)) {
            mermaid_batch_next_++;
            continue;
        }

        auto& node = doc_->GetNodesMut()[i];
        auto& diagram = cache_->GetDiagram(i);

        if (!diagram.bitmap) {
            mermaid_->RequestRender(node, (*cache_)[i], diagram,
                content_width, dark_mode,
                [this] { OnMermaidRenderComplete(); });
            if (diagram.bitmap) {
                any_loaded = true;
            }
        }

        mermaid_batch_next_++;

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >= BATCH_TIME_BUDGET_US) {
            break;
        }
    }
    mermaid_batch_loading_ = false;

    if (any_loaded) {
        cb_.recompute_layout_anchored();
    }

    if (mermaid_batch_next_ >= indices.size()) {
        cb_.kill_timer(TIMER_MERMAID_BATCH);
    }
}

// ============================================================
// ビットマップ管理
// ============================================================

void ResourceManager::EvictOffscreenBitmaps()
{
    const float viewport_top = viewport_->GetScrollY();
    const float viewport_height = cb_.get_viewport_height();
    if (viewport_height <= 0.0f) {
        return;
    }

    const float buffer = viewport_height * EVICT_BUFFER_SCREENS;
    const float evict_top = viewport_top - buffer;
    const float evict_bottom = viewport_top + viewport_height + buffer;

    const size_t node_count = doc_->GetNodes().size();

    // テキストレイアウトの解放
    const int first_keep = FindFirstVisibleNodeIndex(*cache_, node_count, evict_top);
    int last_keep = first_keep;
    for (int i = first_keep; i < static_cast<int>(node_count); i++) {
        if ((*cache_)[i].y_position > evict_bottom) {
            break;
        }
        last_keep = i + 1;
    }

    auto evict_text_layout = [&](size_t i) {
        auto& entry = (*cache_)[i];
        if (!entry.text_layout && !entry.has_table_layout()) {
            return;
        }
        entry.text_layout.Reset();
        entry.effects_applied = false;
        entry.inline_code_bgs.clear();
        entry.table_layout.reset();
        entry.layout_dirty = true;
    };

    for (size_t i = 0; i < static_cast<size_t>(first_keep); i++) {
        evict_text_layout(i);
    }
    for (size_t i = static_cast<size_t>(last_keep); i < node_count; i++) {
        evict_text_layout(i);
    }

    // 画像ビットマップの解放
    for (size_t i : doc_->GetImageNodeIndices()) {
        auto& diagram = cache_->GetDiagram(i);
        if (!diagram.bitmap) {
            continue;
        }
        if (IsOffscreen((*cache_)[i].y_position, (*cache_)[i].height, evict_top, evict_bottom)) {
            diagram.bitmap.Reset();
        }
    }

    // Mermaidビットマップの解放
    bool any_mermaid_evicted = false;
    for (size_t i : doc_->GetMermaidNodeIndices()) {
        auto& diagram = cache_->GetDiagram(i);
        if (!diagram.bitmap) {
            continue;
        }
        if (IsOffscreen((*cache_)[i].y_position, (*cache_)[i].height, evict_top, evict_bottom)) {
            diagram.bitmap.Reset();
            any_mermaid_evicted = true;
        }
    }
    if (any_mermaid_evicted) {
        mermaid_->ClearCache();
    }
}

void ResourceManager::FlushPendingResources()
{
    // 完了した非同期デコード結果をキャッシュに格納する。
    // 結果がある場合はコールバック経由で pending_flush_ が設定される。
    image_loader_->ProcessCompletedDecodes();

    if (!pending_flush_) {
        return;
    }
    pending_flush_ = false;

    bool changed = (ApplyCachedImages() > 0);

    mermaid_batch_loading_ = true;
    changed |= (RequestMermaidRenders() > 0);
    mermaid_batch_loading_ = false;

    if (changed) {
        cb_.recompute_layout();
    }
}

void ResourceManager::ScheduleBitmapManage()
{
    pending_flush_ = true;  // スクロールで新たに可視になったノードをスキャンする
    FlushPendingResources();
    cb_.set_timer(TIMER_BITMAP_MANAGE, 150);
}

void ResourceManager::OnBitmapManageTimer()
{
    cb_.kill_timer(TIMER_BITMAP_MANAGE);

    EvictOffscreenBitmaps();
    pending_flush_ = true;  // evict後に可視範囲のリソースを再読み込みする
    FlushPendingResources();

    cb_.invalidate();
}
