#include "resource_manager.h"
#include "app_state.h"
#include "document.h"
#include "document_utils.h"
#include "draw_command.h"
#include "image_loader.h"
#include "inline_math.h"
#include "layout_cache.h"
#include "mermaid_file_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "theme.h"
#include "viewport_manager.h"
#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace {

constexpr std::span<const size_t> VisibleSlice(std::span<const size_t> sorted_indices, IndexRange range) noexcept
{
    const auto b = std::ranges::lower_bound(sorted_indices, range.begin);
    const auto e = std::ranges::lower_bound(b, sorted_indices.end(), range.end);
    return { b, e };
}

} // namespace

bool ResourceManager::ApplyCachedImageSizes(IndexRange range)
{
    // 読み込みを起動しないので、キャッシュが空なら何も適用できない (起動直後の復元など)。
    if (image_loader_.CacheSize() == 0) {
        return false;
    }
    InlineImageUpdate update;
    ApplyCachedImagesIn(std::array{ VisibleSlice(state_.document.doc.GetImageNodeIndices(), range) }, false, update);
    return update.pending_resolved || update.sizes_changed;
}

bool ResourceManager::ApplyCachedHeights(IndexRange range)
{
    ApplyCachedImageSizes(range);
    return ApplyMermaidCacheHeights(range);
}

bool ResourceManager::ApplyMermaidCacheHeights(IndexRange range)
{
    const float content_width = MdContentWidth(state_);
    const Theme& theme = *state_.theme;
    const bool dark_mode = theme.dark;
    const auto& nodes = state_.document.doc.GetNodes();
    bool any_applied = false;
    for (size_t i : VisibleSlice(state_.document.doc.GetDiagramNodeIndices(), range)) {
        // 結果を残しているエントリ (仮表示で引き継いだ図など) にはハッシュも残し、RequestRender に計算し直させない。
        auto* const diagram = state_.document.layout_cache.FindDiagram(i);
        const uint64_t code_hash = diagram ? mermaid_util::EnsureDiagramCodeHash(diagram->code_hash, nodes[i])
                                           : mermaid_util::DiagramCodeHash(nodes[i]);
        if (const auto hit = mermaid_file_cache_.Find(mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode),
                                                      mermaid_util::DiagramRequestHash(code_hash, content_width, dark_mode),
                                                      content_width, /*touch=*/false)) {
            const float node_width = layout::NodeContentWidth(nodes[i], theme, content_width);
            // 引き継いだ図は高さが変わらないので、Y の再計算を呼ばせない。
            const float height = layout::BitmapDisplayHeight(hit->entry.css_width, hit->entry.css_height, node_width);
            any_applied |= std::exchange(state_.document.layout_cache[i].height, height) != height;
        }
    }
    return any_applied;
}

void ResourceManager::LoadImages()
{
    DropQueuesIfJumped();
    InlineImageUpdate update;
    ApplyCachedImagesIn(PrioritizedSlices(state_.document.doc.GetImageNodeIndices()), true, update);
    NotifyInlineImages(update);
}

void ResourceManager::OnImagesDecoded(std::chrono::steady_clock::time_point deadline)
{
    if (image_loader_.ProcessCompletedDecodes(deadline)) {
        LoadImages();
    }
}

void ResourceManager::RequestMermaidRenders()
{
    // 同期キャッシュヒットの度に再レイアウトせず、まとめて 1 回だけ行う。
    IndexRange changed;
    RequestVisibleDiagrams(changed);
    RecomputeChangedHeights(changed);
}

