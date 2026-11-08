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
#include <bitset>
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

using ImageSlots = std::bitset<kMaxImageSlots>;

// ノード i の画像のうち keep に重なる部分にあるもの。表は 1 ノードなので、keep に重なる行のセルの画像だけにする。
// 表全体にすると、1 行が範囲にかかるだけで表の全画像を持つ (1705x1056 のスクリーンショットを 1 行 1 枚置いた 40 行の表を
// 1200x900 の窓で最後までめくって戻ると private 98MB・GPU 55MB、行ごとだと 57MB・42MB。2026-11-08)。
// 表の行の位置が決まらない間 (組む前・keep が無い・数式か画像の寸法を待って仮に組んだ間) は nullopt。仮に組んだ表は
// 画像を幅 0 で組んで行が詰まっているので、範囲の行が実際よりずっと多い。
std::optional<ImageSlots> ImageSlotsIn(const Node& node, const LayoutCache& cache, size_t i, const YRange* keep)
{
    if (!node.has_table()) {
        return ImageSlots{}.set();
    }
    const auto& entry = cache[i];
    const auto* tl = entry.table_layout();
    if (!keep || !tl || entry.pending_inline) {
        return std::nullopt;
    }
    const auto local = keep->ToLocal(cache.Top(i));
    const auto [r_begin, r_end] = tl->VisibleRowRange(local.top, local.bottom);
    const NodeTableData& tbl = *node.table_data();
    ImageSlots slots;
    for (size_t ci = tl->CellIndex(r_begin, 0); ci < tl->CellIndex(r_end, 0); ++ci) {
        for (const TextRun& run : tbl.CellRuns(ci)) {
            if (run.image()) {
                slots.set(run.image_slot);
            }
        }
    }
    return slots;
}

} // namespace

bool ResourceManager::ApplyCachedImageSizes(IndexRange range)
{
    // 読み込みを起動しないので、読んだ画像が無ければ何も適用できない (起動直後の復元など)。
    if (image_loader_.ImageInfoCount() == 0) {
        return false;
    }
    InlineImageUpdate update;
    ApplyCachedImagesIn(std::array{ VisibleSlice(state_.document.doc.GetImageNodeIndices(), range) }, nullptr, update);
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
    // 受け取ったビットマップは描く側へ渡す。先読み範囲の外へ出た画像の分は捨て、戻ったら読み直す。
    const auto request = [this](const DeliveredImages& delivered) {
        InlineImageUpdate update;
        ApplyCachedImagesIn(PrioritizedSlices(state_.document.doc.GetImageNodeIndices()), &delivered, update);
        image_loader_.CancelStaleBitmapRequests();
        return update;
    };
    const InlineImageUpdate update = request(image_loader_.TakeDelivered());
    NotifyInlineImages(update);
    // 寸法を待って組み直した表は行の位置が決まったので、範囲の行の画像をすぐ頼む。BITMAP_MANAGE を待つと、初めて開いた
    // 写真の表の最初の画像が 275ms、すぐ頼むと 123ms (行ごとに読む前は 96ms。2026-11-08)。受け取ったものは無いので、
    // 表は変わらない。
    if (update.pending_resolved && update.unsettled_tables) {
        request({});
    }
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
        // 行の位置が決まらない表は、読んだ行を見分けられないので全部残す。その間は表の画像を新たに読まないので増えない。
        const ImageSlots slots = ImageSlotsIn(nodes[i], state_.document.layout_cache, i, &keep->keep).value_or(ImageSlots{}.set());
        const auto srcs = nodes[i].image_srcs();
        for (size_t slot = 0; slot < srcs.size(); ++slot) {
            if (!slots.test(slot)) {
                continue;
            }
            const uint64_t key = InlineImageKey(srcs[slot]);
            if (const auto* bitmap = inline_drawings_.images.Find(key)) {
                *kept.TryEmplace(key).first = *bitmap;
            }
        }
    }
    inline_drawings_.images = std::move(kept);
}

void ResourceManager::FlushPendingResources()
{
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

void ResourceManager::ApplyCachedImagesIn(std::span<const std::span<const size_t>> slices, const DeliveredImages* delivered, InlineImageUpdate& update)
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
    const auto prefetch = ComputeBufferedRange(PREFETCH_BUFFER_SCREENS);
    for (const size_t i : slices | std::views::join) {
        const auto& node = nodes[i];
        // 表のセルの画像は列幅に縮めて出し、表は本文の幅に収めるので、表も本文の幅でデコードする。
        const float max_width = layout::NodeContentWidth(node, theme, content_width);
        const auto slots = ImageSlotsIn(node, state_.document.layout_cache, i, prefetch ? &prefetch->keep : nullptr);
        update.unsettled_tables |= !slots;
        const ImageSlots needs_bitmap = slots.value_or(ImageSlots{});
        const auto srcs = node.image_srcs();
        for (size_t slot = 0; slot < srcs.size(); ++slot) {
            ApplyCachedImage(srcs[slot], doc_dir, max_width, delivered, needs_bitmap.test(slot), update);
        }
    }
}

void ResourceManager::ApplyCachedImage(std::string_view src, const std::wstring& doc_dir, float max_width, const DeliveredImages* delivered,
                                       bool needs_bitmap, InlineImageUpdate& update)
{
    const uint64_t key = InlineImageKey(src);
    const std::wstring* abs_path = ResolveImagePath(src, doc_dir);
    const auto* info = abs_path ? image_loader_.FindImageInfo(*abs_path) : nullptr;
    if (!info && (!abs_path || image_loader_.HasGivenUp(*abs_path))) {
        // 読めないもの (解決できない src・読むのをあきらめたファイル) は alt を出す。読み直しで失敗の記録が消えても、
        // 読めるまで alt のまま読み直す (読み込み中の幅 0 との行き来でちらつかせない)。
        SetInlineImage(key, { .state = InlineImageState::Unavailable }, update);
        return;
    }
    if (info) {
        // DPI が変わると同じ画像でも DIP の寸法が変わる。
        const auto size = image_loader_.DipSize(*info);
        SetInlineImage(key, {
                                .state = InlineImageState::Ready, .size = { size.width, size.height }
        },
                       update);
    }
    if (!delivered) {
        return;
    }
    if (!needs_bitmap) {
        // 届いたビットマップも捨て、範囲に入ったら読み直す。
        if (!info) {
            image_loader_.RequestSizeAsync(*abs_path);
        }
        return;
    }
    if (ID2D1Bitmap* const received = delivered->Find(*abs_path)) {
        auto& bitmap = *inline_drawings_.images.TryEmplace(key).first;
        if (bitmap.Get() != received) {
            bitmap = received;
            update.bitmaps_changed = true;
        }
        return;
    }
    // 描くビットマップが無ければ読む (保持範囲の外や別の文書から戻ったときは寸法が残っているので組み直さない)。
    // 持っている間は読み直さない。件数に上限のあるキャッシュに置いていた頃は、表の画像が 128 種類を超えると読み込みが
    // 互いを追い出し、約 1ms ごとに読み直しと描き直しが続いた (4db8079b)。
    // 表示幅が広がったら、今の解像度で描きながら高い解像度で読み直す。
    if (!inline_drawings_.FindImage(key) || (info && image_loader_.NeedsSharperDecode(*info, max_width))) {
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
