#pragma once
#include "app_constants.h"
#include "document.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "viewport_manager.h"
#include "image_loader.h"
#include "mermaid_file_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "nav.h"
#include "theme_service.h"
#include "profiler.h"
#include "string_convert.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <concepts>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// ResourceManager が依存するサービス群を 1 つにまとめる DI コンテナ。
// 各ポインタは ResourceManager の生存期間中 valid である必要がある。
struct ResourceManagerDeps {
    Document* doc = nullptr;
    LayoutCache* cache = nullptr;
    ViewportManager* viewport = nullptr;
    ImageLoader* image_loader = nullptr;
    IMermaidRenderer* mermaid = nullptr;
    // リロード時の高さ適用にだけ使う。nullptr なら適用しない。
    MermaidFileCache* mermaid_file_cache = nullptr;
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
    // 遅延レイアウトの計測範囲も兼ねる。範囲外へのスクロールは EnsureVisibleLayout が同期計測で拾う。
    static constexpr float EVICT_BUFFER_SCREENS = 2.0f;
    // evict 範囲より広く先読みすると、evict 直後の flush で範囲外の画像/図を毎回読み直す。
    static constexpr float PREFETCH_BUFFER_SCREENS = 2.0f;
    static_assert(PREFETCH_BUFFER_SCREENS <= EVICT_BUFFER_SCREENS);
    static constexpr int BATCH_TIME_BUDGET_US = 6000;

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

        const std::pmr::wstring& doc_dir = deps_.doc->GetDirectory();
        if (doc_dir.empty()) {
            return 0;
        }
        // 全件モードは読み込みを起動しないので、キャッシュが空なら何も適用できない (起動直後の復元など)。
        if (!respect_viewport && deps_.image_loader->CacheSize() == 0) {
            return 0;
        }

        const float content_width = cb_.get_content_width();
        if (content_width <= 0.0f) {
            return 0;
        }

        const Theme& theme = cb_.get_theme();
        const auto& nodes = deps_.doc->GetNodes();
        const auto& image_indices = deps_.doc->GetImageNodeIndices();

        // 通常描画は可視範囲に intersect する image index のみを走査。
        // リロード時 (respect_viewport=false) は CalcScrollYForDiff の Y 計算用に全件処理する。
        const IndexSlice slice = respect_viewport
            ? BufferedSlice(image_indices, PREFETCH_BUFFER_SCREENS)
            : IndexSlice{ image_indices.begin(), image_indices.end() };