void ResourceManager::EvictOffscreenBitmaps()
{
    const auto keep = ComputeBufferedRange(EVICT_BUFFER_SCREENS);
    if (!keep) {
        return;
    }

    const auto layout_keep = ComputeBufferedRange(LAYOUT_EVICT_BUFFER_SCREENS);
    const auto& lr = layout_keep->range;
    state_.document.layout_cache.EvictTextLayouts(lr.begin, lr.end);
    // keep 範囲外のテーブルは上で全行解放済みなので、範囲内だけ見る。表はセル 1 つごとに text_layout を持つので、
    // ノード単位の evict では拾えない不可視行のセルも捨てる。
    state_.document.layout_cache.EvictInvisibleTableRows(VisibleSlice(state_.document.doc.GetTableNodeIndices(), lr), layout_keep->keep);

    // 本文中の画像のビットマップも保持範囲の画像の分だけ残す。寸法は表に残すので、戻ってきても組み直さない。
    if (inline_drawings_.images.size() == 0) {
        return;
    }
    FlatHashMap<Microsoft::WRL::ComPtr<ID2D1Bitmap>> kept;
    const auto& nodes = state_.document.doc.GetNodes();
    for (const size_t i : VisibleSlice(state_.document.doc.GetImageNodeIndices(), keep->range)) {
        for (const std::string_view src : nodes[i].image_srcs()) {
            const uint64_t key = InlineImageKey(src);
            if (const auto* bitmap = inline_drawings_.images.Find(key)) {
                *kept.TryEmplace(key).first = *bitmap;
            }
        }
    }
    inline_drawings_.images = std::move(kept);
}

void ResourceManager::FlushPendingResources()
{
    // 画像の通知で組み直す (RelayoutViewport) ときは入り直さない。入り直すと届いたデコード結果の転送が
    // kUploadSliceBudget の区切りを超え、走査と図の要求も二重になる。組み直しで範囲に入った分は BITMAP_MANAGE が拾う。
    if (notifying_images_) {
        return;
    }
    image_loader_.ProcessCompletedDecodes();
    // ScheduleBitmapManage 以外（OnBitmapManageTimer 等）からのフラッシュでも
    // last_flush_time_ を一元的に更新し、両経路でスロットリングが効くようにする。
    last_flush_time_ = std::chrono::steady_clock::now();

    LoadImages();
    // 図の再レイアウトを 1 回にまとめる。
    IndexRange changed;
    RequestVisibleDiagrams(changed);
    RecomputeChangedHeights(changed);
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
    host_.SetTimer(app_timer::Id::BITMAP_MANAGE, app_timer::BITMAP_MANAGE_DELAY_MS);
}

void ResourceManager::OnBitmapManageTimer()
{
    host_.KillTimer(app_timer::Id::BITMAP_MANAGE);

    EvictOffscreenBitmaps();
    // evict 直後は可視範囲のリソースを読み直す。
    // evict 対象は可視範囲外なので、再描画はフラッシュで適用があった時 (RecomputeLayout) だけでよい。
    FlushPendingResources();
}

void ResourceManager::ApplyCachedImagesIn(std::span<const std::span<const size_t>> slices, bool start_loads, InlineImageUpdate& update)
{
    const std::wstring& doc_dir = state_.document.doc.GetDirectory();
    if (doc_dir.empty()) {
        return;
    }
    // 解決結果は src と文書フォルダだけで決まるので、同じフォルダでのリロードでは使い回す。
    if (doc_dir != resolved_dir_) {
        resolved_image_paths_.clear();
        resolved_dir_ = doc_dir;
    }
    const float content_width = MdContentWidth(state_);
    if (content_width <= 0.0f) {
        return;
    }

    const Theme& theme = *state_.theme;
    const auto& nodes = state_.document.doc.GetNodes();
    for (const size_t i : slices | std::views::join) {
        const auto& node = nodes[i];
        // 表のセルの画像は列幅に縮めて出し、表は本文の幅に収めるので、表も本文の幅でデコードする。
        const float max_width = layout::NodeContentWidth(node, theme, content_width);
        for (const std::string_view src : node.image_srcs()) {
            ApplyCachedImage(src, doc_dir, max_width, start_loads, update);
        }
    }
}

