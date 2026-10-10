#include "preloader.h"
#include "document_service.h"
#include "layout_computer.h"
#include "profiler.h"
#include <utility>

Preloader::~Preloader()
{
    Join();
}

void Preloader::Start(std::pmr::wstring path, Theme theme)
{
    // 二重呼び出し防御: 前回 worker を回収し ctx_ も差し替える。
    Join();

    auto ctx = std::make_shared<Context>();
    ctx_ = ctx;

    thread_ = std::jthread([this, ctx, path = std::move(path), theme = std::move(theme)](std::stop_token st) mutable {
        MENDO_PROFILE("Preload::worker");

        // stop_token なしだと Parse 完走まで終了時の join が数百 ms ブロックする。
        auto load_result = DocumentService::LoadFile(path, st);
        if (st.stop_requested()) {
            return;
        }
        LayoutCache cache;
        if (load_result) {
            cache = mendo::layout::MakeEstimatedLayoutCache(load_result->GetNodes(), theme, st);
        }
        {
            const std::lock_guard lock(sink_mutex_);
            // Cancel の sink クリアと同一 mutex 上で直列化し、クリア後の再 publish を防ぐ
            // (coordinator の try_publish と同じパターン)。
            if (st.stop_requested()) {
                return;
            }
            if (load_result) {
                result_.emplace(AsyncLoadResult{ std::move(*load_result), std::move(cache) });
            }
            else {
                error_ = load_result.error();
            }
        }

        std::unique_lock lk(ctx->mtx);
        ctx->cv.wait(lk, st, [&] { return ctx->hwnd != nullptr; });
        if (st.stop_requested()) {
            return;
        }
        const HWND h = ctx->hwnd;
        const UINT m = ctx->msg_id;
        lk.unlock();
        ::PostMessageW(h, m, 0, 0);
    });
}

bool Preloader::HasPublished() const
{
    const std::lock_guard lock(sink_mutex_);
    return result_.has_value() || error_.has_value();
}

void Preloader::Join()
{
    // Cancel 済み (ctx_ が null) でも worker が走行中のことがあるため、
    // joinable のみで判定する。
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
    if (HasPublished()) {
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

template <class Opt>
Opt Preloader::TakeFromSink(Opt& sink)
{
    Opt out;
    {
        const std::lock_guard lock(sink_mutex_);
        out = std::exchange(sink, std::nullopt);
    }
    // Attach 前に取り出されると worker は hwnd 待ちのままなので、stop で起こしてから join する
    // (結果は取り出し済みなので PostMessage は不要)。ctx_ も解放して IsActive() を false にする。
    if (out) {
        Join();
    }
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

void Preloader::Cancel() noexcept
{
    // join しない: worker が Parse 中だと UI スレッドが数百 ms ブロックするため。
    // 以後の publish は worker 側の lock 内 stop 確認で弾かれ、走行中スレッドは
    // 次の Start() か破棄時の Join() が回収する。
    if (ctx_) {
        thread_.request_stop();
        ctx_.reset();
    }
    const std::lock_guard lock(sink_mutex_);
    result_.reset();
    error_.reset();
}
