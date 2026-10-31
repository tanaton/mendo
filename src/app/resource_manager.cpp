#include "resource_manager.h"
#include "document.h"
#include "file_io.h"
#include "image_loader.h"
#include "layout_cache.h"
#include "mermaid_file_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "nav.h"
#include "profiler.h"
#include "string_convert.h"
#include "theme.h"
#include "viewport_manager.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <ranges>
#include <utility>

namespace {

constexpr std::span<const size_t> VisibleSlice(std::span<const size_t> sorted_indices, size_t first_visible_node, size_t last_visible_node_plus_1) noexcept
{
    const auto b = std::ranges::lower_bound(sorted_indices, first_visible_node);
    const auto e = std::ranges::lower_bound(b, sorted_indices.end(), last_visible_node_plus_1);
    return { b, e };
}

// 開かない src (リモート・UNC・解決失敗) は空文字列。
std::wstring ResolveLocalImagePath(std::string_view src, const std::wstring& doc_dir)
{
    if (src.contains("://")) {
        return {};
    }
    // canonical() は symlink 解決のためにファイルシステムを叩くので、
    // UI 同期パスから外すため absolute() + lexically_normal() を使う。
    // 画像参照が symlink を跨ぐのはレアケースとして許容する。
    // src は UTF-8。char から直接構築すると ACP 解釈になり非 ASCII パスが壊れる。
    // "my%20images/a.png" のようなエンコード済みの記述も多いので、デコードしてから引く。
    std::filesystem::path img_path(string_convert::Utf8ToWide(PercentDecode(src)));
    // 相対パスは文書と同じ場所を引くだけなので、文書が共有フォルダ上でも通す。
    if (!path_util::IsLocalPath(img_path.native())) {
        return {};
    }
    if (img_path.is_relative()) {
        // \\?\ 付きの文書フォルダのまま結合すると、.. で \\?\UNC\host へ抜けられる。
        auto base = path_util::ToWin32Path(doc_dir);
        if (!base) {
            return {};
        }
        img_path = std::filesystem::path(std::move(*base)) / img_path;
    }
    std::error_code ec;
    const auto abs_path = std::filesystem::absolute(img_path, ec);
    if (ec) {
        return {};
    }
    return abs_path.lexically_normal().wstring();
}

} // namespace

void ResourceManager::ApplyCachedImages()
{
    DropQueuesIfJumped();
    ApplyCachedImagesIn(PrioritizedSlices(deps_.doc->GetImageNodeIndices(), PREFETCH_BUFFER_SCREENS), true, height_changed_);
}

bool ResourceManager::ApplyCachedImagesForReload(VisibleRange range)
{
    // 読み込みを起動しないので、キャッシュが空なら何も適用できない (起動直後の復元など)。
    if (deps_.image_loader->CacheSize() == 0) {
        return false;
    }
    // Y は呼び出し側が組み直すので、変えた範囲は height_changed_ に積まず、後の再レイアウトへ持ち越さない。
    layout::HeightChangeRange changed;
    ApplyCachedImagesIn(std::array{ VisibleSlice(deps_.doc->GetImageNodeIndices(), range.first, range.last_plus_1) }, false, changed);
    return !changed.empty();
}

bool ResourceManager::ApplyMermaidCacheHeightsForReload(VisibleRange range)
{
    if (!deps_.mermaid_file_cache) {
        return false;
    }
    const float content_width = host_->MdContentWidth();
    const Theme& theme = host_->GetTheme();
    const bool dark_mode = theme.dark;
    const auto& nodes = deps_.doc->GetNodes();
    bool any_applied = false;
    for (size_t i : VisibleSlice(deps_.doc->GetDiagramNodeIndices(), range.first, range.last_plus_1)) {
        // 結果を残しているエントリ (仮表示で引き継いだ図など) にはハッシュも残し、RequestRender に計算し直させない。
        auto* const diagram = deps_.cache->FindDiagram(i);
        const uint64_t code_hash = diagram ? mermaid_util::EnsureDiagramCodeHash(diagram->code_hash, nodes[i])
                                           : mermaid_util::DiagramCodeHash(nodes[i]);
        if (const auto hit = deps_.mermaid_file_cache->Find(mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode),
                                                            mermaid_util::DiagramRequestHash(code_hash, content_width, dark_mode),
                                                            content_width, /*touch=*/false)) {
            const float node_width = layout::NodeContentWidth(nodes[i], theme, content_width);
            // 引き継いだ図は高さが変わらないので、Y の再計算を呼ばせない。
            const float height = layout::BitmapDisplayHeight(hit->entry.css_width, hit->entry.css_height, node_width);
            any_applied |= std::exchange((*deps_.cache)[i].height, height) != height;
        }
    }
    return any_applied;
}