void ResourceManager::ApplyCachedImage(std::string_view src, const std::wstring& doc_dir, float max_width, bool start_loads, InlineImageUpdate& update)
{
    const uint64_t key = InlineImageKey(src);
    const std::wstring* abs_path = ResolveImagePath(src, doc_dir);
    const auto* cached = abs_path ? image_loader_.FindCachedImage(*abs_path) : nullptr;
    if (!cached) {
        // 読めないもの (解決できない src・読むのをあきらめたファイル) は alt を出す。読み直しで失敗の記録が消えても、
        // 読めるまで alt のまま読み直す (読み込み中の幅 0 との行き来でちらつかせない)。
        // キャッシュから落ちた画像は寸法とビットマップを残したまま読み直す。
        if (!abs_path || image_loader_.HasGivenUp(*abs_path)) {
            SetInlineImage(key, { .state = InlineImageState::Unavailable }, update);
        }
        else if (start_loads) {
            image_loader_.RequestLoadAsync(*abs_path, max_width);
        }
        return;
    }
    // DPI が変わると同じビットマップでも DIP の寸法が変わる。
    const auto size = image_loader_.DipSize(*cached);
    SetInlineImage(key, {
                            .state = InlineImageState::Ready, .size = { size.width, size.height }
    },
                   update);
    auto& bitmap = *inline_drawings_.images.TryEmplace(key).first;
    if (bitmap != cached->bitmap) {
        bitmap = cached->bitmap;
        update.bitmaps_changed = true;
    }
    // 表示幅が広がったら、今の解像度で描きながら高い解像度で読み直す。
    if (start_loads && image_loader_.NeedsSharperDecode(*cached, max_width)) {
        image_loader_.RequestLoadAsync(*abs_path, max_width);
    }
}

void ResourceManager::SetInlineImage(uint64_t key, const InlineImageEntry& entry, InlineImageUpdate& update)
{
    if (const InlineImageEntry before = inline_images_.Exchange(key, entry); before != entry) {
        (before.state == InlineImageState::Loading ? update.pending_resolved : update.sizes_changed) = true;
    }
}

void ResourceManager::NotifyInlineImages(const InlineImageUpdate& update)
{
    if (update.pending_resolved || update.sizes_changed || update.bitmaps_changed) {
        notifying_images_ = true;
        const ScopeGuard reset{ [this] noexcept { notifying_images_ = false; } };
        host_.OnInlineImagesUpdated(update.pending_resolved, update.sizes_changed);
    }
}

void ResourceManager::RequestVisibleDiagrams(IndexRange& changed)
{
    const float content_width = MdContentWidth(state_);
    if (content_width <= 0.0f) {
        return;
    }

    const bool dark_mode = state_.theme->dark;
    InvalidateDiagramsForRequestChange(content_width, dark_mode);
    DropQueuesIfJumped();
    const auto slices = PrioritizedSlices(state_.document.doc.GetDiagramNodeIndices());
    for (const size_t i : slices | std::views::join) {
        RequestDiagramRender(i, content_width, dark_mode, changed);
    }
}

void ResourceManager::RequestDiagramRender(size_t i, float content_width, bool dark_mode, IndexRange& changed)
{
    auto& diagram = state_.document.layout_cache.EnsureDiagram(i);
    // エラー確定した図も NeedsRender()=false で弾き、失敗レンダの無限リトライを防ぐ。
    if (!diagram.NeedsRender()) {
        return;
    }
    const Node& node = state_.document.doc.GetNodes()[i];
    if (!mermaid_.RequestRender(node, diagram, content_width, dark_mode)) {
        return;
    }
    // 失敗 (エラー確定) でも、プレースホルダーの文言と、文言を見せる高さが変わる。
    ApplyBitmapHeight(i, diagram, layout::NodeContentWidth(node, *state_.theme, content_width), changed);
}

void ResourceManager::ApplyBitmapHeight(size_t i, const DiagramEntry& bitmap, float node_width, IndexRange& changed)
{
    auto& entry = state_.document.layout_cache[i];
    entry.CommitMeasured(node_width, layout::BitmapNodeHeight(&bitmap, node_width, entry.height, *state_.theme));
    changed.Add(i);
}

