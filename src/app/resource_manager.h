#pragma once
#include "app_constants.h"
#include "document.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "viewport_manager.h"
#include "image_loader.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "theme_service.h"
#include "profiler.h"
#include "ascii_util.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <unordered_map>
#include <string>
#include <windows.h>

// ResourceManager が依存するサービス群を 1 つにまとめる DI コンテナ。
// 各ポインタは ResourceManager の生存期間中 valid である必要がある。
struct ResourceManagerDeps {
    Document* doc = nullptr;
    LayoutCache* cache = nullptr;
    ViewportManager* viewport = nullptr;
    ImageLoader* image_loader = nullptr;
    IMermaidRenderer* mermaid = nullptr;
    ThemeService* theme_service = nullptr;
};

namespace resource_manager_detail {

struct IndexSlice {
    std::pmr::vector<size_t>::const_iterator begin;
    std::pmr::vector<size_t>::const_iterator end;
};

constexpr IndexSlice VisibleSlice(const std::pmr::vector<size_t>& sorted_indices, size_t first_visible_node, size_t last_visible_node_plus_1) noexcept
{
    const auto b = std::lower_bound(sorted_indices.begin(), sorted_indices.end(), first_visible_node);
    const auto e = std::lower_bound(b, sorted_indices.end(), last_visible_node_plus_1);
    return { b, e };
}

} // namespace resource_manager_detail

// 画像・Mermaidリソースのライフサイクル管理。
// Appから画像読み込み、Mermaidバッチ処理、ビットマップ解放の責務を分離する。
template <class Cb>
class ResourceManagerT {
public:
    static constexpr float EVICT_BUFFER_SCREENS = 5.0f;
    static constexpr float PREFETCH_BUFFER_SCREENS = 3.0f;
    static constexpr int BATCH_TIME_BUDGET_US = 6000;

    ResourceManagerT() = default;

    constexpr void Init(const ResourceManagerDeps& deps, Cb cb) noexcept
    {
        deps_ = deps;
        cb_ = std::move(cb);
    }

    // respect_viewport=true: 可視範囲のみ走査し、未キャッシュは非同期ロード起動（通常描画用）。
    // respect_viewport=false: 全画像を走査、未キャッシュは無視（リロード時のスクロール計算前用）。
    int ApplyCachedImages(bool respect_viewport = true)
    {
        using resource_manager_detail::IndexSlice;
        using resource_manager_detail::VisibleSlice;

        const std::pmr::wstring doc_dir = deps_.doc->GetDirectory();
        if (doc_dir.empty()) {
            return 0;
        }

        const float content_width = cb_.get_content_width();
        if (content_width <= 0.0f) {
            return 0;
        }

        const float indent_width = cb_.get_indent_width();
        auto& nodes = deps_.doc->GetNodesMut();
        const auto& image_indices = deps_.doc->GetImageNodeIndices();

        // 通常描画は可視範囲に intersect する image index のみを走査。
        // リロード時 (respect_viewport=false) は CalcScrollForDiff の Y 計算用に全件処理する。
        // viewport_height <= 0.0f の時は初期化中等なのでレイアウト範囲無視（全件）で従来挙動を保つ。
        IndexSlice slice{ image_indices.begin(), image_indices.end() };
        if (respect_viewport) {
            const float viewport_height = cb_.get_viewport_height();
            if (viewport_height > 0.0f) {
                const float viewport_top = deps_.viewport->GetScrollY();
                const float buffer = viewport_height * PREFETCH_BUFFER_SCREENS;
                const float range_top = viewport_top - buffer;
                const float range_bottom = viewport_top + viewport_height + buffer;
                const auto vr = ComputeVisibleNodeRange(*deps_.cache, nodes.size(), range_top, range_bottom);
                slice = VisibleSlice(image_indices, vr.first, vr.last_plus_1);
            }
        }

        int applied = 0;
        for (auto it = slice.begin; it != slice.end; ++it) {
            const size_t i = *it;
            auto& node = nodes[i];
            auto& diagram = deps_.cache->GetDiagram(i);
            if (diagram.bitmap) {
                continue;
            }

            auto* const img = node.image_data();
            if (!img || ascii_util::Contains(img->src, "://")) {
                continue;
            }

            // 解決済みパスのキャッシュを確認し、再計算を回避する。
            auto [path_it, inserted] = resolved_image_paths_.try_emplace(i);
            auto& abs_str = std::get<1>(*path_it);
            if (inserted) {
                // canonical() は symlink 解決のためにファイルシステムを叩くので、
                // UI 同期パスから外すため absolute() + lexically_normal() を使う。
                // 画像参照が symlink を跨ぐのはレアケースとして許容する。
                std::filesystem::path img_path(img->src);
                if (img_path.is_relative()) {
                    img_path = std::filesystem::path(doc_dir) / img_path;
                }
                std::error_code ec;
                auto abs_path = std::filesystem::absolute(img_path, ec);
                if (ec) {
                    resolved_image_paths_.erase(i);
                    continue;
                }
                abs_str = abs_path.lexically_normal().wstring();
            }

            if (deps_.image_loader->GetCachedImage(abs_str, diagram)) {
                img->width = diagram.width;
                img->height = diagram.height;

                const float indent = node.indent_level * indent_width;
                const float node_width = content_width - indent;
                float h = diagram.height;
                if (diagram.width > node_width && diagram.width > 0) {
                    h *= node_width / diagram.width;
                }
                (*deps_.cache)[i].height = h;
                (*deps_.cache)[i].layout_dirty = false;
                ++applied;
            }
            else if (respect_viewport) {
                // 通常運用時のみ未キャッシュ画像を非同期ロード起動。
                // リロード時は後続の LoadImages effect で起動するためスキップ。
                deps_.image_loader->RequestLoadAsync(abs_str, [this] { OnImageLoadComplete(); });
            }
        }
        return applied;
    }

