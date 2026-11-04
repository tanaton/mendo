#include "mermaid.h"
#include "app_constants.h"
#include "d2d_util.h"
#include "diagram_raster.h"
#include "pele_engine.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_file_cache.h"
#include "resource.h"
#include "string_convert.h"
#include "task_scheduler.h"
#include "utility.h"
#include "win32_util.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

constexpr std::wstring_view MERMAID_VIEWER_CLASS = L"mendo_MermaidViewer";
// 最初の大きさ。本体に対する割合。
constexpr int VIEWER_SIZE_PERCENT = 90;
// 保存・コピーの画像の 1 枚の画素数の上限。
constexpr double MAX_EXPORT_PIXELS = 64.0 * 1024 * 1024;

void BringToFront(HWND hwnd) noexcept
{
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetForegroundWindow(hwnd);
}

} // namespace

MermaidRenderer::~MermaidRenderer()
{
    Shutdown();
}

void MermaidRenderer::Shutdown()
{
    // SVG 専用リクエストのコールバックは cancelled=true で呼び、呼び出し元の状態をリセットさせる。
    for (auto& req : std::exchange(render_waiting_, {})) {
        FailCallbacksIfAny(req, true);
    }
    inflight_.clear();
    drop_gen_.fetch_add(1, std::memory_order_relaxed);
    job_results_.TakeAll();
    if (pele_) {
        pele_->Cancel();
    }
    latch_.Wait();
    // 待つ間に描き終えた SVG の要求も、取り消しで返す。
    for (auto& r : job_results_.TakeAll()) {
        FailCallbacksIfAny(r.req, true);
    }
    pele_.reset();
    renders_running_ = 0;
    KillTimer(hwnd_, std::to_underlying(app_timer::Id::PELE_TRIM));
    DestroyViewer();
}

void MermaidRenderer::Init(HWND hwnd, IWICImagingFactory* wic, MermaidFileCache& file_cache, TaskScheduler& bg_scheduler,
                           std::move_only_function<void()> on_update, std::move_only_function<void()> on_viewer_failed)
{
    hwnd_ = hwnd;
    file_cache_ = &file_cache;
    wic_factory_ = wic;
    bg_scheduler_ = &bg_scheduler;
    on_update_ = std::move(on_update);
    on_viewer_failed_ = std::move(on_viewer_failed);
}

void MermaidRenderer::OnPeleTrimTimer()
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

const DiagramResult* MermaidRenderer::FindWidthFree(uint64_t width_free_hash, float max_width)
{
    const auto* free = cache_.Find(width_free_hash);
    return free && mermaid_util::IsWidthFree(free->width, max_width) ? free : nullptr;
}

const DiagramResult* MermaidRenderer::FindCached(uint64_t request_hash, uint64_t width_free_hash, float max_width)
{
    const auto* free = FindWidthFree(width_free_hash, max_width);
    return free ? free : cache_.Find(request_hash);
}

std::shared_ptr<const mermaid_util::DiagramDrawing> MermaidRenderer::FindWidthFreeDrawing(std::string_view code, float max_width, bool dark_mode)
{
    const auto* free = FindWidthFree(mermaid_util::DiagramWidthFreeHash(mermaid_util::DiagramCodeHash(code), dark_mode), max_width);
    return free ? free->drawing : nullptr;
}

void MermaidRenderer::SetDiagramError(const RenderRequest& req, DiagramError::Kind kind, std::wstring_view msg)
{
    if (req.kind != RequestKind::Display) {
        return;
    }
    // 枠に収まらない分は描くときに省略記号で切る (placeholder_text の書式)。pele の文言は長さに上限が無いので、
    // キャッシュと TextLayoutCache のキーに丸ごと持つ量だけをここで抑える。
    constexpr size_t MAX_ERROR_LEN = 1024;
    InsertCache(req.request_hash, DiagramResult{
                                      .error = { kind, mermaid_util::SanitizeErrorMessage(msg, MAX_ERROR_LEN) }
    });
}

