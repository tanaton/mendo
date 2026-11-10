#include "mermaid.h"
#include "app_constants.h"
#include "d2d_util.h"
#include "diagram_raster.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_file_cache.h"
#include "resource.h"
#include "string_convert.h"
#include "task_scheduler.h"
#include "win32_util.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <utility>

namespace {

// 保存・コピーの画像の 1 枚の画素数の上限。
constexpr double MAX_EXPORT_PIXELS = 64.0 * 1024 * 1024;

} // namespace

MermaidRenderer::~MermaidRenderer()
{
    Shutdown();
}

void MermaidRenderer::Shutdown()
{
    // 取り消した要求のコールバックは呼ばない (結果を待つ側も終わるところなので)。
    render_waiting_.clear();
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

void MermaidRenderer::Init(HWND hwnd, ID2D1Factory* d2d, IWICImagingFactory* wic, IDWriteFactory* dw, MermaidFileCache& file_cache, TaskScheduler& bg_scheduler,
                           PeleFactory make_pele, ViewerFactory open_viewer, std::move_only_function<void()> on_update,
                           std::move_only_function<void()> on_viewer_failed)
{
    open_viewer_ = open_viewer;
    hwnd_ = hwnd;
    file_cache_ = &file_cache;
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
    // 描いている途中のものは捨てない。図 4 個の文書で mendo の private bytes が 63 → 52MB、1 個で 51 → 48MB になる。
    if (pele_) {
        pele_->Trim();
    }
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target)
{
    render_target_ = render_target;
    // 旧デバイスで記録できなかったエラーも捨てて再試行させる。
    cache_.Clear();
    record_dc_ = CreateRecordContext(render_target);
}

void MermaidRenderer::InsertCache(uint64_t hash, DiagramResult result)
{
    cache_.Insert(hash, std::move(result));
    cache_.TrimToBudget(MAX_CACHE_BYTES, [](const DiagramResult& c) static noexcept { return c.drawing ? mermaid_util::DrawingBytes(*c.drawing) : 0; });
}

const DiagramResult* MermaidRenderer::FindCached(uint64_t request_hash, uint64_t width_free_hash, float max_width)
{
    const auto* free = cache_.Find(width_free_hash);
    return free && mermaid_util::IsWidthFree(free->width, max_width) ? free : cache_.Find(request_hash);
}

void MermaidRenderer::SetDiagramError(uint64_t request_hash, DiagramError::Kind kind, std::wstring_view msg)
{
    // 枠に収まらない分は描くときに省略記号で切る (placeholder_text の書式)。pele の文言は長さに上限が無いので、
    // キャッシュと TextLayoutCache のキーに丸ごと持つ量だけをここで抑える。
    constexpr size_t MAX_ERROR_LEN = 1024;
    InsertCache(request_hash, DiagramResult{
                                  .error = { kind, mermaid_util::SanitizeErrorMessage(msg, MAX_ERROR_LEN) }
    });
}

void MermaidRenderer::DropQueued()
{
    // 投入済みの表示用の図の描画とディスクキャッシュの読み込みも取り消す。
    drop_gen_.fetch_add(1, std::memory_order_relaxed);
    // インライン数式は幅にもテーマにも依らないので、移動しても描く価値が残る。
    std::erase_if(render_waiting_, [this](const RenderRequest& r) {
        const auto* display = std::get_if<DisplayTarget>(&r.target);
        if (!display) {
            return false;
        }
        inflight_.erase(display->request_hash);
        return true;
    });
    // 拡大表示が待っている図も捨てたので頼み直す。本文は見えている図を頼み直すが、拡大表示の図は本文の範囲の外にもある。
    // 投入済みで取り消した分は、JobDropped が戻ったときに頼み直す (ProcessJobResults)。
    PollViewerDrawing();
}

void MermaidRenderer::DropQueuedMath()
{
    std::erase_if(render_waiting_, [](const RenderRequest& r) { return std::holds_alternative<MathRequest>(r.target); });
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
    // エラーの結果は絵を持たないので、仮表示中の旧結果より新しいコードのエラーを見せる。
    diagram_entry.SetResult(*result);
    return true;
}

const DiagramResult* MermaidRenderer::FindOrRequest(std::string_view code, float max_width, bool dark_mode)
{
    const uint64_t code_hash = mermaid_util::DiagramCodeHash(code);
    const uint64_t hash = mermaid_util::DiagramRequestHash(code_hash, max_width, dark_mode);
    const uint64_t width_free_hash = mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode);
    if (const auto* cached = FindCached(hash, width_free_hash, max_width)) {
        return cached;
    }
    if (!inflight_.insert(hash).second) {
        return nullptr;
    }

    const auto hit = file_cache_->Find(width_free_hash, hash, max_width, /*touch=*/true);
    // ディスクから読めなかったときは描画し直すので、ディスクに当たってもコードを持たせる。
    RenderRequest req{
        .code = std::string{ code },
        .max_width = max_width,
        .dark_mode = dark_mode,
        .target = DisplayTarget{ .request_hash = hash, .width_free_hash = width_free_hash, .width_free = hit && hit->key == width_free_hash },
    };
    if (hit) {
        StartDiskLoad(std::move(req), file_cache_->FilePath(hit->key));
    }
    else {
        StartRender(std::move(req));
    }
    return nullptr;
}