    int ApplyCachedImagesForReload()
    {
        return ApplyCachedImages(false);
    }

    void LoadImages()
    {
        if (ApplyCachedImages() > 0) {
            cb_.recompute_layout();
            cb_.invalidate();
        }
    }

    void OnAppImageLoaded()
    {
        deps_.image_loader->ProcessCompletedDecodes();
    }

    void OnImageLoadComplete()
    {
        pending_flush_ = true;
        if (ApplyCachedImages() > 0) {
            cb_.recompute_layout_anchored();
        }
    }

    int RequestMermaidRenders()
    {
        using resource_manager_detail::IndexSlice;
        using resource_manager_detail::VisibleSlice;

        const float content_width = cb_.get_content_width();
        InvalidateMermaidForWidthChange(content_width);

        if (content_width <= 0.0f) {
            return 0;
        }

        const float viewport_top = deps_.viewport->GetScrollY();
        const float viewport_height = cb_.get_viewport_height();
        const float buffer = viewport_height * PREFETCH_BUFFER_SCREENS;
        const float range_top = viewport_top - buffer;
        const float range_bottom = viewport_top + viewport_height + buffer;

        const auto& diagram_indices = deps_.doc->GetDiagramNodeIndices();
        IndexSlice slice{ diagram_indices.begin(), diagram_indices.end() };
        if (viewport_height > 0.0f) {
            const auto& nodes = deps_.doc->GetNodes();
            const auto vr = ComputeVisibleNodeRange(*deps_.cache, nodes.size(), range_top, range_bottom);
            slice = VisibleSlice(diagram_indices, vr.first, vr.last_plus_1);
        }

        // 同期キャッシュヒットの度に OnMermaidRenderComplete が recompute_layout_anchored を
        // 発火するのを抑止し、ループ後にまとめて 1 回だけ呼ぶ。nested 呼び出し
        // (FlushPendingResources 経由) では外側が責任を持つよう save+restore する。
        const bool outer_batch = mermaid_batch_loading_;
        mermaid_batch_loading_ = true;

        int applied = 0;
        for (auto it = slice.begin; it != slice.end; ++it) {
            const size_t i = *it;
            auto& node = deps_.doc->GetNodesMut()[i];
            auto& diagram = deps_.cache->GetDiagram(i);
            if (diagram.bitmap) {
                continue;
            }

            deps_.mermaid->RequestRender(node, (*deps_.cache)[i], diagram, content_width, deps_.theme_service->IsDarkMode(), [this] { OnMermaidRenderComplete(); });
            if (diagram.bitmap) {
                ++applied;
            }
        }

        mermaid_batch_loading_ = outer_batch;

        if (!outer_batch && applied > 0) {
            pending_flush_ = true;
            cb_.recompute_layout_anchored();
        }
        return applied;
    }

    void OnMermaidRenderComplete()
    {
        if (mermaid_batch_loading_) {
            return;
        }
        pending_flush_ = true;
        cb_.recompute_layout_anchored();
    }