void MermaidRenderer::InvokeSvgCallbackIfAny(RenderRequest& req, std::wstring_view svg, bool cancelled)
{
    if (auto cb = std::move(req.svg_callback)) {
        cb(svg, cancelled);
    }
}

void MermaidRenderer::CancelMathCallbackIfAny(RenderRequest& req)
{
    if (!req.math) {
        return;
    }
    if (auto cb = std::move(req.math->callback)) {
        InlineMathBatchResult result{ .cancelled = true };
        result.items.reserve(req.math->items.size());
        for (const auto& item : req.math->items) {
            result.items.push_back({ .key = item.key });
        }
        cb(std::move(result));
    }
}

void MermaidRenderer::DropQueued()
{
    // 投入済みの表示用の図の描画とディスクキャッシュの読み込みも取り消す。
    drop_gen_.fetch_add(1, std::memory_order_relaxed);
    // インライン数式は幅にもテーマにも依らないので、移動しても描く価値が残る。SVG と拡大表示の要求は頼み直す仕組みが無いので残す。
    for (auto& req : TakePending(render_waiting_, RequestKind::Display)) {
        ReleaseInflight(req);
    }
}

void MermaidRenderer::DropQueuedMath()
{
    for (auto& req : TakePending(render_waiting_, RequestKind::Math)) {
        CancelMathCallbackIfAny(req);
    }
}

std::vector<MermaidRenderer::RenderRequest> MermaidRenderer::TakePending(std::deque<RenderRequest>& queue, RequestKind kind)
{
    const auto taken = std::ranges::stable_partition(queue, [kind](const RenderRequest& r) { return r.kind != kind; });
    std::vector<RenderRequest> out(std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
    queue.erase(taken.begin(), taken.end());
    return out;
}

void MermaidRenderer::ApplyResult(DiagramEntry& diagram_entry, const DiagramResult& result)
{
    // エラーの結果は絵を持たないので、仮表示中の旧結果より新しいコードのエラーを見せる。
    static_cast<DiagramResult&>(diagram_entry) = result;
    diagram_entry.stale = false;
}

bool MermaidRenderer::RequestRender(
    const Node& node,
    DiagramEntry& diagram_entry,
    float max_width, bool dark_mode)
{
    const uint64_t code_hash = mermaid_util::EnsureDiagramCodeHash(diagram_entry.code_hash, node);
    const uint64_t hash = mermaid_util::DiagramRequestHash(code_hash, max_width, dark_mode);
    const uint64_t width_free_hash = mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode);
    if (const auto* cached = FindCached(hash, width_free_hash, max_width)) {
        ApplyResult(diagram_entry, *cached);
        return true;
    }
    if (!inflight_.insert(hash).second) {
        return false;
    }

    // ディスクから読めなかったときは描画し直すので、ディスクに当たってもコードを持たせる。
    RenderRequest req{
        .code = std::string{ node.GetText() },
        .max_width = max_width,
        .dark_mode = dark_mode,
        .request_hash = hash,
        .width_free_hash = width_free_hash,
    };
    if (const auto hit = file_cache_->Find(width_free_hash, hash, max_width, /*touch=*/true)) {
        req.width_free = hit->key == width_free_hash;
        StartDiskLoad(std::move(req), file_cache_->FilePath(hit->key));
        return false;
    }
    StartRender(std::move(req));
    return false;
}