void ResourceManager::LoadImages()
{
    ApplyCachedImages();
    RecomputeChangedHeights();
}

void ResourceManager::OnImagesDecoded(std::chrono::steady_clock::time_point deadline)
{
    if (deps_.image_loader->ProcessCompletedDecodes(deadline)) {
        LoadImages();
    }
}

void ResourceManager::RequestMermaidRenders()
{
    // 同期キャッシュヒットの度に再レイアウトせず、まとめて 1 回だけ行う。
    RequestVisibleDiagrams();
    RecomputeChangedHeights();
}

void ResourceManager::CancelMermaidBatch()
{
    host_->KillTimer(app_timer::Id::MERMAID_BATCH);
    height_changed_ = {};
}

void ResourceManager::ScheduleMermaidBatch()
{
    host_->SetTimer(app_timer::Id::MERMAID_BATCH, app_timer::FRAME_INTERVAL_MS);
}

void ResourceManager::ProcessMermaidBatch()
{
    MENDO_PROFILE("ProcessMermaidBatch");
    host_->KillTimer(app_timer::Id::MERMAID_BATCH);
    RequestMermaidRenders();
}

void ResourceManager::EvictOffscreenBitmaps()
{
    const auto keep = ComputeBufferedRange(EVICT_BUFFER_SCREENS);
    if (!keep) {
        return;
    }

    const auto layout_keep = ComputeBufferedRange(LAYOUT_EVICT_BUFFER_SCREENS);
    const auto& lr = layout_keep->range;
    deps_.cache->EvictTextLayouts(lr.first, lr.last_plus_1);
    // keep 範囲外のテーブルは上で全行解放済みなので、範囲内だけ見る。表はセル 1 つごとに text_layout を持つので、
    // ノード単位の evict では拾えない不可視行のセルも捨てる。
    deps_.cache->EvictInvisibleTableRows(VisibleSlice(deps_.doc->GetTableNodeIndices(), lr.first, lr.last_plus_1), layout_keep->keep);

    // image/diagram bitmap も可視範囲外の分を捨てる。レンダ済みビットマップの
    // 二次キャッシュ (mermaid 内, 128 件) は自前で上限管理されるため全消去しない。
    // 全消去すると可視中の図まで捨てて WebView2 再レンダの cliff を生むため。
    deps_.cache->EvictDiagramBitmapsOutside(keep->range.first, keep->range.last_plus_1);
}

void ResourceManager::FlushPendingResources()
{
    deps_.image_loader->ProcessCompletedDecodes();
    // ScheduleBitmapManage 以外（OnBitmapManageTimer 等）からのフラッシュでも
    // last_flush_time_ を一元的に更新し、両経路でスロットリングが効くようにする。
    last_flush_time_ = std::chrono::steady_clock::now();

    // 画像と図の再レイアウトを 1 回にまとめる。
    ApplyCachedImages();
    RequestVisibleDiagrams();
    RecomputeChangedHeights();
}

void ResourceManager::ScheduleBitmapManage()
{
    // 直近の flush から間がなければ再実行を抑止する。
    // 細かいスクロールで FlushPendingResources が毎フレーム走るのを防ぎ、
    // タイマー側で集約的に処理する。
    constexpr auto kFlushThrottle = std::chrono::milliseconds(50);
    if (std::chrono::steady_clock::now() - last_flush_time_ >= kFlushThrottle) {
        // 連続スクロール中も通過した範囲のレイアウトを溜め込まないよう、停止を待たずに捨てる。
        EvictOffscreenBitmaps();
        FlushPendingResources();
    }
    host_->SetTimer(app_timer::Id::BITMAP_MANAGE, app_timer::BITMAP_MANAGE_DELAY_MS);
}