const std::wstring* ResourceManager::ResolveImagePath(std::string_view src, const std::wstring& doc_dir)
{
    auto it = resolved_image_paths_.find(src);
    if (it == resolved_image_paths_.end()) {
        it = resolved_image_paths_.emplace(std::string{ src }, ResolveLocalPath(src, doc_dir)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

void ResourceManager::DropQueuesIfJumped()
{
    const float scroll_y = state_.view.viewport.GetScrollY();
    const float viewport_height = ComputePaneLayout(state_).md_rect.height;
    if (viewport_height > 0.0f && std::abs(scroll_y - last_request_scroll_) > viewport_height * EVICT_BUFFER_SCREENS) {
        mermaid_.DropQueued();
        image_loader_.DropQueued();
    }
    last_request_scroll_ = scroll_y;
}

void ResourceManager::RecomputeChangedHeights(const IndexRange& changed)
{
    if (!changed.empty()) {
        host_.RecomputeLayout(changed);
    }
}

std::optional<ResourceManager::BufferedRange> ResourceManager::ComputeBufferedRange(float screens)
{
    const float viewport_height = ComputePaneLayout(state_).md_rect.height;
    if (viewport_height <= 0.0f) {
        return std::nullopt;
    }
    const float viewport_top = state_.view.viewport.GetScrollY();
    const YRange keep = layout::BufferedRange(viewport_top, viewport_height, screens);
    const auto vr = ComputeVisibleNodeRange(state_.document.layout_cache, keep.top, keep.bottom);
    return BufferedRange{ viewport_top, viewport_top + viewport_height, keep, vr };
}

std::array<std::span<const size_t>, 3> ResourceManager::PrioritizedSlices(std::span<const size_t> indices)
{
    const auto r = ComputeBufferedRange(PREFETCH_BUFFER_SCREENS);
    if (!r) {
        return { indices, {}, {} };
    }
    const auto all = VisibleSlice(indices, r->range);
    const auto vr = ComputeVisibleNodeRange(state_.document.layout_cache, r->viewport_top, r->viewport_bottom);
    const auto visible = VisibleSlice(all, vr);
    const auto above = static_cast<size_t>(visible.data() - all.data());
    return { visible, all.subspan(above + visible.size()), all.first(above) };
}

void ResourceManager::InvalidateDiagramsForRequestChange(float content_width, bool dark_mode)
{
    // 初回は比べる相手が無い。
    const bool requested_before = last_mermaid_content_width_ > 0.0f;
    // 図はテーマの色を焼き込むので、テーマが変われば全部描き直す。
    const bool dark_changed = requested_before && dark_mode != last_mermaid_dark_mode_;
    const bool width_changed =
        requested_before && mermaid_util::QuantizeWidth(content_width) != mermaid_util::QuantizeWidth(last_mermaid_content_width_);
    if (dark_changed || width_changed) {
        const float min_width = std::min(content_width, last_mermaid_content_width_);
        // 不可視の図も旧幅・旧テーマの結果を持ちうるので、全部の図を見る。
        // メモリキャッシュはキーに量子化幅とテーマを含むので消さず、戻したときに再利用する。
        for (size_t i : state_.document.doc.GetDiagramNodeIndices()) {
            auto* diagram = state_.document.layout_cache.FindDiagram(i);
            if (!diagram) {
                continue;
            }
            // 幅が変わればエラー結果も変わりうるため、error 込みで再試行させる。描き直しの間は旧幅の結果を仮に表示する。
            if (dark_changed || !(diagram->HasImage() && diagram->width > 0 && mermaid_util::IsWidthFree(diagram->width, min_width))) {
                diagram->MarkStale();
            }
        }
        // 旧幅・旧テーマの描画待ちを捨て、新しい要求を先に処理させる。描画中のものは旧幅・旧テーマのキャッシュに入るだけ。
        mermaid_.DropQueued();
    }
    last_mermaid_content_width_ = content_width;
    last_mermaid_dark_mode_ = dark_mode;
}