template <typename Work>
void MermaidRenderer::PostJob(Job job, Work work)
{
    bg_scheduler_->Post([this, job = std::move(job), work = std::move(work), drop_gen = drop_gen_.load(std::memory_order_relaxed),
                         guard = latch_.Acquire()]() mutable {
        // インライン数式・SVG・拡大表示は取り消すと頼み直す契機が無いので、移動しても描く (DropQueued でも待ちに残す)。
        job.dropped = job.req.kind == RequestKind::Display && drop_gen != drop_gen_.load(std::memory_order_relaxed);
        if (!job.dropped) {
            work(job);
        }
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
    PostJob(Job{ .req = std::move(req) }, [this, path = std::move(path), locale = std::wstring{ i18n::TextLocale() }](Job& job) {
        LoadDiskJob(job, path, locale);
    });
}

void MermaidRenderer::LoadDiskJob(Job& job, const std::filesystem::path& path, std::wstring_view locale)
{
    const auto data = ReadAllBytes(path.c_str(), &job.read_error);
    if (data.empty()) {
        job.read_failed = true;
        return;
    }
    // 描画命令のラベルの数式は TeX だけを置いてあるので組み直す。
    if (auto drawing = mermaid_util::ParseDrawing(string_convert::Utf8ToWide({ reinterpret_cast<const char*>(data.data()), data.size() }))) {
        job.used_pele = pele_->ResolveMaths(*drawing, locale);
        job.parsed = std::make_shared<const mermaid_util::DiagramDrawing>(std::move(*drawing));
    }
}

IDWriteFactory* MermaidRenderer::WriteFactory()
{
    if (!dw_factory_) {
        LogHrFailure("DWriteCreateFactory(diagram)",
                     DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_factory_.GetAddressOf())));
    }
    return dw_factory_.Get();
}

Microsoft::WRL::ComPtr<ID2D1CommandList> MermaidRenderer::RecordDrawing(const mermaid_util::DiagramDrawing& drawing)
{
    return RecordDiagram(record_dc_.Get(), WriteFactory(), wic_factory_.Get(), drawing, i18n::TextLocale());
}

Microsoft::WRL::ComPtr<IWICBitmap> MermaidRenderer::DiagramBitmap(const mermaid_util::DiagramDrawing& drawing)
{
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
    if (render_target_) {
        render_target_->GetFactory(&d2d);
    }
    // 大きな図はビットマップの最大辺と画素数の上限に収まるまで縮める。
    const float w = drawing.width;
    const float h = drawing.height;
    const float dpr = std::min({ DpiScaleFrom(static_cast<float>(GetDpiForWindow(hwnd_))), static_cast<float>(MaxBitmapDim(render_target_)) / std::max(w, h),
                                 static_cast<float>(std::sqrt(MAX_EXPORT_PIXELS / (static_cast<double>(w) * h))) });
    return RasterizeDiagram(d2d.Get(), WriteFactory(), wic_factory_.Get(), drawing, dpr, i18n::TextLocale());
}

