#include "file_load_service.h"
#include "file_loader.h"
#include "layout.h"
#include "profiler.h"
#include "ui_constants.h"

void FileLoadService::PreloadContext::SignalAbort()
{
    {
        const std::lock_guard lk(mtx);
        aborted = true;
    }
    cv.notify_all();
}

FileLoadService::~FileLoadService()
{
    // jthread は destructor で join するが、worker が cv.wait でブロック中だと
    // デッドロックするため、明示的に abort 通知してから抜ける。
    if (preload_ctx_) {
        preload_ctx_->SignalAbort();
    }
}

void FileLoadService::StartLoading(std::pmr::wstring path)
{
    loading_path_ = std::move(path);
    BeginLoadingAnimation();
    async_in_flight_ = true;
}

void FileLoadService::BeginLoadingAnimation() noexcept
{
    loading_ = true;
    loading_angle_ = 0.0f;
}

void FileLoadService::StopLoading() noexcept
{
    loading_ = false;
    async_in_flight_ = false;
}

void FileLoadService::TickLoadingAnimation() noexcept
{
    loading_angle_ += spinner::ROTATION_INCREMENT;
    if (loading_angle_ > TWO_PI) {
        loading_angle_ -= TWO_PI;
    }
}

std::expected<void, FileLoadError> FileLoadService::ExecuteLoad(Document& doc, LayoutCache& cache)
{
    StopLoading();

    auto result = doc_service_.LoadFile(loading_path_);
    if (!result) {
        return std::unexpected(result.error());
    }
    doc = std::move(*result);
    cache.Reset(doc.GetNodes().size());
    return {};
}

void FileLoadService::ResetAsyncState()
{
    const std::lock_guard lock(async_mutex_);
    async_result_.reset();
    async_error_.reset();
}

void FileLoadService::StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme)
{
    ResetAsyncState();
    async_in_flight_ = true;
    const uint32_t gen = async_gen_.fetch_add(1, std::memory_order_relaxed) + 1;

    // ワーカースレッドは loading_path_ に触らないため、capture コピーで安全。
    // theme も値コピー: UI スレッドの SetTheme は std::wstring メンバ (font_family など) を
    // 非アトミックに書き換えるので、参照キャプチャだと worker の EstimateNodeHeights が
    // 文字列リードと同時にレースする。
    scheduler.Post([this, path = loading_path_, hwnd, msg_id, gen, theme] {
        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        auto load_result = FileLoader::LoadFile(path);
        if (!load_result) {
            if (async_gen_.load(std::memory_order_relaxed) == gen) {
                {
                    const std::lock_guard lock(async_mutex_);
                    async_error_ = load_result.error();
                }
                ::PostMessageW(hwnd, msg_id, 0, 0);
            }
            return;
        }

        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        Document doc = Document::FromMarkdown(std::move(load_result->text), load_result->byte_size, path);

        if (async_gen_.load(std::memory_order_relaxed) != gen) {
            return;
        }

        LayoutCache cache;
        cache.Reset(doc.GetNodes().size(), /* shrink = */ false);
        EstimateNodeHeights(doc.GetNodes(), cache, theme);

        {
            const std::lock_guard lock(async_mutex_);
            async_result_.emplace(AsyncLoadResult{ std::move(doc), std::move(cache), /* heights_estimated = */ true });
        }

        ::PostMessageW(hwnd, msg_id, 0, 0);
    });
}

std::optional<AsyncLoadResult> FileLoadService::TakeAsyncResult()
{
    const std::lock_guard lock(async_mutex_);
    auto result = std::move(async_result_);
    async_result_.reset();
    return result;
}

std::optional<FileLoadError> FileLoadService::TakeAsyncError() noexcept
{
    const std::lock_guard lock(async_mutex_);
    auto err = async_error_;
    async_error_.reset();
    return err;
}

void FileLoadService::StartPreloadAsync(std::pmr::wstring path)
{
    ResetAsyncState();
    async_in_flight_ = true;
    loading_path_ = path;

    auto ctx = std::make_shared<PreloadContext>();
    preload_ctx_ = ctx;

    // EstimateNodeHeights は Theme 依存のためここでは行わず、OnParseComplete 経由で
    // FinishLoadMarkdownFile が後で実行する。
    preload_thread_ = std::jthread([this, ctx, path = std::move(path)](std::stop_token st) mutable {
        MENDO_PROFILE("Preload::worker");

        auto load_result = DocumentService::LoadFile(path);
        if (load_result) {
            LayoutCache cache;
            cache.Reset(load_result->GetNodes().size(), /* shrink = */ false);
            const std::lock_guard lock(async_mutex_);
            async_result_.emplace(AsyncLoadResult{ std::move(*load_result), std::move(cache), /* heights_estimated = */ false });
        }
        else {
            const std::lock_guard lock(async_mutex_);
            async_error_ = load_result.error();
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

void FileLoadService::OnInitComplete(HWND hwnd, UINT msg_id)
{
    if (!preload_ctx_) {
        return;
    }
    {
        const std::lock_guard lk(preload_ctx_->mtx);
        preload_ctx_->hwnd = hwnd;
        preload_ctx_->msg_id = msg_id;
    }
    preload_ctx_->cv.notify_one();
}

bool FileLoadService::IsPreloadDone()
{
    if (!preload_ctx_) {
        return false;
    }
    const std::lock_guard lock(async_mutex_);
    return async_result_.has_value() || async_error_.has_value();
}

void FileLoadService::JoinPreload()
{
    if (!preload_ctx_) {
        return;
    }
    preload_ctx_->SignalAbort();
    if (preload_thread_.joinable()) {
        preload_thread_.join();
    }
    preload_ctx_.reset();
}

FileLoadService::PreloadAttachResult FileLoadService::AttachOrApplyPreload(HWND hwnd, UINT msg_id)
{
    if (!preload_ctx_) {
        return PreloadAttachResult::None;
    }
    if (IsPreloadDone()) {
        JoinPreload();
        return PreloadAttachResult::AppliedSync;
    }
    OnInitComplete(hwnd, msg_id);
    return PreloadAttachResult::AttachedAsync;
}
