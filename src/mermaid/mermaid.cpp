#include "mermaid.h"
#include "d2d_util.h"
#include "diagram_raster.h"
#include "i18n.h"
#include "resource.h"
#include "task_scheduler.h"
#include "win32_util.h"
#include <wrl/client.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <utility>

namespace {

// 保存・コピーの画像の 1 枚の画素数の上限。
constexpr double MAX_EXPORT_PIXELS = 64.0 * 1024 * 1024;

// 幅なしの項目を max_width で使えるならそれを、使えなければ幅つきの項目を指す。どちらも無ければ end。
template <class Slots>
auto FindUsable(Slots& slots, uint64_t width_free_key, uint64_t sized_key, float max_width)
{
    const auto it = slots.find(width_free_key);
    if (it != slots.end() && mermaid_util::UsableAtWidth(it->second.result.width, it->second.result.any_width, max_width)) {
        return it;
    }
    return slots.find(sized_key);
}

} // namespace

DiagramCache::Hit DiagramCache::Find(uint64_t width_free_key, uint64_t sized_key, float max_width)
{
    const auto it = FindUsable(slots_, width_free_key, sized_key, max_width);
    if (it == slots_.end()) {
        return {};
    }
    it->second.last_used = ++lru_seq_;
    return { .key = it->first, .result = &it->second.result };
}

const DiagramResult* DiagramCache::Peek(uint64_t width_free_key, uint64_t sized_key, float max_width) const
{
    const auto it = FindUsable(slots_, width_free_key, sized_key, max_width);
    return it == slots_.end() ? nullptr : &it->second.result;
}

const DiagramResult* DiagramCache::Peek(uint64_t key) const
{
    const auto it = slots_.find(key);
    return it == slots_.end() ? nullptr : &it->second.result;
}

void DiagramCache::Put(uint64_t key, DiagramResult result)
{
    assert(result.drawing || !result.error.empty());
    const auto& drawing = result.drawing;
    size_t cost = ITEM_BYTES + result.error.message.size() * sizeof(wchar_t);
    bool oversized = false;
    if (drawing) {
        size_t bytes = mermaid_util::DrawingBytes(*drawing);
        for (const auto& layer : drawing->layers) {
            bytes += layer.maths.size() * LABEL_MATH_BYTES;
        }
        // ほかを全部捨てても入らない図は、上限に数えずに表示している間だけ持つ (手放したら JS で描き直す)。
        oversized = cost + bytes > max_bytes_;
        cost += oversized ? 0 : bytes;
    }
    Erase(key);
    total_bytes_ += cost;
    slots_.emplace(key, Slot{ .result = std::move(result), .cost = cost, .last_used = ++lru_seq_, .oversized = oversized });
}

void DiagramCache::Erase(uint64_t key)
{
    if (const auto it = slots_.find(key); it != slots_.end()) {
        Erase(it);
    }
}

void DiagramCache::ReleaseIdleDrawings()
{
    for (auto it = slots_.begin(); it != slots_.end();) {
        Slot& slot = it->second;
        // 手放すもの (コマンドリストか、上限より大きい図) の残る項目だけを見る。手放し終えた項目の last_used は進め直さない。
        if ((!slot.result.commands && !slot.oversized) || slot.HeldOutside()) {
            ++it;
        }
        else if (slot.oversized) {
            it = Erase(it);
        }
        else {
            slot.result.commands = nullptr;
            // 手放した時を使った時とし、画面を離れたばかりの図を後まで残す。
            slot.last_used = ++lru_seq_;
            ++it;
        }
    }
    Trim();
}

void DiagramCache::ResetDevice()
{
    for (auto it = slots_.begin(); it != slots_.end();) {
        DiagramResult& result = it->second.result;
        result.commands = nullptr;
        it = result.error.empty() ? std::next(it) : Erase(it);
    }
}