template <typename Work>
void MermaidRenderer::PostJob(RenderRequest req, JobWork kind, Work work)
{
    bg_scheduler_->Post([this, job = Job{ .req = std::move(req), .work = kind }, work = std::move(work), drop_gen = drop_gen_.load(std::memory_order_relaxed),
                         guard = latch_.Acquire()]() mutable {
        // インライン数式は取り消すと頼み直す契機が無いので、移動しても描く (DropQueued でも待ちに残す)。
        const bool dropped = std::holds_alternative<DisplayTarget>(job.req.target) && drop_gen != drop_gen_.load(std::memory_order_relaxed);
        job.outcome = dropped ? JobOutcome{ JobDropped{} } : work(job);
        if (job_results_.Push(std::move(job))) {
            ::PostMessageW(hwnd_, app_msg::MERMAID_JOB_DONE, 0, 0);
        }
    });
}

void MermaidRenderer::StartDiskLoad(RenderRequest req, std::filesystem::path path)
{
    // 描画命令の図のラベルの数式を組み直すのに使う。
    EnsurePele();
    // ファイル読み込みと解析は worker で行う (UI では時間予算なしに積み上がる)。
    PostJob(std::move(req), JobWork::DiskLoad, [this, path = std::move(path)](Job& job) { return LoadDiskJob(job, path); });
}

MermaidRenderer::JobOutcome MermaidRenderer::LoadDiskJob(Job& job, const std::filesystem::path& path)
{
    DWORD read_error = 0;
    const auto data = ReadAllBytes(path.c_str(), &read_error);
    if (data.empty()) {
        return JobUnreadable{ read_error };
    }
    auto drawing = mermaid_util::ParseDrawing(string_convert::Utf8ToWide({ reinterpret_cast<const char*>(data.data()), data.size() }));
    if (!drawing) {
        return JobUnreadable{};
    }
    // 描画命令のラベルの数式は TeX だけを置いてあるので組み直す。
    job.used_pele = pele_->ResolveMaths(*drawing);
    return JobDrawing{ .drawing = std::make_shared<const mermaid_util::DiagramDrawing>(std::move(*drawing)) };
}