void ResourceManager::OnBitmapManageTimer()
{
    host_->KillTimer(app_timer::Id::BITMAP_MANAGE);

    EvictOffscreenBitmaps();
    // evict 直後は可視範囲のリソースを読み直す。
    // evict 対象は可視範囲外なので、再描画はフラッシュで適用があった時 (RecomputeLayout) だけでよい。
    FlushPendingResources();
}

void ResourceManager::ApplyCachedImagesIn(std::span<const std::span<const size_t>> slices, bool start_loads, layout::HeightChangeRange& changed)
{
    const std::wstring& doc_dir = deps_.doc->GetDirectory();
    if (doc_dir.empty()) {
        return;
    }
    // 解決結果は src と文書フォルダだけで決まるので、同じフォルダでのリロードでは使い回す。
    if (doc_dir != resolved_dir_) {
        resolved_image_paths_.clear();
        resolved_dir_ = doc_dir;
    }
    const float content_width = host_->MdContentWidth();
    if (content_width <= 0.0f) {
        return;
    }

    const Theme& theme = host_->GetTheme();
    const auto& nodes = deps_.doc->GetNodes();

    for (const size_t i : slices | std::views::join) {
        const auto& node = nodes[i];
        const std::string_view src = node.image_src();
        if (src.empty()) {
            continue;
        }

        const std::wstring* abs_path = ResolveImagePath(src, doc_dir);
        if (!abs_path) {
            continue;
        }

        const float max_width = layout::NodeContentWidth(node, theme, content_width);
        const auto request = [&] {
            if (start_loads) {
                deps_.image_loader->RequestLoadAsync(*abs_path, max_width);
            }
        };
        const auto* d = deps_.cache->FindDiagram(i);
        const auto* cached = deps_.image_loader->FindCachedImage(*abs_path);
        if (!cached) {
            // キャッシュから落ちても表示中のビットマップはそのまま使う。
            if (!d || !d->bitmap) {
                request();
            }
            continue;
        }

        // DiagramEntry はキャッシュに当たった画像だけ確保する。寸法は再計測 (幅変更・ズーム) でも使う。
        // DPI が変わると同じビットマップでも DIP の寸法が変わるので、寸法も比べる。
        const auto size = deps_.image_loader->DipSize(*cached);
        if (!d || !d->bitmap || d->bitmap != cached->bitmap || d->width != size.width || d->height != size.height) {
            auto& diagram = deps_.cache->EnsureDiagram(i);
            diagram.bitmap = cached->bitmap;
            diagram.width = size.width;
            diagram.height = size.height;

            ApplyBitmapHeight(i, diagram, max_width, changed);
        }
        // 表示幅が広がったら、今の解像度で描きながら高い解像度で読み直す。
        if (deps_.image_loader->NeedsSharperDecode(*cached, max_width)) {
            request();
        }
    }
}

void ResourceManager::RequestVisibleDiagrams()
{
    const float content_width = host_->MdContentWidth();
    if (content_width <= 0.0f) {
        return;
    }

    InvalidateMermaidForWidthChange(content_width);
    DropQueuesIfJumped();
    const auto slices = PrioritizedSlices(deps_.doc->GetDiagramNodeIndices(), PREFETCH_BUFFER_SCREENS);
    const bool dark_mode = host_->GetTheme().dark;
    for (const size_t i : slices | std::views::join) {
        RequestDiagramRender(i, content_width, dark_mode);
    }
}

void ResourceManager::RequestDiagramRender(size_t i, float content_width, bool dark_mode)
{
    auto& diagram = deps_.cache->EnsureDiagram(i);
    // エラー確定した図も NeedsRender()=false で弾き、失敗レンダの無限リトライを防ぐ。
    if (!diagram.NeedsRender()) {
        return;
    }
    const Node& node = deps_.doc->GetNodes()[i];
    if (!deps_.mermaid->RequestRender(node, diagram, content_width, dark_mode)) {
        return;
    }
    // 失敗 (エラー確定) でも、プレースホルダーの文言と、文言を見せる高さが変わる。
    ApplyBitmapHeight(i, diagram, layout::NodeContentWidth(node, host_->GetTheme(), content_width), height_changed_);
}