size_t DiagramCache::CommandLists() const noexcept
{
    return static_cast<size_t>(std::ranges::count_if(slots_, [](const auto& kv) static noexcept { return kv.second.result.commands != nullptr; }));
}

DiagramCache::Slots::iterator DiagramCache::Erase(Slots::iterator it) noexcept
{
    total_bytes_ -= it->second.cost;
    return slots_.erase(it);
}

void DiagramCache::Trim()
{
    if (total_bytes_ <= max_bytes_) {
        return;
    }
    // 表の外に持ち主がいる項目 (表示中か worker が組んでいる) は捨てない。それだけで上限を超えるなら超えたままにする。
    // 候補は 1 回だけ集めて古い順に取り出す (1 件ごとに表を探し直すと、大きな図を手放して小さな項目をまとめて捨てるときに
    // 件数の 2 乗かかる)。
    std::vector<std::pair<uint64_t, Slots::iterator>> candidates;
    for (auto it = slots_.begin(); it != slots_.end(); ++it) {
        if (!it->second.HeldOutside()) {
            candidates.emplace_back(it->second.last_used, it);
        }
    }
    const auto newer = [](const auto& a, const auto& b) static noexcept { return a.first > b.first; };
    std::ranges::make_heap(candidates, newer);
    while (total_bytes_ > max_bytes_ && !candidates.empty()) {
        std::ranges::pop_heap(candidates, newer);
        Erase(candidates.back().second);
        candidates.pop_back();
    }
}

MermaidRenderer::MermaidRenderer() = default;

MermaidRenderer::~MermaidRenderer()
{
    Shutdown();
}

void MermaidRenderer::Shutdown()
{
    // 取り消した要求のコールバックは呼ばない (結果を待つ側も終わるところなので)。
    math_waiting_.clear();
    display_waiting_.clear();
    pass_count_ = 0;
    inflight_.clear();
    drop_gen_.fetch_add(1, std::memory_order_relaxed);
    if (pele_) {
        pele_->Cancel();
    }
    latch_.Wait();
    job_results_.TakeAll();
    pele_.reset();
    renders_running_ = 0;
}

void MermaidRenderer::Init(HWND hwnd, ID2D1Factory* d2d, IWICImagingFactory* wic, IDWriteFactory* dw, TaskScheduler& bg_scheduler, PeleFactory make_pele,
                           Hooks hooks)
{
    hwnd_ = hwnd;
    d2d_factory_ = d2d;
    wic_factory_ = wic;
    dw_factory_ = dw;
    bg_scheduler_ = &bg_scheduler;
    make_pele_ = std::move(make_pele);
    hooks_ = std::move(hooks);
}

void MermaidRenderer::TrimIdleRuntimes()
{
    hooks_.kill_trim_timer();
    // 描いている途中のものは捨てない
    if (pele_) {
        pele_->Trim(bg_scheduler_);
    }
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target)
{
    render_target_ = render_target;
    // 旧デバイスで記録できなかったエラーも捨てて再試行させる。描画命令は残し、記録し直す。
    cache_.ResetDevice();
    record_dc_ = CreateRecordContext(render_target);
}

void MermaidRenderer::DropQueued()
{
    drop_gen_.fetch_add(1, std::memory_order_relaxed);
    // インライン数式は幅にもテーマにも依らないので、移動しても描く価値が残る。
    for (const RenderRequest& r : display_waiting_) {
        inflight_.erase(std::get<DisplayTarget>(r.target).request_hash);
    }
    display_waiting_.clear();
    pass_count_ = 0;
}

void MermaidRenderer::BeginRequestPass()
{
    pass_count_ = 0;
}

void MermaidRenderer::PromoteWaiting(uint64_t request_hash)
{
    const auto front = display_waiting_.begin() + static_cast<ptrdiff_t>(pass_count_);
    const auto it = std::find_if(front, display_waiting_.end(), [request_hash](const RenderRequest& r) {
        return std::get<DisplayTarget>(r.target).request_hash == request_hash;
    });
    if (it != display_waiting_.end()) {
        std::rotate(front, it, it + 1);
        ++pass_count_;
    }
}

