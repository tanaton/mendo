#include "preloader.h"
#include "document_service.h"
#include "profiler.h"

void Preloader::Context::SignalAbort()
{
    {
        const std::lock_guard lk(mtx);
        aborted = true;
    }
    cv.notify_all();
}

Preloader::~Preloader()
{
    Join();
}

void Preloader::Start(std::pmr::wstring path)
{
    // 二重呼び出し防御: 前回 worker が cv.wait 中に thread_ を再代入すると、
    // jthread の暗黙 request_stop は cv を notify しないので join が無限ブロックする。
    Join();

    auto ctx = std::make_shared<Context>();
    ctx_ = ctx;

    // EstimateNodeHeights は Theme 依存のためここでは行わず、OnParseComplete 経由で
    // FinishLoadMarkdownFile が後で実行する。
    thread_ = std::jthread([this, ctx, path = std::move(path)](std::stop_token st) mutable {
        MENDO_PROFILE("Preload::worker");

        // stop_token なしだと Parse 完走まで終了時の join が数百 ms ブロックする。
        auto load_result = DocumentService::LoadFile(path, st);
        // sink 書き込み前に abort 確認。直後の publish + main 側即破棄を回避。
        if (st.stop_requested()) {
            return;
        }
        if (load_result) {
            LayoutCache cache;
            cache.Reset(load_result->GetNodes().size(), /* shrink = */ false);
            const std::lock_guard lock(sink_mutex_);
            result_.emplace(AsyncLoadResult{ std::move(*load_result), std::move(cache), /* heights_estimated = */ false });
        }
        else {
            const std::lock_guard lock(sink_mutex_);
            error_ = load_result.error();
        }

        std::unique_lock lk(ctx->mtx);
        ctx->cv.wait(lk, [&] { return ctx->hwnd != nullptr || ctx->aborted || st.stop_requested(); });
        if (ctx->aborted || st.stop_requested()) {
            return;
        }
        const HWND h = ctx->hwnd;
        const UINT m = ctx->msg_id;
        lk.unlock();
        ::PostMessageW(h, m, 0, 0);
    });
}

bool Preloader::IsDoneLocked() const
{
    const std::lock_guard lock(sink_mutex_);
    return result_.has_value() || error_.has_value();
}

void Preloader::Join()
{
    if (!ctx_) {
        return;
    }
    ctx_->SignalAbort();
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    ctx_.reset();
}

Preloader::AttachResult Preloader::AttachOrApply(HWND hwnd, UINT msg_id)
{
    if (!ctx_) {
        return AttachResult::None;
    }
    if (IsDoneLocked()) {
        Join();
        return AttachResult::AppliedSync;
    }
    {
        const std::lock_guard lk(ctx_->mtx);
        ctx_->hwnd = hwnd;
        ctx_->msg_id = msg_id;
    }
    ctx_->cv.notify_one();
    return AttachResult::AttachedAsync;
}

void Preloader::FinalizeIfDrained(bool taken)
{
    if (taken && thread_.joinable()) {
        // AttachedAsync 経路: worker は PostMessage 直後に return するため即 join できる。
        thread_.join();
        ctx_.reset();
    }
}

template <class Opt>
Opt Preloader::TakeFromSink(Opt& sink)
{
    Opt out;
    {
        const std::lock_guard lock(sink_mutex_);
        out = std::move(sink);
        sink.reset();
    }
    FinalizeIfDrained(out.has_value());
    return out;
}

std::optional<AsyncLoadResult> Preloader::TakeResult()
{
    return TakeFromSink(result_);
}

std::optional<FileLoadError> Preloader::TakeError()
{
    return TakeFromSink(error_);
}