void MermaidRenderer::ApplyMathResult(Job& r)
{
    // 終了で止めた (Cancel) ときは描けなかった数式と見分けられないので、取り消しとして返す。
    if (pele_->Cancelled()) {
        CancelMathCallbackIfAny(r.req);
        return;
    }
    auto& math = *r.req.math;
    auto cb = std::exchange(math.callback, {});
    if (!cb) {
        return;
    }
    InlineMathBatchResult result;
    result.items.reserve(math.items.size());
    for (size_t i = 0; i < math.items.size(); ++i) {
        auto& item = result.items.emplace_back(InlineMathBatchResult::Item{ .key = math.items[i].key, .drawing = std::move(r.maths[i]) });
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
    // 結果は呼び出し側に直接渡し、図のキャッシュには入れない。
    switch (r.req.kind) {
    case RequestKind::Math:
        ApplyMathResult(r);
        return false;
    case RequestKind::View:
        if (auto callback = std::move(r.req.drawing_callback); callback && !r.dropped) {
            callback(std::move(r.parsed));
        }
        return false;
    case RequestKind::Svg:
        InvokeSvgCallbackIfAny(r.req, r.svg, r.dropped);
        return false;
    case RequestKind::Display:
        break;
    }
    // 表示に要るなら要求し直される。
    if (r.dropped) {
        ReleaseInflight(r.req);
        return false;
    }
    if (r.parsed) {
        DiagramResult result{ .commands = RecordDrawing(*r.parsed), .width = r.parsed->width, .height = r.parsed->height };
        if (result.commands) {
            if (r.rendered) {
                r.req.width_free = mermaid_util::IsWidthFree(result.width, r.req.max_width);
                file_cache_->StoreAsync(r.req.CacheKey(), result.width, result.height, std::move(r.bytes));
            }
            result.drawing = std::move(r.parsed);
            InsertCache(r.req.CacheKey(), std::move(result));
            ReleaseInflight(r.req);
            return true;
        }
    }
    // 描いたばかりの図は描き直しても同じ結果になるので、エラーを確定させて再要求させない。
    if (r.rendered) {
        SetDiagramError(r.req, r.error.empty() ? DiagramError::Kind::Render : r.error.kind, r.error.message);
        ReleaseInflight(r.req);
        return true;
    }
    if (r.read_failed) {
        file_cache_->OnReadFailed(r.req.CacheKey(), r.read_error);
    }
    StartRender(std::move(r.req));
    return false;
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
        if (r.rendered) {
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
        on_update_();
    }
}

void MermaidRenderer::EnsurePele()
{
    if (pele_) {
        return;
    }
    std::vector<std::span<const std::byte>> fonts;
    fonts.reserve(std::size(kKatexFontFiles));
    for (UINT i = 0; i < std::size(kKatexFontFiles); ++i) {
        fonts.push_back(LoadRcData(IDR_KATEX_FONT_FIRST + i));
    }
    pele_ = std::make_unique<PeleEngine>(LoadRcData(IDR_PELE_JS), LoadRcData(IDR_KATEX_JS), std::move(fonts), WriteFactory());
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
        Job job{ .req = std::move(render_waiting_.front()), .rendered = true };
        render_waiting_.pop_front();
        PostJob(std::move(job), [this, locale = std::wstring{ i18n::TextLocale() }](Job& j) { RenderJob(j, locale); });
    }
}

void MermaidRenderer::RenderJob(Job& job, std::wstring_view locale)
{
    using Status = PeleOutcome::Status;
    job.used_pele = true;
    if (job.req.kind == RequestKind::Math) {
        std::vector<PeleEngine::MathSource> sources;
        sources.reserve(job.req.math->items.size());
        for (const auto& item : job.req.math->items) {
            sources.push_back({ item.tex, item.font_size });
        }
        job.maths = pele_->RenderMaths(sources, locale);
        return;
    }
    // 拡大表示とクリップボードは本文の幅に縮めない (pele の配置は本文と同じ幅で組む)。
    auto out = pele_->Render(job.req.code, job.req.dark_mode, job.req.max_width, locale, job.req.kind == RequestKind::Display);
    switch (out.status) {
    case Status::Cancelled:
        job.dropped = true;
        return;
    case Status::Unsupported:
        job.error = { DiagramError::Kind::Unsupported };
        return;
    case Status::Error:
        job.error = { DiagramError::Kind::Render, std::move(out.error) };
        return;
    case Status::Drawn:
        break;
    }
    if (job.req.kind == RequestKind::Svg) {
        job.svg = string_convert::Utf8ToWide(DrawingToSvg(out.drawing));
        return;
    }
    // ディスクキャッシュには UTF-8 で置く (図 1 個 平均 4KB)。
    if (job.req.kind == RequestKind::Display) {
        const auto utf8 = mermaid_util::SerializeDrawing(out.drawing);
        job.bytes = std::make_shared<const ByteBuffer>(std::span{ reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size() });
    }
    job.parsed = std::make_shared<const mermaid_util::DiagramDrawing>(std::move(out.drawing));
}