Microsoft::WRL::ComPtr<ID2D1CommandList> MermaidRenderer::RecordDrawing(const mermaid_util::DiagramDrawing& drawing)
{
    return RecordDiagram(record_dc_.Get(), dw_factory_.Get(), wic_factory_.Get(), drawing, i18n::TextLocale());
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
    SetDiagramError(display.request_hash, error.kind, error.message);
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
            DiagramResult result{ .commands = RecordDrawing(*d.drawing), .width = d.drawing->width, .height = d.drawing->height };
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
                file_cache_->StoreAsync(display.CacheKey(), result.width, result.height, std::move(d.bytes));
            }
            result.drawing = std::move(d.drawing);
            InsertCache(display.CacheKey(), std::move(result));
            inflight_.erase(display.request_hash);
            return true;
        },
        [&](const JobFailed& f) {
            return SettleDisplayError(display, f.error);
        },
        [&](const JobUnreadable& u) {
            if (u.read_error) {
                file_cache_->OnReadFailed(display.CacheKey(), u.read_error);
            }
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
    // デバイスの再作成を待つ間に受け取ると、記録できないだけの結果が失敗として確定する。
    // 数式の組は描き直しても同じと見なされ、計画を作り直すまで頼み直されない。描画先が戻ってから受け取る。
    if (!render_target_) {
        return;
    }
    bool resolved = false;
    bool render_done = false;
    bool used_pele = false;
    job_results_.Drain(deadline, [&](Job& r) {
        if (r.work == JobWork::Render) {
            --renders_running_;
            render_done = true;
        }
        used_pele |= r.used_pele;
        resolved |= ApplyJobResult(r);
    });
    if (used_pele) {
        SetTimer(hwnd_, std::to_underlying(app_timer::Id::PELE_TRIM), PELE_TRIM_MS, nullptr);
    }
    if (render_done) {
        PumpRenders();
    }
    // まとめて 1 回だけ要求し直させる。
    if (resolved) {
        PollViewerDrawing();
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
    render_waiting_.push_back(std::move(req));
    PumpRenders();
}

void MermaidRenderer::PumpRenders()
{
    // bg のワーカーを 1 本残し、配置の重い図 (1 図 0.8 秒ほど) が画像のデコードとディスクキャッシュの読み込みを待たせないようにする。
    // ランタイムも同時に描く数だけ作るので、メモリの上限にもなる。
    const int limit = std::max(1, static_cast<int>(bg_scheduler_->WorkerCount()) - 1);
    while (renders_running_ < limit && !render_waiting_.empty()) {
        ++renders_running_;
        auto req = std::move(render_waiting_.front());
        render_waiting_.pop_front();
        PostJob(std::move(req), JobWork::Render, [this](Job& j) { return RenderJob(j); });
    }
}

MermaidRenderer::JobOutcome MermaidRenderer::RenderJob(Job& job)
{
    using Status = PeleOutcome::Status;
    job.used_pele = true;
    if (std::holds_alternative<MathRequest>(job.req.target)) {
        const auto sources = std::get<MathRequest>(job.req.target).items | std::views::transform([](const auto& item) static noexcept {
            return PeleEngine::MathSource{ item.tex, item.font_size };
        }) | std::ranges::to<std::vector>();
        return JobMaths{ pele_->RenderMaths(sources) };
    }
    auto out = pele_->Render(job.req.code, job.req.dark_mode, job.req.max_width);
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
    // ディスクキャッシュには UTF-8 で置く (図 1 個 平均 9KB)。
    JobDrawing drawn{ .bytes = mermaid_util::SerializeDrawing(out.drawing) };
    drawn.drawing = std::make_shared<const mermaid_util::DiagramDrawing>(std::move(out.drawing));
    return drawn;
}

void MermaidRenderer::RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback)
{
    StartRender(RenderRequest{
        .target = MathRequest{ .items = std::move(items), .callback = std::move(callback) },
    });
}

void MermaidRenderer::OpenViewer(std::string_view code, int node, float max_width, bool dark_mode)
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
    viewer_.max_width = max_width;
    viewer_.dark_mode = dark_mode;
    if (open) {
        viewer_.view->ShowLoading();
    }
    else if (viewer_.view = open_viewer_(hwnd_, d2d_factory_.Get(), dw_factory_.Get(), wic_factory_.Get(), dark_mode, [this] { CloseViewer(); }); !viewer_.view) {
        FailViewer();
        return;
    }
    RequestViewerDrawing();
}

void MermaidRenderer::SetViewerDarkMode(bool dark_mode, float max_width)
{
    if (!ViewerOpen()) {
        return;
    }
    viewer_.max_width = max_width;
    viewer_.dark_mode = dark_mode;
    viewer_.view->SetDarkMode(dark_mode);
    RequestViewerDrawing();
}

void MermaidRenderer::UpdateViewerCode(std::string_view code, float max_width)
{
    if (!ViewerOpen() || viewer_.code == code) {
        return;
    }
    viewer_.code.assign(code);
    viewer_.max_width = max_width;
    RequestViewerDrawing();
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

void MermaidRenderer::RequestViewerDrawing()
{
    viewer_.pending = true;
    PollViewerDrawing();
}

void MermaidRenderer::PollViewerDrawing()
{
    if (!viewer_.pending || !ViewerOpen()) {
        return;
    }
    const auto* result = FindOrRequest(viewer_.code, viewer_.max_width, viewer_.dark_mode);
    if (!result) {
        return;
    }
    viewer_.pending = false;
    if (result->drawing) {
        viewer_.view->SetDrawing(result->drawing);
    }
    // 書き換えの途中の図はよく構文エラーになるので、出ている図があれば閉じずに残す。エラーの文言は本文の図に出る。
    else if (!viewer_.view->HasDrawing()) {
        FailViewer();
    }
}