void ResourceManager::ApplyBitmapHeight(size_t i, const DiagramEntry& bitmap, float node_width, layout::HeightChangeRange& changed)
{
    auto& entry = (*deps_.cache)[i];
    entry.CommitMeasured(node_width, layout::BitmapNodeHeight(&bitmap, node_width, entry.height, host_->GetTheme()));
    changed.Add(i);
}

const std::wstring* ResourceManager::ResolveImagePath(std::string_view src, const std::wstring& doc_dir)
{
    auto it = resolved_image_paths_.find(src);
    if (it == resolved_image_paths_.end()) {
        it = resolved_image_paths_.emplace(std::string{ src }, ResolveLocalImagePath(src, doc_dir)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

void ResourceManager::DropQueuesIfJumped()
{
    const float scroll_y = deps_.viewport->GetScrollY();
    const float viewport_height = host_->MdPaneHeight();
    if (viewport_height > 0.0f && std::abs(scroll_y - last_request_scroll_) > viewport_height * EVICT_BUFFER_SCREENS) {
        deps_.mermaid->DropQueued();
        deps_.image_loader->DropQueued();
    }
    last_request_scroll_ = scroll_y;
}

void ResourceManager::RecomputeChangedHeights()
{
    if (!height_changed_.empty()) {
        host_->RecomputeLayout(std::exchange(height_changed_, {}));
    }
}

std::optional<ResourceManager::BufferedRange> ResourceManager::ComputeBufferedRange(float screens)
{
    const float viewport_height = host_->MdPaneHeight();
    if (viewport_height <= 0.0f) {
        return std::nullopt;
    }
    const float viewport_top = deps_.viewport->GetScrollY();
    const YRange keep = layout::BufferedRange(viewport_top, viewport_height, screens);
    const auto vr = ComputeVisibleNodeRange(*deps_.cache, keep.top, keep.bottom);
    return BufferedRange{ viewport_top, viewport_top + viewport_height, keep, vr };
}

std::array<std::span<const size_t>, 3> ResourceManager::PrioritizedSlices(std::span<const size_t> indices, float screens)
{
    const auto r = ComputeBufferedRange(screens);
    if (!r) {
        return { indices, {}, {} };
    }
    const auto all = VisibleSlice(indices, r->range.first, r->range.last_plus_1);
    const auto vr = ComputeVisibleNodeRange(*deps_.cache, r->viewport_top, r->viewport_bottom);
    const auto visible = VisibleSlice(all, vr.first, vr.last_plus_1);
    const auto above = static_cast<size_t>(visible.data() - all.data());
    return { visible, all.subspan(above + visible.size()), all.first(above) };
}

void ResourceManager::InvalidateMermaidForWidthChange(float content_width)
{
    if (last_mermaid_content_width_ > 0.0f &&
        mermaid_util::QuantizeWidth(content_width) != mermaid_util::QuantizeWidth(last_mermaid_content_width_)) {
        const float min_width = std::min(content_width, last_mermaid_content_width_);
        // 幅変化 invalidation は全 diagram を対象にする必要がある（不可視分も旧幅ビットマップを持ちうるため）。
        // メモリキャッシュはキーに量子化幅を含むので消さず、幅を戻したときに再利用する。
        for (size_t i : deps_.doc->GetDiagramNodeIndices()) {
            auto* diagram = deps_.cache->FindDiagram(i);
            // 幅が変わればエラー結果も変わりうるため、error 込みで再試行させる。描き直しの間は旧幅の結果を仮に表示する。
            if (diagram && !(diagram->bitmap && diagram->width > 0 && mermaid_util::IsWidthFree(diagram->width, min_width))) {
                diagram->MarkStale();
            }
        }
        // 旧幅の描画待ちを捨て、新しい幅の要求を先に処理させる。描画中のものは旧幅のキャッシュに入るだけ。
        deps_.mermaid->DropQueued();
    }
    last_mermaid_content_width_ = content_width;
}