    void CancelMermaidBatch()
    {
        deps_.mermaid->CancelPending();
        cb_.kill_timer(app_timer::Id::MERMAID_BATCH);
    }

    void ScheduleMermaidBatch()
    {
        mermaid_batch_next_ = 0;
        cb_.set_timer(app_timer::Id::MERMAID_BATCH, 16);
    }

    void ProcessMermaidBatch()
    {
        using resource_manager_detail::VisibleSlice;

        MENDO_PROFILE("ProcessMermaidBatch");

        const float content_width = cb_.get_content_width();
        InvalidateMermaidForWidthChange(content_width);

        if (content_width <= 0.0f) {
            cb_.kill_timer(app_timer::Id::MERMAID_BATCH);
            return;
        }

        const float viewport_top = deps_.viewport->GetScrollY();
        const float viewport_height = cb_.get_viewport_height();
        const float buffer = viewport_height * EVICT_BUFFER_SCREENS;
        const float range_top = viewport_top - buffer;
        const float range_bottom = viewport_top + viewport_height + buffer;

        const bool dark_mode = deps_.theme_service->IsDarkMode();
        const auto& indices = deps_.doc->GetDiagramNodeIndices();
        bool any_loaded = false;

        const auto start = std::chrono::steady_clock::now();

        // バッチ範囲を可視 + buffer の部分レンジに限定する。
        // mermaid_batch_next_ は indices 内の position（indices[n] が node index）。
        // 進捗の意味を保ったまま、可視レンジ内のみを走査。
        size_t slice_end = indices.size();
        if (viewport_height > 0.0f) {
            const auto& nodes = deps_.doc->GetNodes();
            const auto vr = ComputeVisibleNodeRange(*deps_.cache, nodes.size(), range_top, range_bottom);
            const auto s = VisibleSlice(indices, vr.first, vr.last_plus_1);
            const size_t slice_start = static_cast<size_t>(s.begin - indices.begin());
            slice_end = static_cast<size_t>(s.end - indices.begin());
            if (mermaid_batch_next_ < slice_start) {
                mermaid_batch_next_ = slice_start;
            }
        }

        mermaid_batch_loading_ = true;
        while (mermaid_batch_next_ < slice_end) {
            const size_t i = indices[mermaid_batch_next_];

            auto& node = deps_.doc->GetNodesMut()[i];
            auto& diagram = deps_.cache->GetDiagram(i);

            if (!diagram.bitmap) {
                deps_.mermaid->RequestRender(node, (*deps_.cache)[i], diagram, content_width, dark_mode, [this] { OnMermaidRenderComplete(); });
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

        if (mermaid_batch_next_ >= slice_end) {
            cb_.kill_timer(app_timer::Id::MERMAID_BATCH);
        }
    }

    void EvictOffscreenBitmaps()
    {
        using resource_manager_detail::VisibleSlice;

        const float viewport_top = deps_.viewport->GetScrollY();
        const float viewport_height = cb_.get_viewport_height();
        if (viewport_height <= 0.0f) {
            return;
        }

        const float buffer = viewport_height * EVICT_BUFFER_SCREENS;
        const float evict_top = viewport_top - buffer;
        const float evict_bottom = viewport_top + viewport_height + buffer;

        const size_t node_count = deps_.doc->GetNodes().size();

        const auto vr = ComputeVisibleNodeRange(*deps_.cache, node_count, evict_top, evict_bottom);
        const int first_keep = static_cast<int>(vr.first);
        const int last_keep = static_cast<int>(vr.last_plus_1);

        deps_.cache->EvictTextLayouts(static_cast<size_t>(first_keep), static_cast<size_t>(last_keep));

        // 可視範囲をまたぐ巨大テーブルでは、ノード単位 evict では拾えない不可視行のセルを別途解放する。
        deps_.cache->EvictInvisibleTableRows(viewport_top, viewport_top + viewport_height, buffer);

        // image/diagram bitmap の evict も可視範囲外（[0, first_keep) と
        // [last_keep, node_count)）だけを走査する。IndexSlice で配列の該当部分を
        // 切り出して、各々 bitmap をリセット。
        const auto& image_indices = deps_.doc->GetImageNodeIndices();
        const auto img_keep = VisibleSlice(image_indices, static_cast<size_t>(first_keep), static_cast<size_t>(last_keep));
        for (auto it = image_indices.begin(); it != img_keep.begin; ++it) {
            auto& diagram = deps_.cache->GetDiagram(*it);
            if (diagram.bitmap) {
                diagram.bitmap.Reset();
            }
        }
        for (auto it = img_keep.end; it != image_indices.end(); ++it) {
            auto& diagram = deps_.cache->GetDiagram(*it);
            if (diagram.bitmap) {
                diagram.bitmap.Reset();
            }
        }

        const auto& diagram_indices = deps_.doc->GetDiagramNodeIndices();
        const auto dia_keep = VisibleSlice(diagram_indices, static_cast<size_t>(first_keep), static_cast<size_t>(last_keep));
        // オフスクリーンの diagram bitmap (layout 側) だけ解放する。レンダ済みビットマップの
        // 二次キャッシュ (mermaid 内 LRU, 128 件) は自前で上限管理されるため全消去しない。
        // 全消去すると可視中の図まで捨てて WebView2 再レンダの cliff を生むため。
        auto evict_mermaid = [&](size_t i) {
            auto& diagram = deps_.cache->GetDiagram(i);
            if (diagram.bitmap) {
                diagram.bitmap.Reset();
            }
        };
        for (auto it = diagram_indices.begin(); it != dia_keep.begin; ++it) {
            evict_mermaid(*it);
        }
        for (auto it = dia_keep.end; it != diagram_indices.end(); ++it) {
            evict_mermaid(*it);
        }
    }

    void FlushPendingResources()
    {
        // 完了した非同期デコード結果をキャッシュに格納する。
        // 結果があればコールバック経由で pending_flush_ が設定される。
        deps_.image_loader->ProcessCompletedDecodes();

        if (!pending_flush_) {
            return;
        }
        pending_flush_ = false;
        // ScheduleBitmapManage 以外（OnBitmapManageTimer 等）からのフラッシュでも
        // last_flush_time_ を一元的に更新し、両経路で 50ms スロットリングが効くようにする。
        last_flush_time_ = std::chrono::steady_clock::now();

        bool changed = (ApplyCachedImages() > 0);

        mermaid_batch_loading_ = true;
        changed |= (RequestMermaidRenders() > 0);
        mermaid_batch_loading_ = false;

        if (changed) {
            cb_.recompute_layout();
        }
    }

    void ScheduleBitmapManage()
    {
        // 直近 50ms 以内に既に flush していれば再実行を抑止する。
        // 細かいスクロールで FlushPendingResources が毎フレーム走るのを防ぎ、
        // タイマー (150ms) 側で集約的に処理する。
        const auto now = std::chrono::steady_clock::now();
        const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_time_).count();
        pending_flush_ = true;
        if (since_last >= 50) {
            FlushPendingResources();
        }
        cb_.set_timer(app_timer::Id::BITMAP_MANAGE, 150);
    }

    void OnBitmapManageTimer()
    {
        cb_.kill_timer(app_timer::Id::BITMAP_MANAGE);

        EvictOffscreenBitmaps();
        // evict 直後は可視範囲のリソース再読み込みが必要なので強制フラッシュする。
        pending_flush_ = true;
        FlushPendingResources();

        cb_.invalidate();
    }

    void ClearResolvedPaths() noexcept
    {
        resolved_image_paths_.clear();
    }

private:
    void InvalidateMermaidForWidthChange(float content_width)
    {
        if (content_width <= 0.0f) {
            return;
        }

        if (last_mermaid_content_width_ > 0.0f &&
            mermaid_util::QuantizeWidth(content_width) != mermaid_util::QuantizeWidth(last_mermaid_content_width_)) {
            const float min_width = std::min(content_width, last_mermaid_content_width_);
            bool any_invalidated = false;
            // 幅変化 invalidation は全 diagram を対象にする必要がある（不可視分も旧幅ビットマップを持ちうるため）。
            for (size_t i : deps_.doc->GetDiagramNodeIndices()) {
                auto& diagram = deps_.cache->GetDiagram(i);
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
                deps_.mermaid->ClearCache();
            }
            // 旧幅で処理中の in-flight リクエストを無効化し、完了時に旧 bitmap で上書きされるのを防ぐ。
            deps_.mermaid->CancelPending();
        }
        last_mermaid_content_width_ = content_width;
    }

    ResourceManagerDeps deps_{};
    Cb cb_{};

    float last_mermaid_content_width_ = 0.0f;
    bool mermaid_batch_loading_ = false;
    size_t mermaid_batch_next_ = 0;
    std::unordered_map<size_t, std::wstring> resolved_image_paths_;
    bool pending_flush_ = false;
    std::chrono::steady_clock::time_point last_flush_time_{};
};