// 数式のラベルも KaTeX で描かず元の文字のまま書く (Office や Inkscape は MathML を読めない)。
void MermaidRenderer::RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback)
{
    // 本文の描画命令を使えるなら pele で描かないので、図の描画の待ちに並べない。
    if (auto drawing = FindWidthFreeDrawing(code, max_width, dark_mode)) {
        PostJob(Job{ .req = { .kind = RequestKind::Svg, .svg_callback = std::move(callback) } },
                [drawing = std::move(drawing)](Job& job) { job.svg = string_convert::Utf8ToWide(DrawingToSvg(*drawing)); });
        return;
    }
    StartRender(RenderRequest{
        .code = std::string{ code },
        .max_width = max_width,
        .dark_mode = dark_mode,
        .kind = RequestKind::Svg,
        .svg_callback = std::move(callback),
    });
}

void MermaidRenderer::RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback)
{
    StartRender(RenderRequest{
        .kind = RequestKind::Math,
        .math = std::make_unique<MathRequest>(MathRequest{ .items = std::move(items), .callback = std::move(callback) }),
    });
}

void MermaidRenderer::OpenViewer(std::string_view code, float max_width, bool dark_mode)
{
    if (viewer_.hwnd) {
        BringToFront(viewer_.hwnd);
        if (viewer_.code == code) {
            return;
        }
    }
    viewer_.code.assign(code);
    viewer_.max_width = max_width;
    viewer_.dark_mode = dark_mode;
    viewer_.has_image = false;
    if (viewer_.hwnd) {
        viewer_.view->ShowLoading();
    }
    else if (!CreateViewer()) {
        FailViewer();
        return;
    }
    RequestViewerDrawing();
}

void MermaidRenderer::CloseViewer()
{
    if (viewer_.hwnd) {
        DestroyViewer();
    }
}

void MermaidRenderer::SetViewerDarkMode(bool dark_mode)
{
    if (!viewer_.hwnd) {
        return;
    }
    viewer_.dark_mode = dark_mode;
    ApplyDarkModeToWindow(viewer_.hwnd, dark_mode);
    viewer_.view->SetDarkMode(dark_mode);
    RequestViewerDrawing();
}

void MermaidRenderer::UpdateViewerCode(std::string_view code)
{
    if (!viewer_.hwnd || viewer_.code == code) {
        return;
    }
    viewer_.code.assign(code);
    RequestViewerDrawing();
}

void MermaidRenderer::ApplyViewerLanguage()
{
    if (!viewer_.hwnd) {
        return;
    }
    SetWindowTextW(viewer_.hwnd, i18n::S().viewer_title.data());
    viewer_.view->ApplyLanguage();
}

void MermaidRenderer::DestroyViewer()
{
    InvalidateViewerDrawing();
    Viewer viewer = std::exchange(viewer_, {});
    viewer.view.reset();
    if (viewer.hwnd) {
        DestroyWindow(viewer.hwnd);
    }
}

void MermaidRenderer::FailViewer()
{
    CloseViewer();
    on_viewer_failed_();
}