std::vector<uint64_t> MermaidRenderer::WaitingRequests() const
{
    std::vector<uint64_t> hashes(math_waiting_.size(), 0);
    for (const RenderRequest& r : display_waiting_) {
        hashes.push_back(std::get<DisplayTarget>(r.target).request_hash);
    }
    return hashes;
}

void MermaidRenderer::ReleaseIdleDrawings()
{
    // 画面外の図のメモリが見た範囲に比例しないよう、コマンドリストは手放し、描画命令は上限までしか持たない。戻ったら記録し直す
    // (外れていれば JS で描き直す) が、図の多い文書でメモリを返すほうをユーザーが選んだ。
    cache_.ReleaseIdleDrawings();
}

void MermaidRenderer::DropQueuedMath()
{
    math_waiting_.clear();
}

bool MermaidRenderer::RequestRender(
    const Node& node,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode)
{
    const auto* result = FindOrRequest(node.GetText(), max_width, dark_mode);
    if (!result) {
        return false;
    }
    // エラーの結果は絵を持たないので、仮表示中の旧結果より新しいコードのエラーを見せる。置き換わる結果 (前のコード・幅・テーマ) は
    // 持ち主がいなくなれば、この要求の回の終わりの ReleaseIdleDrawings でキャッシュからもコマンドリストを外す。
    diagram_entry.SetResult(*result);
    return true;
}

std::optional<mermaid_util::DiagramSize> MermaidRenderer::KnownSize(const Node& node, float max_width, bool dark_mode) const
{
    const auto key = DisplayTarget::Of(mermaid_util::DiagramCodeHash(node), max_width, dark_mode);
    const auto* result = cache_.Peek(key.width_free_hash, key.request_hash, max_width);
    // エラーの結果は寸法を持たない。
    if (!result || !(result->width > 0.0f)) {
        return std::nullopt;
    }
    return mermaid_util::DiagramSize{ result->width, result->height };
}

const DiagramResult* MermaidRenderer::FindOrRequest(std::string_view code, float max_width, bool dark_mode)
{
    auto key = DisplayTarget::Of(mermaid_util::DiagramCodeHash(code), max_width, dark_mode);
    // 幅を混ぜないキーの結果 (いまの幅の上限より狭いか、どの幅でも使えるときだけ) を先に、無ければ幅つきのキーで引く。
    const auto [hit_key, result] = cache_.Find(key.width_free_hash, key.request_hash, max_width);
    if (result && (result->commands || !result->error.empty())) {
        return result;
    }
    if (!inflight_.insert(key.request_hash).second) {
        PromoteWaiting(key.request_hash);
        return nullptr;
    }
    // 覚えていた図は記録し直して、当たったキーへ置き直す。
    key.width_free = result && hit_key == key.width_free_hash;
    key.any_width = key.width_free && result->any_width;
    // 記録できなかったときは描き直すので、キャッシュに当たってもコードを持たせる。
    RenderRequest req{ .code = std::string{ code }, .max_width = max_width, .dark_mode = dark_mode, .target = key };
    // 画面外へ出たかデバイスが変わってコマンドリストの無い図は、文字を組んで記録し直す。
    if (result) {
        StartLayout(std::move(req), result->drawing);
    }
    else {
        StartRender(std::move(req));
    }
    return nullptr;
}

template <typename Work>
void MermaidRenderer::PostJob(RenderRequest req, JobWork kind, Work work)
{
    // インライン数式は取り消すと頼み直す契機が無いので、移動しても描く (stale は空のまま。DropQueued でも待ちに残す)。
    PeleEngine::StaleWhen stale;
    if (std::holds_alternative<DisplayTarget>(req.target)) {
        stale = { &drop_gen_, drop_gen_.load(std::memory_order_relaxed) };
    }
    bg_scheduler_->Post([this, job = Job{ .req = std::move(req), .work = kind, .stale = stale }, work = std::move(work),
                         guard = latch_.Acquire()]() mutable {
        job.outcome = job.stale.Changed() ? JobOutcome{ JobDropped{} } : work(job);
        if (job_results_.Push(std::move(job)) && hooks_.wake) {
            hooks_.wake();
        }
    });
}

