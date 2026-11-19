#include "mermaid.h"
#include "app_constants.h"
#include "d2d_util.h"
#include "diagram_raster.h"
#include "i18n.h"
#include "resource.h"
#include "task_scheduler.h"
#include "win32_util.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>

namespace {

// 保存・コピーの画像の 1 枚の画素数の上限。
constexpr double MAX_EXPORT_PIXELS = 64.0 * 1024 * 1024;

// 描いた図のラベルの数式の描画命令を、層と数式の順に集める。
std::vector<std::shared_ptr<const MathDrawing>> LabelMaths(const mermaid_util::DiagramDrawing& drawing)
{
    std::vector<std::shared_ptr<const MathDrawing>> maths;
    for (const auto& layer : drawing.layers) {
        for (const auto& math : layer.maths) {
            maths.push_back(math.drawing);
        }
    }
    return maths;
}

// 直列化した形から解いた描画命令 (数式は TeX だけ) に、LabelMaths で集めた描画命令を同じ順で差し込む。数が合わなければ false。
bool SetLabelMaths(mermaid_util::DiagramDrawing& drawing, std::span<const std::shared_ptr<const MathDrawing>> maths)
{
    size_t i = 0;
    for (auto& layer : drawing.layers) {
        for (auto& math : layer.maths) {
            if (i == maths.size()) {
                return false;
            }
            math.drawing = maths[i++];
        }
    }
    return i == maths.size();
}

// 幅なしの項目を max_width で使えるならそれを、使えなければ幅つきの項目を指す。どちらも無ければ end。
template <class Slots>
auto FindUsable(Slots& slots, uint64_t width_free_key, uint64_t sized_key, float max_width)
{
    const auto it = slots.find(width_free_key);
    if (it != slots.end() && mermaid_util::UsableAtWidth(it->second.item.result.width, it->second.item.result.any_width, max_width)) {
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
    return { .key = it->first, .item = &it->second.item };
}

const DiagramCache::Item* DiagramCache::Peek(uint64_t width_free_key, uint64_t sized_key, float max_width) const
{
    const auto it = FindUsable(slots_, width_free_key, sized_key, max_width);
    return it == slots_.end() ? nullptr : &it->second.item;
}

const DiagramCache::Item* DiagramCache::Peek(uint64_t key) const
{
    const auto it = slots_.find(key);
    return it == slots_.end() ? nullptr : &it->second.item;
}

void DiagramCache::Put(uint64_t key, Item item)
{
    Erase(key);
    const auto cost_of = [](const Item& i) noexcept {
        return ITEM_BYTES + (i.bytes ? i.bytes->size() : 0) + i.maths.size() * LABEL_MATH_BYTES + i.result.error.message.size() * sizeof(wchar_t);
    };
    // ほかを全部捨てても入らない図は、表示している間だけ持つ (手放したら JS で描き直す)。
    if (item.bytes && cost_of(item) > max_bytes_) {
        item.bytes = nullptr;
        item.maths.clear();
    }
    if (!item.bytes && item.result.error.empty() && !item.result.drawing) {
        return;
    }
    const size_t cost = cost_of(item);
    total_bytes_ += cost;
    slots_.emplace(key, Slot{ .item = std::move(item), .cost = cost, .last_used = ++lru_seq_ });
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
        DiagramResult& result = it->second.item.result;
        // 持ち主を増やすのは UI スレッドだけなので、1 と読めたら持ち主はこの表だけ (worker が手放す途中なら多めに読み、次の回に外す)。
        if (!result.drawing || result.drawing.use_count() > 1) {
            ++it;
            continue;
        }
        result.drawing = nullptr;
        result.commands = nullptr;
        // 手放した時を使った時とし、画面を離れたばかりの図を後まで残す。直列化した形の無い (上限より大きい) 図は項目ごと捨てる。
        it->second.last_used = ++lru_seq_;
        it = it->second.item.bytes ? std::next(it) : Erase(it);
    }
    Trim();
}

void DiagramCache::ResetDevice()
{
    for (auto it = slots_.begin(); it != slots_.end();) {
        DiagramResult& result = it->second.item.result;
        result.commands = nullptr;
        it = result.error.empty() ? std::next(it) : Erase(it);
    }
}

size_t DiagramCache::CommandLists() const noexcept
{
    return static_cast<size_t>(std::ranges::count_if(slots_, [](const auto& kv) static noexcept { return kv.second.item.result.commands != nullptr; }));
}

DiagramCache::Slots::iterator DiagramCache::Erase(Slots::iterator it) noexcept
{
    total_bytes_ -= it->second.cost;
    return slots_.erase(it);
}

void DiagramCache::Trim()
{
    // 解いた描画命令を持つ項目は、手放した後も持ち主がいる (表示中か worker が読んでいる) ので捨てない。それだけで上限を
    // 超えるなら超えたままにする。
    while (total_bytes_ > max_bytes_) {
        auto oldest = slots_.end();
        for (auto it = slots_.begin(); it != slots_.end(); ++it) {
            const Slot& slot = it->second;
            if (!slot.item.result.drawing && (oldest == slots_.end() || slot.last_used < oldest->second.last_used)) {
                oldest = it;
            }
        }
        if (oldest == slots_.end()) {
            return;
        }
        Erase(oldest);
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
    KillTimer(hwnd_, std::to_underlying(app_timer::Id::PELE_TRIM));
    CloseViewer();
}

void MermaidRenderer::Init(HWND hwnd, ID2D1Factory* d2d, IWICImagingFactory* wic, IDWriteFactory* dw, TaskScheduler& bg_scheduler, PeleFactory make_pele,
                           ViewerFactory open_viewer, std::move_only_function<void()> on_update, std::move_only_function<void()> on_viewer_failed)
{
    open_viewer_ = open_viewer;
    hwnd_ = hwnd;
    d2d_factory_ = d2d;
    wic_factory_ = wic;
    dw_factory_ = dw;
    bg_scheduler_ = &bg_scheduler;
    make_pele_ = std::move(make_pele);
    on_update_ = std::move(on_update);
    on_viewer_failed_ = std::move(on_viewer_failed);
}

void MermaidRenderer::TrimIdleRuntimes()
{
    KillTimer(hwnd_, std::to_underlying(app_timer::Id::PELE_TRIM));
    // 描いている途中のものは捨てない
    if (pele_) {
        pele_->Trim(bg_scheduler_);
    }
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target, ID3D11Device* d3d, ID2D1Device* device)
{
    render_target_ = render_target;
    d3d_device_ = d3d;
    d2d_device_ = device;
    // 旧デバイスで記録できなかったエラーも捨てて再試行させる。解いた描画命令は残し、記録し直す。
    cache_.ResetDevice();
    record_dc_ = CreateRecordContext(render_target);
    if (ViewerOpen()) {
        viewer_.view->SetDevice(d3d, device);
    }
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
    // 画面外の図のメモリが見た範囲に比例しないよう、描画命令ごと手放す。戻ったらキャッシュの直列化した形から解き直す (外れていれば
    // JS で描き直す) が、図の多い文書でメモリを返すほうをユーザーが選んだ。
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
    const std::string_view code = node.GetText();
    DisplayTarget hit;
    auto* item = FindOrRequest(code, max_width, dark_mode, &hit);
    if (!item) {
        return false;
    }
    // デバイスが変わって外したコマンドリストを、描画命令から記録し直す。worker で組んだ文字は記録時に捨てたので、
    // 初めて描いたときと同じく worker で組み直してから記録する。届くまではプレースホルダーを同じ寸法で出す。
    if (item->result.drawing && !item->result.commands) {
        if (inflight_.insert(hit.request_hash).second) {
            StartLoad(RenderRequest{ .code = std::string{ code }, .max_width = max_width, .dark_mode = dark_mode, .target = hit }, *item);
        }
        return false;
    }
    // エラーの結果は絵を持たないので、仮表示中の旧結果より新しいコードのエラーを見せる。置き換わる結果 (前のコード・幅・テーマ) は
    // 持ち主がいなくなれば、次の ReleaseIdleDrawings でキャッシュからも解いた形を外す。
    diagram_entry.SetResult(item->result);
    return true;
}

std::optional<mermaid_util::DiagramSize> MermaidRenderer::KnownSize(const Node& node, float max_width, bool dark_mode) const
{
    const auto key = DisplayTarget::Of(mermaid_util::DiagramCodeHash(node), max_width, dark_mode);
    const auto* item = cache_.Peek(key.width_free_hash, key.request_hash, max_width);
    // エラーの結果は寸法を持たない。
    if (!item || !(item->result.width > 0.0f)) {
        return std::nullopt;
    }
    return mermaid_util::DiagramSize{ item->result.width, item->result.height };
}

DiagramCache::Item* MermaidRenderer::FindOrRequest(std::string_view code, float max_width, bool dark_mode, DisplayTarget* cached_key)
{
    auto key = DisplayTarget::Of(mermaid_util::DiagramCodeHash(code), max_width, dark_mode);
    // 幅を混ぜないキーの結果 (いまの幅の上限より狭いか、どの幅でも使えるときだけ) を先に、無ければ幅つきのキーで引く。
    const auto hit = cache_.Find(key.width_free_hash, key.request_hash, max_width);
    key.width_free = hit.item && hit.key == key.width_free_hash;
    key.any_width = key.width_free && hit.item->result.any_width;
    auto* item = hit.item;
    // 解いた描画命令かエラーがあれば結果が出ている。直列化した形だけなら worker で解く。
    if (item && (item->result.drawing || !item->result.error.empty())) {
        if (cached_key) {
            *cached_key = key;
        }
        return item;
    }
    if (!inflight_.insert(key.request_hash).second) {
        PromoteWaiting(key.request_hash);
        return nullptr;
    }
    // 描画命令を解けなかったときは描画し直すので、キャッシュに当たってもコードを持たせる。
    RenderRequest req{ .code = std::string{ code }, .max_width = max_width, .dark_mode = dark_mode, .target = key };
    if (item && item->bytes) {
        StartLoad(std::move(req), *item);
    }
    else {
        StartRender(std::move(req));
    }
    return nullptr;
}

template <typename Work>
void MermaidRenderer::PostJob(RenderRequest req, JobWork kind, Work work)
{
    bg_scheduler_->Post([this, job = Job{ .req = std::move(req), .work = kind, .drop_gen = drop_gen_.load(std::memory_order_relaxed) }, work = std::move(work),
                         guard = latch_.Acquire()]() mutable {
        // インライン数式は取り消すと頼み直す契機が無いので、移動しても描く (DropQueued でも待ちに残す)。
        const bool dropped = std::holds_alternative<DisplayTarget>(job.req.target) && job.drop_gen != drop_gen_.load(std::memory_order_relaxed);
        job.outcome = dropped ? JobOutcome{ JobDropped{} } : work(job);
        if (job_results_.Push(std::move(job))) {
            ::PostMessageW(hwnd_, app_msg::MERMAID_JOB_DONE, 0, 0);
        }
    });
}

void MermaidRenderer::StartLoad(RenderRequest req, const DiagramCache::Item& item)
{
    // 解くのと文字を組むのは worker で行う (UI では時間予算なしに積み上がる)。ラベルの数式は覚えた描画命令を差し込むので、
    // KaTeX も pele も動かさない。
    PostJob(std::move(req), JobWork::Load,
            [this, drawing = item.result.drawing, bytes = item.bytes, maths = item.maths](Job&) mutable -> JobOutcome {
        if (!drawing) {
            auto parsed = mermaid_util::ParseDrawing(*bytes);
            if (!parsed || !SetLabelMaths(*parsed, maths)) {
                return JobUnreadable{};
            }
            drawing = std::make_shared<const mermaid_util::DiagramDrawing>(std::move(*parsed));
        }
        JobDrawing read{ .drawing = std::move(drawing), .bytes = std::move(bytes), .maths = std::move(maths) };
        read.texts = LayoutDiagramTexts(dw_factory_.Get(), *read.drawing, i18n::TextLocale());
        return read;
    });
}

Microsoft::WRL::ComPtr<ID2D1CommandList> MermaidRenderer::RecordDrawing(const mermaid_util::DiagramDrawing& drawing, const DiagramTextLayouts* texts)
{
    return RecordDiagram(record_dc_.Get(), dw_factory_.Get(), wic_factory_.Get(), drawing, i18n::TextLocale(), texts);
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

bool MermaidRenderer::ApplyJobResult(Job& r)
{
    if (std::holds_alternative<MathRequest>(r.req.target)) {
        // 結果は呼び出し側に直接渡し、図のキャッシュには入れない。
        ApplyMathResult(r);
        return false;
    }
    return ApplyDisplayResult(r);
}

bool MermaidRenderer::SettleDisplayError(const DisplayTarget& display, const DiagramError& error)
{
    // 枠に収まらない分は描くときに省略記号で切る (placeholder_text の書式)。pele の文言は長さに上限が無いので、
    // キャッシュと TextLayoutCache のキーに丸ごと持つ量だけをここで抑える。
    constexpr size_t MAX_ERROR_LEN = 1024;
    cache_.Put(display.request_hash, { .result = { .error = { error.kind, mermaid_util::SanitizeErrorMessage(error.message, MAX_ERROR_LEN) } } });
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
            DiagramResult result{ .commands = RecordDrawing(*d.drawing, &d.texts), .width = d.drawing->width, .height = d.drawing->height };
            // 記録した文字の並びはもう使わない (コマンドリストは並びを参照しない)。数が多いので worker で捨てる。
            if (!d.texts.runs.empty()) {
                DisposeOnWorker(*bg_scheduler_, std::move(d.texts));
            }
            if (!result.commands) {
                // 描いたばかりの図は描き直しても記録できないので確定させる。読んだ図は描き直す。
                if (rendered) {
                    return SettleDisplayError(display, { DiagramError::Kind::Render });
                }
                StartRender(std::move(r.req));
                return false;
            }
            if (rendered) {
                display.width_free = mermaid_util::IsWidthFree(result.width, r.req.max_width);
                if (!display.width_free && mermaid_util::IsFlowchartCode(r.req.code)) {
                    // 幅の上限より広いフローチャートは、前に別の幅で描いた描画命令と同じなら幅で出力が変わらない。前の幅の
                    // 直列化した形と比べる (捨てられていれば比べられず、この幅でも描く)。
                    const uint64_t* last = last_drawn_.Find(display.width_free_hash);
                    const DiagramCache::Item* previous = last && *last != display.request_hash ? cache_.Peek(*last) : nullptr;
                    display.any_width = previous && previous->bytes && *previous->bytes == *d.bytes;
                    // 前の幅の結果は、どの幅でも当たる幅なしのキーの結果と同じなので、もう引かれない。
                    if (display.any_width) {
                        cache_.Erase(*last);
                    }
                    display.width_free = display.any_width;
                    last_drawn_.Insert(display.width_free_hash, display.request_hash);
                }
            }
            result.any_width = display.any_width;
            result.drawing = std::move(d.drawing);
            cache_.Put(display.CacheKey(), { .result = std::move(result), .bytes = std::move(d.bytes), .maths = std::move(d.maths) });
            inflight_.erase(display.request_hash);
            return true;
        },
        [&](const JobFailed& f) {
            return SettleDisplayError(display, f.error);
        },
        // 描いた結果をキャッシュに置くので、次は解ける描画命令が当たる。
        [&](const JobUnreadable&) {
            StartRender(std::move(r.req));
            return false;
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
    // 前の回に置いた結果のうち、どの図も拡大表示も受け取らなかったもの (届く前に画面外へ出た図) の解いた形を手放す。受け取るのは
    // 通知の中で要求し直したときだが、後で要求し直す呼び出し側のために、置いた回の終わりではなく次の回の始まりで手放す。
    cache_.ReleaseIdleDrawings();
    bool resolved = false;
    bool render_done = false;
    job_results_.Drain(deadline, [&](Job& r) {
        if (r.work == JobWork::Render) {
            --renders_running_;
            render_done = true;
        }
        resolved |= ApplyJobResult(r);
    });
    if (render_done) {
        PumpRenders();
        // 描き終えて待ちが空になったらランタイムを 1 個まで返す (すぐ次が来ると作り直すが、早く返すほうをユーザーが選んだ)。
        if (renders_running_ == 0 && math_waiting_.empty() && display_waiting_.empty()) {
            pele_->Trim(bg_scheduler_, 1);
        }
        SetTimer(hwnd_, std::to_underlying(app_timer::Id::PELE_TRIM), PELE_TRIM_MS, nullptr);
    }
    // まとめて 1 回だけ要求し直させる。
    if (resolved) {
        on_update_();
    }
}

std::unique_ptr<PeleEngine> MermaidRenderer::CreateBundledPele(IDWriteFactory* dw)
{
    auto fonts = std::views::iota(0u, static_cast<UINT>(std::size(kKatexFontFiles))) |
                 std::views::transform([](UINT i) { return LoadRcData(IDR_KATEX_FONT_FIRST + i); }) | std::ranges::to<std::vector>();
    return std::make_unique<PeleEngine>(LoadRcData(IDR_PELE_JS), LoadRcData(IDR_KATEX_JS), std::move(fonts), dw, i18n::TextLocale());
}

void MermaidRenderer::EnsurePele()
{
    if (!pele_) {
        pele_ = make_pele_(dw_factory_.Get());
    }
}

void MermaidRenderer::StartRender(RenderRequest req)
{
    EnsurePele();
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
    // bg のワーカーを 1 本残し、配置の重い図が画像のデコードとキャッシュの直列化した形の読み込みを待たせないようにする。
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
    auto out = pele_->Render(job.req.code, job.req.dark_mode, job.req.max_width, { &drop_gen_, job.drop_gen });
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
    // キャッシュには直列化した形で置く。置いている間、書き足しで伸ばした余りを持たない。
    std::string bytes = mermaid_util::SerializeDrawing(out.drawing);
    bytes.shrink_to_fit();
    JobDrawing drawn{
        .bytes = std::make_shared<const std::string>(std::move(bytes)),
        .maths = LabelMaths(out.drawing),
    };
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

void MermaidRenderer::OpenViewer(std::string_view code, int node, bool dark_mode)
{
    const bool open = ViewerOpen();
    viewer_.node = node;
    if (open) {
        viewer_.view->BringToFront();
        if (viewer_.code == code) {
            return;
        }
    }
    viewer_.code.assign(code);
    if (open) {
        viewer_.view->ShowLoading();
    }
    else if (viewer_.view = open_viewer_(hwnd_, d3d_device_.Get(), d2d_device_.Get(), dw_factory_.Get(), wic_factory_.Get(), dark_mode, [this] { CloseViewer(); });
             !viewer_.view) {
        FailViewer();
        return;
    }
    AwaitViewerDrawing();
}

void MermaidRenderer::SetViewerDarkMode(bool dark_mode)
{
    if (!ViewerOpen()) {
        return;
    }
    viewer_.view->SetDarkMode(dark_mode);
    AwaitViewerDrawing();
}

void MermaidRenderer::UpdateViewerCode(std::string_view code)
{
    if (!ViewerOpen() || viewer_.code == code) {
        return;
    }
    viewer_.code.assign(code);
    AwaitViewerDrawing();
}

void MermaidRenderer::ApplyViewerLanguage()
{
    if (ViewerOpen()) {
        viewer_.view->ApplyLanguage();
    }
}

void MermaidRenderer::CloseViewer()
{
    // 窓も壊す。頼んだ図の描画は止めず、結果は本文と同じキャッシュに残る。
    viewer_ = {};
}

void MermaidRenderer::FailViewer()
{
    CloseViewer();
    on_viewer_failed_();
}

void MermaidRenderer::AwaitViewerDrawing()
{
    viewer_.pending = true;
    on_update_();
}

void MermaidRenderer::RequestViewerDrawing(float max_width, bool dark_mode)
{
    if (!viewer_.pending || !ViewerOpen()) {
        return;
    }
    const auto* item = FindOrRequest(viewer_.code, max_width, dark_mode);
    if (!item) {
        return;
    }
    viewer_.pending = false;
    if (const DiagramResult& result = item->result; result.drawing) {
        // デバイスが変わって外したコマンドリストは、窓が自分で記録する (ここで記録してキャッシュに入れると、本文の図が
        // 表示していなくても残る)。
        viewer_.view->SetDrawing(result.drawing, result.commands);
    }
    // 書き換えの途中の図はよく構文エラーになるので、出ている図があれば閉じずに残す。エラーの文言は本文の図に出る。
    else if (!viewer_.view->HasDrawing()) {
        FailViewer();
    }
}