LRESULT CALLBACK MermaidRenderer::ViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* const self = BindWindowInstance<MermaidRenderer>(hwnd, msg, lParam);
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto& viewer = self->viewer_;
    if (LRESULT result = 0; viewer.view && viewer.view->HandleMessage(msg, wParam, lParam, result)) {
        return result;
    }
    switch (msg) {
    case WM_CLOSE:
        self->CloseViewer();
        return 0;

    case WM_NCDESTROY:
        // 本体と一緒に壊されたときも、壊れた hwnd を持ち続けない。
        viewer.hwnd = nullptr;
        break;

    // 本体 (Win32Window) と同じく、開閉で DWM のアニメーションを出さない。閉じ方が多く (Esc・×・文書の切り替え・本体と
    // 一緒に) どれも破棄の中の非表示化を通るので、ここで隠す。本体の最小化でも一緒に隠れるので、解くのも表示を開く
    // 1 か所ではなくここで行う。WM_WINDOWPOSCHANGED は WM_SIZE を作るので既定の処理へ回す。
    case WM_WINDOWPOSCHANGING:
        if (reinterpret_cast<const WINDOWPOS*>(lParam)->flags & SWP_HIDEWINDOW) {
            SetCloaked(hwnd, TRUE);
        }
        break;

    case WM_WINDOWPOSCHANGED:
        if (reinterpret_cast<const WINDOWPOS*>(lParam)->flags & SWP_SHOWWINDOW) {
            UpdateWindow(hwnd);
            SetCloaked(hwnd, FALSE);
        }
        break;

    case WM_DPICHANGED: {
        const RECT& r = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool MermaidRenderer::CreateViewer()
{
    static const bool registered = RegisterWindowClass(
        MERMAID_VIEWER_CLASS.data(), ViewerWndProc, LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON)), LoadCursorW(nullptr, IDC_ARROW));
    RECT owner{};
    if (!registered || !GetWindowRect(hwnd_, &owner)) {
        return false;
    }
    const int owner_w = owner.right - owner.left;
    const int owner_h = owner.bottom - owner.top;
    const int w = MulDiv(owner_w, VIEWER_SIZE_PERCENT, 100);
    const int h = MulDiv(owner_h, VIEWER_SIZE_PERCENT, 100);
    viewer_.hwnd = CreateWindowExW(
        // 所有ウィンドウは既定でタスクバーにボタンが無く、最小化すると戻す手段が無い。図を見ながら編集するときにどかせるよう、
        // 最小化ボタンを消さずにタスクバーのボタンを出す。
        WS_EX_APPWINDOW,
        MERMAID_VIEWER_CLASS.data(),
        i18n::S().viewer_title.data(),
        WS_OVERLAPPEDWINDOW,
        owner.left + (owner_w - w) / 2, owner.top + (owner_h - h) / 2,
        w, h,
        hwnd_, nullptr, GetModuleHandleW(nullptr), this);
    if (!viewer_.hwnd) {
        return false;
    }
    ApplyDarkModeToWindow(viewer_.hwnd, viewer_.dark_mode);
    // 表示する前に作り、最初の描画から DiagramView が背景を塗る。
    viewer_.view = std::make_unique<DiagramView>(viewer_.hwnd, WriteFactory(), wic_factory_.Get());
    viewer_.view->SetDarkMode(viewer_.dark_mode);
    viewer_.view->ApplyLanguage();
    viewer_.view->ShowLoading();
    // 表示の中の WM_WINDOWPOSCHANGED で解く。
    SetCloaked(viewer_.hwnd, TRUE);
    ShowWindow(viewer_.hwnd, SW_SHOW);
    return true;
}

// 書き換えの途中の図はよく構文エラーになるので、出ている図があれば閉じずに残す。エラーの文言は本文の図に出る。
void MermaidRenderer::OnViewerResult(bool drawn)
{
    if (drawn) {
        viewer_.has_image = true;
    }
    else if (!viewer_.has_image) {
        FailViewer();
    }
}

void MermaidRenderer::RequestViewerDrawing()
{
    const unsigned int render = InvalidateViewerDrawing();
    if (auto drawing = FindWidthFreeDrawing(viewer_.code, viewer_.max_width, viewer_.dark_mode)) {
        viewer_.view->SetDrawing(std::move(drawing));
        OnViewerResult(true);
        return;
    }
    StartRender(RenderRequest{
        .code = viewer_.code,
        .max_width = viewer_.max_width,
        .dark_mode = viewer_.dark_mode,
        .kind = RequestKind::View,
        .drawing_callback =
            [this, render](std::shared_ptr<const mermaid_util::DiagramDrawing> drawing) {
                if (render != viewer_render_) {
                    return;
                }
                const bool drawn = drawing != nullptr;
                if (drawn) {
                    viewer_.view->SetDrawing(std::move(drawing));
                }
                OnViewerResult(drawn);
            },
    });
}