void MermaidRenderer::StartLayout(RenderRequest req, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing)
{
    // 文字は worker で組む (UI では時間予算なしに積み上がる)。KaTeX も pele も動かさない。
    PostJob(std::move(req), JobWork::Layout, [this, drawing = std::move(drawing)](Job&) mutable -> JobOutcome {
        JobDrawing laid_out{ .drawing = std::move(drawing) };
        laid_out.texts = LayoutDiagramTexts(dw_factory_.Get(), *laid_out.drawing, i18n::TextLocale());
        return laid_out;
    });
}

Microsoft::WRL::ComPtr<IWICBitmap> MermaidRenderer::DiagramBitmap(const mermaid_util::DiagramDrawing& drawing, float max_width)
{
    const auto size = mermaid_util::FittedDiagramSize(drawing, max_width);
    // 大きな図はビットマップの最大辺と画素数の上限に収まるまで縮める。
    const float w = size.width;
    const float h = size.height;
    const float dpr = std::min({ DpiScaleFrom(static_cast<float>(GetDpiForWindow(hwnd_))), static_cast<float>(MaxBitmapDim(render_target_)) / std::max(w, h),
                                 static_cast<float>(std::sqrt(MAX_EXPORT_PIXELS / (static_cast<double>(w) * h))) });
    return RasterizeDiagram(d2d_factory_.Get(), dw_factory_.Get(), wic_factory_.Get(), drawing, size, dpr, i18n::TextLocale());
}

void MermaidRenderer::ApplyMathResult(Job& r)
{
    auto& math = std::get<MathRequest>(r.req.target);
    auto cb = std::exchange(math.callback, {});
    if (!cb) {
        return;
    }
    // 数式は取り消さず (drop_gen_ は表示用だけ)、終了で取り消した数式も null で返る。
    auto& drawings = std::get<JobMaths>(r.outcome).drawings;
    InlineMathBatchResult result;
    result.items.reserve(math.items.size());
    for (size_t i = 0; i < math.items.size(); ++i) {
        auto& item = result.items.emplace_back(InlineMathBatchResult::Item{ .key = math.items[i].key, .drawing = std::move(drawings[i]) });
        if (const auto& d = item.drawing; d && d->width > 0.0f) {
            item.metrics = { d->width, d->height + d->depth, d->height };
        }
        else {
            item.drawing = nullptr;
        }
    }
    cb(std::move(result));
}

bool MermaidRenderer::SettleDisplayError(const DisplayTarget& display, const DiagramError& error)
{
    // 枠に収まらない分は描くときに省略記号で切る (placeholder_text の書式)。pele の文言は長さに上限が無いので、
    // キャッシュと TextLayoutCache のキーに丸ごと持つ量だけをここで抑える。
    constexpr size_t MAX_ERROR_LEN = 1024;
    cache_.Put(display.request_hash, { .error = { error.kind, mermaid_util::SanitizeErrorMessage(error.message, MAX_ERROR_LEN) } });
    inflight_.erase(display.request_hash);
    return true;
}