        int applied = 0;
        for (auto it = slice.begin; it != slice.end; ++it) {
            const size_t i = *it;
            const auto& node = nodes[i];
            const auto* const img = node.image_data();
            if (!img || img->src.contains("://")) {
                continue;
            }

            const std::wstring* abs_path = ResolveImagePath(i, img->src, doc_dir);
            if (!abs_path) {
                continue;
            }

            // 通常運用時のみ非同期ロードを起動する。リロード時は後続の LoadImages effect で起動する。
            const float max_width = content_width - mendo::layout::NodeIndent(node, theme);
            const auto request = [&] {
                if (respect_viewport) {
                    deps_.image_loader->RequestLoadAsync(*abs_path, [this] { OnImageLoadComplete(); }, max_width);
                }
            };
            const auto* d = deps_.cache->FindDiagram(i);
            const auto* cached = deps_.image_loader->FindCachedImage(*abs_path);
            if (!cached) {
                // LRU から落ちても表示中のビットマップはそのまま使う。
                if (!d || !d->bitmap) {
                    request();
                }
                continue;
            }

            // DiagramEntry はキャッシュに当たった画像だけ確保する。寸法は再計測 (幅変更・ズーム) でも使う。
            if (!d || !d->bitmap || d->bitmap != cached->bitmap) {
                auto& diagram = deps_.cache->EnsureDiagram(i);
                diagram.bitmap = cached->bitmap;
                diagram.width = cached->width;
                diagram.height = cached->height;

                auto& entry = (*deps_.cache)[i];
                entry.height = mendo::layout::ImageDisplayHeight(cached->width, cached->height, max_width);
                entry.layout_dirty = false;
                height_changed_.Add(i);
                ++applied;
            }
            // 表示幅が広がったら、今の解像度で描きながら高い解像度で読み直す。
            if (deps_.image_loader->NeedsSharperDecode(*cached, max_width)) {
                request();
            }
        }
        return applied;
    }

    int ApplyCachedImagesForReload()
    {
        return ApplyCachedImages(false);
    }

    // リロード直後のスクロール位置計算用に、ディスクキャッシュにある図の寸法を推定高さへ上書きする。
    // ビットマップは読まないので、描画は後続の RequestMermaidRenders に任せる。
    bool ApplyMermaidCacheHeightsForReload()
    {
        if (!deps_.mermaid_file_cache) {
            return false;
        }
        const float content_width = cb_.get_content_width();
        const bool dark_mode = deps_.theme_service->IsDarkMode();
        const auto& nodes = deps_.doc->GetNodes();
        bool any_applied = false;
        for (size_t i : deps_.doc->GetDiagramNodeIndices()) {
            const auto hash = mermaid_util::NodeDiagramHash(nodes[i], content_width, dark_mode);
            MermaidFileCache::CacheEntry fentry;
            if (deps_.mermaid_file_cache->LookupDimensions(hash, fentry)) {
                (*deps_.cache)[i].height = fentry.css_height;
                any_applied = true;
            }
        }
        return any_applied;
    }

    void LoadImages()
    {
        if (ApplyCachedImages() > 0) {
            cb_.recompute_layout(TakeHeightChanges());
        }
    }

    void OnAppImageLoaded()
    {
        deps_.image_loader->ProcessCompletedDecodes();
    }

    int RequestMermaidRenders()
    {
        const float content_width = cb_.get_content_width();
        InvalidateMermaidForWidthChange(content_width);

        if (content_width <= 0.0f) {
            return 0;
        }

        DropMermaidQueueIfJumped();
        const auto slice = BufferedSlice(deps_.doc->GetDiagramNodeIndices(), PREFETCH_BUFFER_SCREENS);
        const bool dark_mode = deps_.theme_service->IsDarkMode();

        // 同期キャッシュヒットの度に再レイアウトせず、ループ後にまとめて 1 回だけ行う。
        int applied = 0;
        BatchMermaidCompletions([&] {
            for (auto it = slice.begin; it != slice.end; ++it) {
                applied += RequestDiagramRender(*it, content_width, dark_mode) ? 1 : 0;
            }
        });
        return applied;
    }

    // f の中で完了した図の再レイアウトを 1 回にまとめる (ディスクキャッシュの一括完了など)。
    // nested 呼び出し (FlushPendingResources 経由) では外側が再レイアウトの責任を持つ。
    template <std::invocable F>
    void BatchMermaidCompletions(F&& f)
    {
        const bool outer_batch = std::exchange(mermaid_batch_loading_, true);
        std::forward<F>(f)();
        mermaid_batch_loading_ = outer_batch;
        if (!outer_batch && !height_changed_.empty()) {
            pending_flush_ = true;
            cb_.recompute_layout_anchored(TakeHeightChanges());
        }
    }

    void CancelMermaidBatch()
    {
        deps_.mermaid->CancelPending();
        cb_.kill_timer(app_timer::Id::MERMAID_BATCH);
        height_changed_ = {};
    }

    void ScheduleMermaidBatch()
    {
        mermaid_batch_next_ = 0;
        cb_.set_timer(app_timer::Id::MERMAID_BATCH, app_timer::FRAME_INTERVAL_MS);
    }

    void ProcessMermaidBatch()
    {
        MENDO_PROFILE("ProcessMermaidBatch");

        const float content_width = cb_.get_content_width();
        InvalidateMermaidForWidthChange(content_width);

        if (content_width <= 0.0f) {
            cb_.kill_timer(app_timer::Id::MERMAID_BATCH);
            return;
        }

        DropMermaidQueueIfJumped();
        const bool dark_mode = deps_.theme_service->IsDarkMode();
        const auto& indices = deps_.doc->GetDiagramNodeIndices();

        const auto start = std::chrono::steady_clock::now();

        // バッチ範囲を可視 + buffer の部分レンジに限定する。
        // mermaid_batch_next_ は indices 内の position（indices[n] が node index）。
        // 進捗の意味を保ったまま、可視レンジ内のみを走査。
        const auto s = BufferedSlice(indices, EVICT_BUFFER_SCREENS);
        const size_t slice_start = static_cast<size_t>(s.begin - indices.begin());
        const size_t slice_end = static_cast<size_t>(s.end - indices.begin());
        mermaid_batch_next_ = std::max(mermaid_batch_next_, slice_start);

        BatchMermaidCompletions([&] {
            while (mermaid_batch_next_ < slice_end) {
                RequestDiagramRender(indices[mermaid_batch_next_], content_width, dark_mode);
                ++mermaid_batch_next_;

                const auto elapsed = std::chrono::steady_clock::now() - start;
                if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >= BATCH_TIME_BUDGET_US) {
                    break;
                }
            }
        });

        if (mermaid_batch_next_ >= slice_end) {
            cb_.kill_timer(app_timer::Id::MERMAID_BATCH);
        }
    }

    void EvictOffscreenBitmaps()
    {
        using resource_manager_detail::VisibleSlice;

        const auto keep = ComputeBufferedRange(EVICT_BUFFER_SCREENS);
        if (!keep) {
            return;
        }
        const auto& vr = keep->range;

        deps_.cache->EvictTextLayouts(vr.first, vr.last_plus_1);

        // 可視範囲をまたぐ巨大テーブルでは、ノード単位 evict では拾えない不可視行のセルを別途解放する。
        // keep 範囲外のテーブルは上の EvictTextLayouts で全行解放済みなので、範囲内だけ見る。
        const auto tables = VisibleSlice(deps_.doc->GetTableNodeIndices(), vr.first, vr.last_plus_1);
        deps_.cache->EvictInvisibleTableRows(std::span<const size_t>(tables.begin, tables.end), keep->viewport_top, keep->viewport_bottom, keep->buffer);

        // image/diagram bitmap も可視範囲外の分を捨てる。レンダ済みビットマップの
        // 二次キャッシュ (mermaid 内 LRU, 128 件) は自前で上限管理されるため全消去しない。
        // 全消去すると可視中の図まで捨てて WebView2 再レンダの cliff を生むため。
        deps_.cache->EvictDiagramBitmapsOutside(vr.first, vr.last_plus_1);
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
        // last_flush_time_ を一元的に更新し、両経路でスロットリングが効くようにする。
        last_flush_time_ = std::chrono::steady_clock::now();

        bool changed = (ApplyCachedImages() > 0);

        // 外側でバッチ扱いにし、内側の anchored 再レイアウトを下の recompute_layout 1 回へ統合する。
        const bool outer_batch = std::exchange(mermaid_batch_loading_, true);
        changed |= (RequestMermaidRenders() > 0);
        mermaid_batch_loading_ = outer_batch;

        if (changed) {
            cb_.recompute_layout(TakeHeightChanges());
        }
    }

    void ScheduleBitmapManage()
    {
        // 直近の flush から間がなければ再実行を抑止する。
        // 細かいスクロールで FlushPendingResources が毎フレーム走るのを防ぎ、
        // タイマー側で集約的に処理する。
        constexpr auto kFlushThrottle = std::chrono::milliseconds(50);
        pending_flush_ = true;
        if (std::chrono::steady_clock::now() - last_flush_time_ >= kFlushThrottle) {
            // 連続スクロール中も通過した範囲のレイアウトを溜め込まないよう、停止を待たずに捨てる。
            EvictOffscreenBitmaps();
            FlushPendingResources();
        }
        cb_.set_timer(app_timer::Id::BITMAP_MANAGE, app_timer::BITMAP_MANAGE_DELAY_MS);
    }

    void OnBitmapManageTimer()
    {
        cb_.kill_timer(app_timer::Id::BITMAP_MANAGE);

        EvictOffscreenBitmaps();
        // evict 直後は可視範囲のリソース再読み込みが必要なので強制フラッシュする。
        pending_flush_ = true;
        // evict 対象は可視範囲外なので、再描画はフラッシュで適用があった時 (recompute_layout) だけでよい。
        FlushPendingResources();
    }

    void ClearResolvedPaths() noexcept
    {
        resolved_image_paths_.clear();
    }

