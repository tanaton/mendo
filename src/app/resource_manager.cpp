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
// col_widths があれば、表の画像ごとに、範囲の行で置かれる列の幅 (DIP) の最大を入れる。範囲に無い画像は 0。
std::optional<ImageSlots> ImageSlotsIn(const Node& node, const LayoutCache& cache, size_t i, const YRange* keep,
                                       std::array<float, kMaxImageSlots>* col_widths = nullptr)
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
    if (col_widths) {
        col_widths->fill(0.0f);
    }
    for (size_t ci = tl->CellIndex(r_begin, 0); ci < tl->CellIndex(r_end, 0); ++ci) {
        for (const TextRun& run : tbl.CellRuns(ci)) {
            if (run.image()) {
                slots.set(run.image_slot);
                if (col_widths) {
                    auto& w = (*col_widths)[run.image_slot];
                    w = std::max(w, tl->col_widths[ci % tl->col_count]);
                }
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
        const uint64_t code_hash = mermaid_util::DiagramCodeHash(nodes[i]);
        if (const auto hit = mermaid_file_cache_.Find(mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode),
                                                      mermaid_util::DiagramRequestHash(code_hash, content_width, dark_mode),
                                                      content_width, /*touch=*/false)) {
            const float node_width = layout::NodeContentWidth(nodes[i], theme, content_width);
            // 引き継いだ図は高さが変わらないので、Y の再計算を呼ばせない。
            const float height = layout::DiagramDisplayHeight(hit->entry.css_width, hit->entry.css_height, node_width);
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
        // 頼み直していない走査の後で取り消すと、進行中のデコードの結果まで捨てる。
        if (!host_.IsLoadingOtherDocument()) {
            image_loader_.CancelStaleBitmapRequests();
        }
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
    // 同期キャッシュヒットの度に再レイアウトせず、まとめて 1 回だけ行う。Y の再計算は変わった範囲に限る。
    IndexRange changed;
    RequestVisibleDiagrams(changed);
    if (!changed.empty()) {
        host_.RecomputeLayout(changed);
    }
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

    // 図のコマンドリストも保持範囲の図の分だけ残す (test.md の図 27 個で commit 1〜2MB、1 個 40〜70KB)。寸法と描画命令は
    // 残すので組み直さず、戻ってきたら先読み範囲に入ったときに UI で記録し直す (文字を組むので 1 個約 0.9ms)。
    // メモリを返すほうをユーザーが選んだ (2026-11-13)。
    const std::span<const size_t> diagrams = state_.document.doc.GetDiagramNodeIndices();
    const auto kept_diagrams = VisibleSlice(diagrams, keep->range);
    for (const auto part : { std::span(diagrams.begin(), kept_diagrams.begin()), std::span(kept_diagrams.end(), diagrams.end()) }) {
        for (const size_t i : part) {
            auto* diagram = state_.document.layout_cache.FindDiagram(i);
            if (!diagram) {
                continue;
            }
            const Microsoft::WRL::ComPtr<ID2D1CommandList> commands = diagram->commands;
            if (diagram->ReleaseCommands()) {
                mermaid_.ReleaseCommands(commands.Get());
            }
        }
    }

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
    RequestMermaidRenders();
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

void ResourceManager::DropQueues()
{
    mermaid_.DropQueued();
    image_loader_.DropQueued();
}

void ResourceManager::ForgetInlineImages()
{
    inline_images_.Clear();
    inline_drawings_.images = {};
    resolved_image_paths_.clear();
    image_watcher_.Clear();
}

void ResourceManager::OnRenderDeviceRecreated()
{
    // 旧デバイスの画像のビットマップと図のコマンドリストは新 RT で描けない。寸法は残し、FlushPendingResources が
    // 新しい RT で読み直したビットマップに差し替える。
    state_.document.layout_cache.InvalidateAllDiagramCommands();
    inline_drawings_.images = {};
    // 受け箱に残したデコードの結果を入れてから、足りない画像と図を頼む。図は描画の要求からしか頼み直さないので、ここで頼む。
    FlushPendingResources();
}

void ResourceManager::ApplyCachedImagesIn(std::span<const std::span<const size_t>> slices, const DeliveredImages* delivered, InlineImageUpdate& update)
{
    const std::wstring& doc_dir = state_.document.doc.GetDirectory();
    if (doc_dir.empty()) {
        return;
    }
    // 解決結果は src と文書フォルダだけで決まるので、同じ文書のリロードでは使い回す。別の文書を開いたときは
    // ForgetInlineImages が捨てる。ここは、それより前に新しい文書で走査が走ったときの備え。
    if (doc_dir != resolved_dir_) {
        resolved_image_paths_.clear();
        // 監視も前のフォルダの画像の分をやめる。ImageLoader に残る寸法は更新を知らないままになるが、戻ったときは
        // ビットマップを読み直すので、そのデコードの結果で寸法も入れ替わる。
        image_watcher_.Clear();
        resolved_dir_ = doc_dir;
    }
    const float content_width = MdContentWidth(state_);
    if (content_width <= 0.0f) {
        return;
    }

    const Theme& theme = *state_.theme;
    const auto& nodes = state_.document.doc.GetNodes();
    const auto prefetch = ComputeBufferedRange(PREFETCH_BUFFER_SCREENS);
    std::array<float, kMaxImageSlots> col_widths;
    for (const size_t i : slices | std::views::join) {
        const auto& node = nodes[i];
        const float max_width = layout::NodeContentWidth(node, theme, content_width);
        // 表のセルの画像は列幅に縮めて出すので、置かれる列の幅でデコードする (ビットマップを頼むのは行の位置が決まった表だけ
        // なので、列幅は決まっている)。本文の幅でデコードしていた頃は、3 列 40 行の写真の表を 1200x900 の窓で最後までめくると
        // commit 121MB・GPU 97MB、列の幅だと 66MB・51MB (2026-11-13)。列が広がったら NeedsSharperDecode が読み直させる。
        const bool table = node.has_table();
        const auto slots = ImageSlotsIn(node, state_.document.layout_cache, i, prefetch ? &prefetch->keep : nullptr, table ? &col_widths : nullptr);
        update.unsettled_tables |= !slots;
        const ImageSlots needs_bitmap = slots.value_or(ImageSlots{});
        const auto srcs = node.image_srcs();
        for (size_t slot = 0; slot < srcs.size(); ++slot) {
            const bool bitmap = needs_bitmap.test(slot);
            const float width = table && bitmap ? std::min(max_width, col_widths[slot]) : max_width;
            ApplyCachedImage(srcs[slot], doc_dir, width, delivered, bitmap, update);
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
    // フォルダの監視を開いている最中の画像は、開き終えてから読む (OnImageFolderEvent)。
    const bool may_request = !host_.IsLoadingOtherDocument() && !image_watcher_.IsOpening(*abs_path);
    if (!needs_bitmap) {
        // 届いたビットマップも捨て、範囲に入ったら読み直す。ファイルが更新された画像は、行の高さが変わりうるので寸法を読み直す。
        if ((!info || info->stale) && may_request) {
            image_loader_.RequestSizeAsync(*abs_path);
        }
        return;
    }
    const bool stale = info && info->stale;
    if (ID2D1Bitmap* const received = delivered->Find(*abs_path)) {
        auto& bitmap = *inline_drawings_.images.TryEmplace(key).first;
        if (bitmap.Get() != received) {
            bitmap = received;
            update.bitmaps_changed = true;
        }
        // 読む間にファイルが更新されたビットマップは、出しておいてもう一度読む。
        if (!stale) {
            return;
        }
    }
    if (!may_request) {
        return;
    }
    // 描くビットマップが無ければ読む (保持範囲の外や別の文書から戻ったときは寸法が残っているので組み直さない)。
    // 持っている間は読み直さない。件数に上限のあるキャッシュに置いていた頃は、表の画像が 128 種類を超えると読み込みが
    // 互いを追い出し、約 1ms ごとに読み直しと描き直しが続いた (4db8079b)。
    // 表示幅が広がったら、今の解像度で描きながら高い解像度で読み直す。ファイルが更新された画像も、前の絵を描きながら
    // 読み直す (読めるまで寸法もビットマップも前のままなので、ちらつかない)。
    if (!inline_drawings_.FindImage(key) || stale || (info && image_loader_.NeedsSharperDecode(*info, max_width))) {
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
    // 図は届いた結果の反映も RequestRender が行うので、読み込み中に届いた結果は差し替えずに終わった後の要求で反映する。
    if (content_width <= 0.0f || host_.IsLoadingOtherDocument()) {
        return;
    }

    const bool dark_mode = state_.theme->dark;
    InvalidateDiagramsForRequestChange(content_width, dark_mode);
    DropQueuesIfJumped();
    // 拡大表示の図は本文の範囲の外にもあるので、範囲に依らず頼む。見ている窓の図なので本文より先に並べる。
    mermaid_.RequestViewerDrawing(content_width, dark_mode);
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
    const float node_width = layout::NodeContentWidth(node, *state_.theme, content_width);
    auto& entry = state_.document.layout_cache[i];
    entry.CommitMeasured(node_width, layout::DiagramNodeHeight(&diagram, node_width, entry.height, *state_.theme));
    changed.Add(i);
}

const std::wstring* ResourceManager::ResolveImagePath(std::string_view src, const std::wstring& doc_dir)
{
    auto it = resolved_image_paths_.find(src);
    if (it == resolved_image_paths_.end()) {
        it = resolved_image_paths_.emplace(std::string{ src }, ResolveLocalPath(src, doc_dir)).first;
        // 読むのは解決した後で、フォルダを開き終えるまでは読まない (IsOpening) ので、読んだ後の更新を取りこぼさない。
        if (!it->second.empty()) {
            image_watcher_.Watch(it->second, image_loader_.scheduler());
        }
    }
    return it->second.empty() ? nullptr : &it->second;
}

void ResourceManager::OnImageFolderEvent()
{
    const bool waiting = !changed_image_paths_.empty();
    const bool opened = image_watcher_.CheckForChanges(changed_image_paths_);
    // 書き込みの途中で読まないよう、最初の通知から間を置く (文書の読み直しと同じ)。待つ間の通知ではタイマーを延ばさない。
    // 延ばすと、短い間隔で書き出され続ける画像がいつまでも出ない。
    if (!waiting && !changed_image_paths_.empty()) {
        host_.SetTimer(app_timer::Id::IMAGE_RELOAD, app_timer::FILE_RELOAD_DEBOUNCE_MS);
    }
    if (opened) {
        LoadImages();
    }
}

void ResourceManager::OnImageReloadTimer()
{
    host_.KillTimer(app_timer::Id::IMAGE_RELOAD);
    for (const std::wstring& path : std::exchange(changed_image_paths_, {})) {
        image_loader_.Invalidate(path);
    }
    // 先読み範囲の画像はここで読み直しを頼む。範囲の外の画像は、入ったときの走査が頼む。
    LoadImages();
}

void ResourceManager::DropQueuesIfJumped()
{
    const float scroll_y = state_.view.viewport.GetScrollY();
    const float viewport_height = ComputePaneLayout(state_).md_rect.height;
    if (viewport_height > 0.0f && std::abs(scroll_y - last_request_scroll_) > viewport_height * EVICT_BUFFER_SCREENS) {
        DropQueues();
    }
    last_request_scroll_ = scroll_y;
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