bool MermaidRenderer::ApplyDisplayResult(Job& r)
{
    const bool rendered = r.work == JobWork::Render;
    // r.req の中を指すので、r.req を StartRender に渡した後は使わない。
    auto& display = std::get<DisplayTarget>(r.req.target);
    // clang-format off
    return std::visit(overloaded{
        // 取り消してから戻るまでに来た同じ要求は inflight_ で断っているので、要求し直させる。
        [&](JobDropped&) {
            inflight_.erase(display.request_hash);
            return true;
        },
        [&](JobDrawing& d) {
            // 描画中の Renderer とは別の DeviceContext で記録する。記録できなければ null。
            DiagramResult result{ .commands = RecordDiagram(record_dc_.Get(), dw_factory_.Get(), wic_factory_.Get(), *d.drawing, i18n::TextLocale(), &d.texts),
                                  .width = d.drawing->width,
                                  .height = d.drawing->height };
            // 記録した文字の並びはもう使わない (コマンドリストは並びを参照しない)。数が多いので worker で捨てる。
            if (!d.texts.runs.empty()) {
                DisposeOnWorker(*bg_scheduler_, std::move(d.texts));
            }
            if (!result.commands) {
                // 描いたばかりの図は描き直しても記録できないので確定させる。覚えていた図は項目を捨てて描き直す。残すと、幅なしの
                // キーの項目がエラー (幅つきのキー) より先に当たり、要求が止まらない。
                if (rendered) {
                    return SettleDisplayError(display, { DiagramError::Kind::Render });
                }
                cache_.Erase(display.CacheKey());
                display.any_width = false;
                StartRender(std::move(r.req));
                return false;
            }
            if (rendered) {
                display.width_free = mermaid_util::IsWidthFree(result.width, r.req.max_width);
                if (!display.width_free && mermaid_util::IsFlowchartCode(r.req.code)) {
                    // 幅の上限より広いフローチャートは、前に別の幅で描いた描画命令と同じなら幅で出力が変わらない。
                    const auto [last, added] = last_drawn_.TryEmplaceBounded(display.width_free_hash, MAX_LAST_DRAWN);
                    display.any_width = !added && last->request_hash != display.request_hash && last->drawing_hash == d.drawing_hash;
                    // 前の幅の結果は、どの幅でも当たる幅なしのキーの結果と同じなので、もう引かれない。
                    if (display.any_width) {
                        cache_.Erase(last->request_hash);
                    }
                    display.width_free = display.any_width;
                    *last = { .request_hash = display.request_hash, .drawing_hash = d.drawing_hash };
                }
            }
            result.any_width = display.any_width;
            result.drawing = std::move(d.drawing);
            cache_.Put(display.CacheKey(), std::move(result));
            inflight_.erase(display.request_hash);
            return true;
        },
        [&](const JobFailed& f) {
            return SettleDisplayError(display, f.error);
        },
        // 表示用の要求には来ない。
        [](const JobMaths&) -> bool { std::unreachable(); },
    }, r.outcome);
    // clang-format on
}

void MermaidRenderer::ProcessJobResults(std::chrono::steady_clock::time_point deadline)
{
    // デバイスの再作成を待つ間に受け取ると、記録できないだけの結果が失敗として確定する
    if (!render_target_) {
        return;
    }
    // 置いた結果のコマンドリストは、ここでは手放さない。通知 (Hooks::on_update) の中の要求の回で図が受け取り、受け取らなかったもの
    // (届く前に画面外へ出た図) と置き換わった結果は、その回の終わりの ReleaseIdleDrawings で手放す。
    bool resolved = false;
    bool render_done = false;
    job_results_.Drain(deadline, [&](Job& r) {
        if (r.work == JobWork::Render) {
            --renders_running_;
            render_done = true;
        }
        // 数式の結果は呼び出し側に直接渡し、図のキャッシュには入れない。
        if (std::holds_alternative<MathRequest>(r.req.target)) {
            ApplyMathResult(r);
        }
        else {
            resolved |= ApplyDisplayResult(r);
        }
    });
    if (render_done) {
        PumpRenders();
        // 描き終えて待ちが空になったらランタイムを 1 個まで返す (すぐ次が来ると作り直すが、早く返すほうをユーザーが選んだ)。
        if (renders_running_ == 0 && math_waiting_.empty() && display_waiting_.empty()) {
            pele_->Trim(bg_scheduler_, 1);
        }
        hooks_.set_trim_timer(PELE_TRIM_MS);
    }
    // まとめて 1 回だけ要求し直させる。
    if (resolved) {
        hooks_.on_update();
    }
}