private:
    void OnImageLoadComplete()
    {
        pending_flush_ = true;
        if (ApplyCachedImages() > 0) {
            cb_.recompute_layout_anchored(TakeHeightChanges());
        }
    }

    // 同期キャッシュヒットは RequestRender 内で OnMermaidRenderComplete まで完了する。
    // 戻り値: その場で bitmap が確定したか。
    bool RequestDiagramRender(size_t i, float content_width, bool dark_mode)
    {
        auto& diagram = deps_.cache->EnsureDiagram(i);
        // エラー確定した図も NeedsRender()=false で弾き、失敗レンダの無限リトライを防ぐ。
        if (!diagram.NeedsRender()) {
            return false;
        }
        deps_.mermaid->RequestRender(deps_.doc->GetNodes()[i], (*deps_.cache)[i], diagram, content_width, dark_mode, [this, i] { OnMermaidRenderComplete(i); });
        return diagram.bitmap != nullptr;
    }

    void OnMermaidRenderComplete(size_t node_index)
    {
        height_changed_.Add(node_index);
        if (mermaid_batch_loading_) {
            return;
        }
        pending_flush_ = true;
        cb_.recompute_layout_anchored(TakeHeightChanges());
    }

    // 解決済みパスはノード単位でキャッシュする。解決できなければ nullptr。
    const std::wstring* ResolveImagePath(size_t node_index, std::string_view src, const std::pmr::wstring& doc_dir)
    {
        auto [it, inserted] = resolved_image_paths_.try_emplace(node_index);
        if (!inserted) {
            return &it->second;
        }
        // canonical() は symlink 解決のためにファイルシステムを叩くので、
        // UI 同期パスから外すため absolute() + lexically_normal() を使う。
        // 画像参照が symlink を跨ぐのはレアケースとして許容する。
        // src は UTF-8。char から直接構築すると ACP 解釈になり非 ASCII パスが壊れる。
        // "my%20images/a.png" のようなエンコード済みの記述も多いので、デコードしてから引く。
        std::filesystem::path img_path(string_convert::Utf8ToWide(PercentDecode(src)));
        if (img_path.is_relative()) {
            img_path = std::filesystem::path(doc_dir) / img_path;
        }
        std::error_code ec;
        const auto abs_path = std::filesystem::absolute(img_path, ec);
        if (ec) {
            resolved_image_paths_.erase(it);
            return nullptr;
        }
        it->second = abs_path.lexically_normal().wstring();
        return &it->second;
    }

    // 保持範囲 (±EVICT_BUFFER_SCREENS) を越えて移動していたら、旧位置で積んだ描画待ちを捨てる。
    // FIFO のままだと TOC ジャンプや Ctrl+End の後に可視の図が旧位置の図の後回しになる。
    void DropMermaidQueueIfJumped()
    {
        const float scroll_y = deps_.viewport->GetScrollY();
        const float viewport_height = cb_.get_viewport_height();
        if (viewport_height > 0.0f && std::abs(scroll_y - last_mermaid_request_scroll_) > viewport_height * EVICT_BUFFER_SCREENS) {
            deps_.mermaid->DropQueued();
        }
        last_mermaid_request_scroll_ = scroll_y;
    }

    mendo::layout::HeightChangeRange TakeHeightChanges() noexcept
    {
        return std::exchange(height_changed_, {});
    }

    struct BufferedRange {
        float viewport_top;
        float viewport_bottom;
        float buffer;
        VisibleRange range;
    };
    // 可視範囲 ± viewport_height * screens に交差するノード範囲。viewport_height <= 0 (初期化中等) は範囲が決まらない。
    std::optional<BufferedRange> ComputeBufferedRange(float screens)
    {
        const float viewport_height = cb_.get_viewport_height();
        if (viewport_height <= 0.0f) {
            return std::nullopt;
        }
        const float viewport_top = deps_.viewport->GetScrollY();
        const float viewport_bottom = viewport_top + viewport_height;
        const float buffer = viewport_height * screens;
        const auto vr = ComputeVisibleNodeRange(*deps_.cache, viewport_top - buffer, viewport_bottom + buffer);
        return BufferedRange{ viewport_top, viewport_bottom, buffer, vr };
    }

    // indices のうち ComputeBufferedRange に交差する部分。範囲が決まらないときは全件を返す。
    resource_manager_detail::IndexSlice BufferedSlice(const std::pmr::vector<size_t>& indices, float screens)
    {
        const auto r = ComputeBufferedRange(screens);
        if (!r) {
            return { indices.begin(), indices.end() };
        }
        return resource_manager_detail::VisibleSlice(indices, r->range.first, r->range.last_plus_1);
    }

    void InvalidateMermaidForWidthChange(float content_width)
    {
        if (content_width <= 0.0f) {
            return;
        }

        if (last_mermaid_content_width_ > 0.0f &&
            mermaid_util::QuantizeWidth(content_width) != mermaid_util::QuantizeWidth(last_mermaid_content_width_)) {
            const float min_width = std::min(content_width, last_mermaid_content_width_);
            // 幅変化 invalidation は全 diagram を対象にする必要がある（不可視分も旧幅ビットマップを持ちうるため）。
            // メモリキャッシュはキーに量子化幅を含むので消さず、幅を戻したときに再利用する。
            for (size_t i : deps_.doc->GetDiagramNodeIndices()) {
                auto* diagram = deps_.cache->FindDiagram(i);
                if (diagram && diagram->bitmap && diagram->width > 0 &&
                    diagram->width + 1.0f < min_width) {
                    continue;
                }
                // 幅が変わればエラー結果も変わりうるため、error 込みで破棄して再試行させる。
                if (diagram) {
                    diagram->ResetForRetry();
                }
            }
            // 旧幅で処理中の in-flight リクエストを無効化し、完了時に旧 bitmap で上書きされるのを防ぐ。
            deps_.mermaid->CancelPending();
        }
        last_mermaid_content_width_ = content_width;
    }

    ResourceManagerDeps deps_{};
    Cb cb_{};

    float last_mermaid_content_width_ = 0.0f;
    float last_mermaid_request_scroll_ = 0.0f;
    bool mermaid_batch_loading_ = false;
    size_t mermaid_batch_next_ = 0;
    std::unordered_map<size_t, std::wstring> resolved_image_paths_;
    bool pending_flush_ = false;
    std::chrono::steady_clock::time_point last_flush_time_{};
    // 画像/図の適用で高さを更新したノード範囲。Y 再計算をこの範囲に限定する。
    mendo::layout::HeightChangeRange height_changed_ = {};
};