std::unique_ptr<PeleEngine> MermaidRenderer::CreateBundledPele(IDWriteFactory* dw)
{
    auto fonts = std::views::iota(0u, static_cast<UINT>(std::size(kKatexFontFiles))) |
                 std::views::transform([](UINT i) { return LoadRcData(IDR_KATEX_FONT_FIRST + i); }) | std::ranges::to<std::vector>();
    return std::make_unique<PeleEngine>(LoadRcData(IDR_PELE_JS), LoadRcData(IDR_KATEX_JS), std::move(fonts), dw, i18n::TextLocale());
}

void MermaidRenderer::StartRender(RenderRequest req)
{
    // 最初に図か数式を描くときに作る。bg のワーカーが使うので、UI スレッドで投げる前に作る。
    if (!pele_) {
        pele_ = make_pele_(dw_factory_.Get());
    }
    if (std::holds_alternative<MathRequest>(req.target)) {
        math_waiting_.push_back(std::move(req));
    }
    else {
        display_waiting_.insert(display_waiting_.begin() + static_cast<ptrdiff_t>(pass_count_), std::move(req));
        ++pass_count_;
    }
    PumpRenders();
}

void MermaidRenderer::PumpRenders()
{
    // bg のワーカーを 1 本残し、配置の重い図が画像のデコードと戻った図の文字の組みを待たせないようにする。
    // 同時に描く数はランタイムの数、つまりメモリの上限にもなる。
    const int limit = std::max(1, static_cast<int>(bg_scheduler_->WorkerCount()) - 1);
    while (renders_running_ < limit && !(math_waiting_.empty() && display_waiting_.empty())) {
        ++renders_running_;
        // 数式は行の高さを決めるので、表示用の図より先に描く。
        auto& waiting = math_waiting_.empty() ? display_waiting_ : math_waiting_;
        if (&waiting == &display_waiting_ && pass_count_ > 0) {
            --pass_count_;
        }
        auto req = std::move(waiting.front());
        waiting.pop_front();
        PostJob(std::move(req), JobWork::Render, [this](Job& j) { return RenderJob(j); });
    }
}

MermaidRenderer::JobOutcome MermaidRenderer::RenderJob(Job& job)
{
    using Status = PeleOutcome::Status;
    if (std::holds_alternative<MathRequest>(job.req.target)) {
        const auto sources = std::get<MathRequest>(job.req.target).items | std::views::transform([](const auto& item) static noexcept {
            return PeleEngine::MathSource{ item.tex, item.font_size };
        }) | std::ranges::to<std::vector>();
        return JobMaths{ pele_->RenderMaths(sources) };
    }
    auto out = pele_->Render(job.req.code, job.req.dark_mode, job.req.max_width, job.stale);
    switch (out.status) {
    case Status::Cancelled:
        return JobDropped{};
    case Status::Unsupported:
        return JobFailed{ .error = { DiagramError::Kind::Unsupported } };
    case Status::Error:
        return JobFailed{
            .error = { .kind = DiagramError::Kind::Render, .message = std::move(out.error) }
        };
    case Status::Drawn:
        break;
    }
    JobDrawing drawn{ .drawing_hash = mermaid_util::DrawingHash(out.drawing) };
    drawn.drawing = std::make_shared<const mermaid_util::DiagramDrawing>(std::move(out.drawing));
    drawn.texts = LayoutDiagramTexts(dw_factory_.Get(), *drawn.drawing, i18n::TextLocale());
    return drawn;
}

void MermaidRenderer::RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback)
{
    StartRender(RenderRequest{
        .target = MathRequest{ .items = std::move(items), .callback = std::move(callback) },
    });
}
